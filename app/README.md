# RED-V Zephyr Learning App

A multi-task Zephyr RTOS application for the SparkFun RED-V RedBoard (SiFive FE310-G002)
that demonstrates core RTOS concepts.

## What This App Does

Seven threads run simultaneously:

```
[blink_task]     ── toggles LED every 500ms (heartbeat)

[counter_task]   ── produces count ──> [MESSAGE QUEUE] ──> [printer_task] ── prints to UART

[k_timer ISR]    ── fires every 5s, gives semaphore ──> [heartbeat_task] ── prints uptime

[mutex_lo_task]  ── locks mutex, does slow work ──┐
[mutex_med_task] ── runs independently            ├── priority inheritance demo
[mutex_hi_task]  ── locks mutex, reads data    ───┘
```

## Thread Priority Map

```
Priority 3: mutex_hi_task    ← highest (urgent reader)
Priority 4: mutex_med_task
Priority 5: printer_task
Priority 6: counter_task, heartbeat_task
Priority 7: blink_task
Priority 8: mutex_lo_task    ← lowest (slow worker)
```

## Serial Output (115200 baud)

```
[blink] LED heartbeat started (drift-free, 500 ms period)
[counter] Started — sending counts to queue (drift-free)
[printer] Waiting for messages...
[heartbeat] Starting 5-second timer
[printer] Got msg from 'counter': count=1 time=1054 ms
[mutex-lo] Started (prio 8)
[mutex-med] Started (prio 4)
[mutex-hi] Started (prio 3)
[mutex-lo] Locking mutex...
[mutex-lo] Got lock! effective_prio=8 (base=8)
[mutex-lo] Working (holding lock for 500ms)...
[mutex-hi] Need shared data — locking mutex...
[mutex-lo] Releasing mutex (effective_prio=3)          ← BOOSTED 8→3!
[mutex-hi] Got lock! waited 409 ms
[heartbeat] #1 — uptime: 5 seconds
...
```

---

## Zephyr Concepts Explained

### 1. Threads (`K_THREAD_DEFINE`)

A thread is an independent function that runs "at the same time" as other threads.
Zephyr's scheduler switches between them. Each thread needs:

- **Stack**: private memory for local variables and function calls. 512 bytes is
  enough for simple tasks. Too little = stack overflow crash.
- **Priority**: decides which thread runs first. Lower number = higher priority.
- **Entry function**: a function with a `while(1)` loop that never returns.

```c
K_THREAD_DEFINE(blink_tid,     // thread ID (to reference it later)
                512,            // stack size in bytes
                blink_task,     // entry function
                NULL, NULL, NULL, // optional parameters (p1, p2, p3)
                7,              // priority (lower = more important)
                0,              // options (0 = no special flags)
                0);             // delay (0 = start immediately at boot)
```

No `main()` function is needed. `K_THREAD_DEFINE` creates threads at compile
time and Zephyr starts them automatically at boot.

---

### 2. Message Queue (`K_MSGQ_DEFINE`)

A thread-safe FIFO (first-in, first-out) buffer for sending structured data
between threads. One thread produces messages, another consumes them.

```c
struct app_msg {
    uint32_t count;
    uint32_t timestamp_ms;
    char     source[12];
};

// Create a queue that holds up to 4 messages
K_MSGQ_DEFINE(app_msgq, sizeof(struct app_msg), 4, 4);
```

**Producer** (counter_task):
```c
k_msgq_put(&app_msgq, &msg, K_MSEC(100));
//                            ^^^^^^^^^^
//                            Wait up to 100ms if queue is full.
//                            K_NO_WAIT = drop immediately.
//                            K_FOREVER = block until space available.
```

**Consumer** (printer_task):
```c
k_msgq_get(&app_msgq, &msg, K_FOREVER);
//                            ^^^^^^^^^
//                            Block until a message arrives.
//                            Thread sleeps here using ZERO CPU.
```

---

### 3. Semaphore (`K_SEM_DEFINE`)

A simple counter used for signaling. One side "gives" (increments), the other
side "takes" (decrements). If the count is 0, the taker blocks until someone gives.

```c
K_SEM_DEFINE(heartbeat_sem, 0, 1);
//                           ^  ^
//                           |  max count (1 = binary semaphore)
//                           initial count (0 = starts locked)
```

