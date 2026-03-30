# RED-V Zephyr Learning App

A multi-task Zephyr RTOS application for the SparkFun RED-V RedBoard (SiFive FE310-G002)
that demonstrates core RTOS concepts and reads data from I2C/SPI sensors.

## What This App Does

Up to 11 threads run simultaneously:

```
RTOS demos (commented out by default to save RAM):
  [blink_task]     ── toggles LED every 500ms (drift-free timing)
  [counter_task]   ── produces count ──> [MESSAGE QUEUE] ──> [printer_task]
  [k_timer ISR]    ── fires every 5s, gives semaphore ──> [heartbeat_task]
  [mutex_lo/med/hi]── priority inheritance demo (3 threads)
  [workq_task]     ── deferred work from ISR via system workqueue

I2C sensors (active):
  [shtc3_task]     ── reads temperature + humidity every 2s
  [opt4048_task]   ── reads color (CIE XYZ) + lux every 2s

SPI sensor (active):
  [bno085_task]    ── reads quaternion + accelerometer from IMU
```

## Print Group Filter

All output goes through `TPRINTK(group, ...)` which filters by group.
Change `active_groups` in main.c to select what prints:

```c
active_groups = PG_I2C;             // Only sensor tasks
active_groups = PG_MUTEX;           // Only mutex demo
active_groups = PG_MSGQ;            // Only message queue
active_groups = PG_RTOS;            // All RTOS demos
active_groups = PG_ALL;             // Everything
active_groups = PG_I2C | PG_MUTEX;  // Combine groups
```

Available groups:
| Group | What it shows |
|---|---|
| `PG_THREAD` | Thread creation |
| `PG_MSGQ` | Message queue (counter + printer) |
| `PG_SEM` | Semaphore (heartbeat) |
| `PG_TIMER` | Timer callbacks |
| `PG_MUTEX` | Mutex + priority inheritance |
| `PG_WORKQ` | Workqueue demo |
| `PG_SLEEP` | Drift-free timing |
| `PG_I2C` | I2C sensor tasks |

---

## FE310 I2C Hardware Bugs (Discovered via Saleae Logic Analyzer)

Three critical bugs in the SiFive FE310 I2C implementation:

### Bug 1: Pinctrl Not Applied
The Zephyr SiFive I2C driver does NOT configure IOF (I/O Function) pins during
init. GPIO 12 (SDA) and GPIO 13 (SCL) remain in GPIO mode.

**Workaround:** Manually set IOF_EN and clear IOF_SEL for bits 12/13:
```c
volatile uint32_t *iof_sel = (volatile uint32_t *)(0x10012000 + 0x3C);
volatile uint32_t *iof_en  = (volatile uint32_t *)(0x10012000 + 0x38);
*iof_sel &= ~(BIT(12) | BIT(13));
*iof_en  |= (BIT(12) | BIT(13));
```

### Bug 2: I2C Address Not Left-Shifted
The driver sends the 7-bit address as-is on the wire, without left-shifting it.
Saleae confirmed: passing `0x70` puts `0x70` on the wire, decoded as address `0x38`.

**Workaround:** Pass `addr << 1` for all I2C API calls:
```c
#define I2C_SHIFTED(addr7)  ((addr7) << 1)
i2c_read(dev, buf, len, I2C_SHIFTED(0x70));  // Correct: 0xE0 on wire = addr 0x70
```

### Bug 3: Controller State Corruption
The OpenCores I2C controller hangs after ~5 transactions with a stuck START
condition (START issued but no address follows). Confirmed via Saleae capture.

**Workaround:** Full controller re-init before each transaction:
```c
static void i2c_reinit(void) {
    volatile uint8_t *base = (volatile uint8_t *)0x10016000;
    base[0x08] = 0x00;           // Disable controller
    k_busy_wait(10);
    base[0x00] = 0x07;           // Prescaler for 400kHz
    base[0x04] = 0x00;
    base[0x08] = 0x80;           // Re-enable
    k_busy_wait(10);
}
```

### BNO085 Clock Stretching Issue
The BNO085 IMU stretches the I2C clock for ~1.7 seconds during SHTP communication.
The FE310's polled I2C driver busy-waits the entire time, blocking all other I2C
devices. **Solution: Use SPI for the BNO085** (no clock stretching with SPI).

---

## RED-V Board Pin Mapping

The RED-V silkscreen uses **mixed numbering** (Arduino + FE310 GPIO):