**Two operations:**
- `k_sem_give(&heartbeat_sem)` -- signal (safe in ISR)
- `k_sem_take(&heartbeat_sem, K_FOREVER)` -- wait for signal

Think of it like a doorbell: the timer ISR **rings** it (give), and the
heartbeat thread **waits** for it (take).

---

### 4. Timer (`K_TIMER_DEFINE`)

Fires a callback function periodically, managed by the kernel. The callback
runs in **interrupt context (ISR)** -- not in a thread.

```c
// Timer callback (runs as ISR -- must be fast!)
static void heartbeat_timer_cb(struct k_timer *timer)
{
    k_sem_give(&heartbeat_sem);  // Just signal, don't do slow work
}

K_TIMER_DEFINE(heartbeat_timer, heartbeat_timer_cb, NULL);

// Start: first fire in 5s, repeat every 5s
k_timer_start(&heartbeat_timer, K_SECONDS(5), K_SECONDS(5));
```

**ISR rules:**

| Allowed in ISR | NOT allowed in ISR |
|---|---|
| `k_sem_give()` | `k_msleep()` |
| `k_msgq_put()` with `K_NO_WAIT` | `printk()` |
| Set a global flag | Any blocking call |
| Short calculations | Slow I/O (UART, I2C, SPI) |

---

### 5. Drift-Free Timing (`K_TIMEOUT_ABS_MS`)

`k_msleep(N)` drifts because it sleeps N ms **from now**, not from a fixed
schedule. Work time accumulates.

```c
// BAD: drifts over time
while (1) {
    do_work();      // takes 3ms
    k_msleep(100);  // actual period = 103ms
}

// GOOD: drift-free absolute deadline
int64_t next = k_uptime_get() + 100;
while (1) {
    do_work();
    k_sleep(K_TIMEOUT_ABS_MS(next));  // sleep until absolute time
    next += 100;                       // always advance by fixed period
}
```

Why it recovers from overruns: `next += period` always advances from the
**original timeline**, not from "now." If one iteration takes too long,
the next deadline catches up.

| Method | Drift | Recovers from overrun |
|---|---|---|
| `k_msleep(period)` | Yes | No |
| `K_TIMEOUT_ABS_MS(next)` | No | Yes |
| `k_timer` + semaphore | No | Yes |

---

### 6. Mutex (`K_MUTEX_DEFINE`) and Priority Inheritance

A mutex (mutual exclusion) protects shared data so only one thread can
access it at a time.

```c
K_MUTEX_DEFINE(resource_mutex);

// Thread A                        // Thread B
k_mutex_lock(&resource_mutex,      k_mutex_lock(&resource_mutex,
             K_FOREVER);                        K_FOREVER);
shared_data++;  // safe            shared_data += 10;  // waits
k_mutex_unlock(&resource_mutex);   k_mutex_unlock(&resource_mutex);
```

**Mutex vs Semaphore:**

| | Mutex (`k_mutex`) | Semaphore (`k_sem`) |
|---|---|---|
| **Purpose** | Protect shared data | Signal/notify |
| **Who locks/unlocks** | Same thread | Different threads |
| **ISR safe** | No | `k_sem_give()` yes |
| **Ownership** | Lock owner tracked | No owner |
| **Priority inheritance** | Yes | No |
| **Recursive locking** | Yes (same thread) | No |

**The Priority Inversion Problem:**

Without priority inheritance, this sequence causes a bug:
```
1. LOW (prio 8) locks mutex, starts slow work
2. HIGH (prio 3) needs mutex — blocks waiting
3. MEDIUM (prio 4) wakes up, preempts LOW (4 < 8)
4. HIGH waits for LOW, but MEDIUM keeps running
   → HIGH (most important) stuck behind MEDIUM (less important)!
```

This bug crashed the Mars Pathfinder rover in 1997.

**Priority Inheritance Solution (Zephyr does this automatically):**
```
1. LOW (prio 8) locks mutex, starts slow work
2. HIGH (prio 3) needs mutex — blocks waiting
3. Zephyr BOOSTS LOW's priority: 8 → 3
4. MEDIUM (prio 4) can't preempt LOW (3 < 4)
5. LOW finishes quickly, releases mutex
6. HIGH runs immediately, LOW returns to prio 8
```

**What we see in our demo:**
```
[mutex-lo] Got lock! effective_prio=8 (base=8)       ← normal
[mutex-hi] Need shared data — locking mutex...        ← HIGH blocks
[mutex-lo] Releasing mutex (effective_prio=3)          ← BOOSTED 8→3!
[mutex-hi] Got lock! waited 409 ms                     ← HIGH gets it fast
```

---

### 7. Sleep (`k_msleep`)

```c
k_msleep(500);  // Sleep for 500 milliseconds
```

Puts the current thread to sleep. During this time:
- The thread uses **zero CPU**
- Other threads get to run
- The kernel wakes this thread after the specified time

---

### 8. Uptime (`k_uptime_get`)

```c
uint32_t uptime_ms = (uint32_t)k_uptime_get();  // milliseconds since boot
```

Returns milliseconds since the system booted. Driven by the FE310's `mtime`
hardware counter running at 32.768 kHz.

---

## Quick Reference

| Zephyr Concept | What It Does | Create With | Key Operations |
|---|---|---|---|
| **Thread** | Independent concurrent function | `K_THREAD_DEFINE` | Runs automatically |
| **Message Queue** | Send data between threads | `K_MSGQ_DEFINE` | `k_msgq_put` / `k_msgq_get` |
| **Semaphore** | Signal between ISR and thread | `K_SEM_DEFINE` | `k_sem_give` / `k_sem_take` |
| **Timer** | Periodic ISR callback | `K_TIMER_DEFINE` | `k_timer_start` / `k_timer_stop` |
| **Mutex** | Protect shared data | `K_MUTEX_DEFINE` | `k_mutex_lock` / `k_mutex_unlock` |
| **Drift-free sleep** | Precise periodic timing | -- | `K_TIMEOUT_ABS_MS(next)` |
| **Sleep** | Yield CPU for a duration | -- | `k_msleep(ms)` |
| **Uptime** | Time since boot | -- | `k_uptime_get()` |

---

## RED-V Specific Findings

Issues discovered while developing on the SparkFun RED-V RedBoard:

### Clock Mismatch
The FE310's `mtime` counter runs at **32.768 kHz** (RTC oscillator), not 16 MHz.
The default Zephyr config is wrong. Fix in `prj.conf`:
```
CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=32768
```
Without this, `k_msleep()` and `k_busy_wait()` wait ~488x longer than expected.

### LED Pin Conflict
The built-in LED is on GPIO 5, shared with SPI1/SPI2 SCK via IOF0.
Must disable SPI1 and SPI2 in the device tree overlay.

### I2C Driver Bug
The SiFive I2C driver (`i2c_sifive.c`) does **not** apply pinctrl during init.
GPIO 12 (SDA) and GPIO 13 (SCL) must have IOF0 enabled manually:
```c
*(volatile uint32_t *)(0x1001203C) &= ~(BIT(12) | BIT(13)); // IOF_SEL = 0
*(volatile uint32_t *)(0x10012038) |=  (BIT(12) | BIT(13)); // IOF_EN = 1
```

### Pin Labeling
The RED-V silkscreen uses **Arduino pin numbers**, not FE310 GPIO numbers:
- Header "13" = Arduino D13 = FE310 GPIO 5 (LED)
- Header "SDA/18" = Arduino D18/A4 = FE310 GPIO 12
- Header "SCL/19" = Arduino D19/A5 = FE310 GPIO 13

---

## Build and Flash

```bash
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.0_windows-x86_64_minimal/zephyr-sdk-1.0.0"
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"
cd ~/zephyrproject/zephyr
source zephyr-env.sh

# Build
west build -b hifive1_revb /path/to/RED_V_Zephyr/app

# Flash
west flash
```

## Hardware

- **Board**: SparkFun RED-V RedBoard (SiFive FE310-G002)
- **LED**: Built-in blue LED on GPIO 5 (active high)
- **Serial**: 115200 baud, 8N1
- **Qwiic I2C**: GPIO 12 (SDA), GPIO 13 (SCL) -- labeled "SDA/18", "SCL/19"

## Project Structure

```
app/
├── CMakeLists.txt              # Build config
├── prj.conf                    # Zephyr kernel config
├── README.md                   # This file
├── boards/
│   └── hifive1_revb.overlay    # Disable SPI1/SPI2 for LED
└── src/
    └── main.c                  # All 7 tasks + Zephyr concepts
```