### I2C Pins (for SHTC3, OPT4048)
```
Header label "SDA/18" (top-right) = FE310 GPIO 12 = I2C0 SDA
Header label "SCL/19" (top-right) = FE310 GPIO 13 = I2C0 SCL
```
Note: The Qwiic connector is on DIFFERENT traces (not GPIO 12/13).
Use the header pins labeled SDA/18 and SCL/19 at the top-right for I2C.

### SPI Pins (for BNO085)
```
Header D10 = GPIO 2  = SPI1 CS0
Header D11 = GPIO 3  = SPI1 MOSI
Header D12 = GPIO 4  = SPI1 MISO
Header D13 = GPIO 5  = SPI1 SCK (shared with LED!)
```

### GPIO IOF Mapping (from FE310-G002 Manual, Table 51)
```
GPIO  IOF0           IOF1
 2    SPI1_CS0       PWM0_PWM2
 3    SPI1_MOSI      PWM0_PWM3
 4    SPI1_MISO
 5    SPI1_SCK
12    I2C0_SDA       PWM2_PWM2
13    I2C0_SCL       PWM2_PWM3
16    UART0_RX
17    UART0_TX
18    UART1_TX
23    UART1_RX
```

---

## Sensor Wiring

### SHTC3 (Temperature + Humidity) — I2C
```
RED-V          SHTC3 Breakout
SDA/18  ────── SDA
SCL/19  ────── SCL
3.3V    ────── VIN
GND     ────── GND
```
I2C address: 0x70

### OPT4048 (Color/Lux) — I2C
```
RED-V          OPT4048 Breakout
SDA/18  ────── SDA
SCL/19  ────── SCL
3.3V    ────── VIN
GND     ────── GND
```
I2C address: 0x44

### BNO085 (IMU) — SPI
```
RED-V          BNO085 Breakout
D10 (GPIO 2)── CS
D11 (GPIO 3)── SDA/SDI (MOSI)
D12 (GPIO 4)── SDO/ADO (MISO)
D13 (GPIO 5)── SCL/SCK
3.3V        ── VIN
GND         ── GND
PS0         ── 3.3V (SPI mode)
PS1         ── GND  (SPI mode)
```
Note: LED is disabled when SPI1 is active (GPIO 5 = SCK).

---

## Zephyr Concepts Demonstrated

| Concept | Zephyr API | What it teaches |
|---|---|---|
| Threads | `K_THREAD_DEFINE` | Concurrent tasks, priorities, scheduling |
| Message Queue | `K_MSGQ_DEFINE` | Producer-consumer, inter-thread data passing |
| Semaphore | `K_SEM_DEFINE` | ISR-to-thread signaling |
| Timer | `K_TIMER_DEFINE` | Periodic callbacks in ISR context |
| Mutex | `K_MUTEX_DEFINE` | Shared resource protection, priority inheritance |
| Workqueue | `K_WORK_DEFINE` | Deferred work from ISR to thread context |
| Drift-free timing | `K_TIMEOUT_ABS_MS` | Absolute deadline scheduling |
| I2C bus sharing | `K_MUTEX_DEFINE` | Multiple tasks sharing a peripheral |
| Print filtering | `TPRINTK` macro | Runtime debug output control |

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
- **LED**: GPIO 5 (disabled when SPI1 active for BNO085)
- **Serial**: 115200 baud, 8N1
- **I2C**: GPIO 12 (SDA), GPIO 13 (SCL) at 400 kHz — header pins "SDA/18", "SCL/19"
- **SPI**: GPIO 2 (CS), 3 (MOSI), 4 (MISO), 5 (SCK) at 3 MHz
- **Clock fix**: `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=32768` (FE310 mtime = 32.768 kHz)

## Project Structure

```
app/
├── CMakeLists.txt              # Build config (includes SH-2 library)
├── prj.conf                    # Zephyr kernel config (I2C, SPI, GPIO)
├── README.md                   # This file
├── boards/
│   └── hifive1_revb.overlay    # SPI1 enabled, SPI2 disabled
├── lib/
│   └── sh2/                    # CEVA SH-2 library (BNO085 SHTP protocol)
└── src/
    ├── main.c                  # RTOS demos + I2C sensor tasks + print filter
    ├── bno085_task.c           # BNO085 IMU task (SPI + SH-2 library)
    └── bno085_task.h           # BNO085 task header
```
