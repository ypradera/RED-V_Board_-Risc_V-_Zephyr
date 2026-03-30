/*
 * RED-V Zephyr Learning App
 *
 * Demonstrates key Zephyr RTOS concepts:
 *
 *   1. K_THREAD_DEFINE  — creating threads at compile time
 *   2. K_MSGQ_DEFINE    — message queue for inter-thread communication
 *   3. k_timer          — periodic timer with callback (no thread needed)
 *   4. k_sem            — semaphore for synchronization
 *   5. k_msleep         — putting a thread to sleep
 *   6. k_mutex          — mutual exclusion with priority inheritance
 *   7. k_work           — workqueues (defer work from ISR to thread)
 *
 * Architecture:
 *
 *   [blink_task] ──toggles LED every 500ms──
 *
 *   [counter_task] ──produces count──> [MESSAGE QUEUE] ──> [printer_task] ──prints to UART──
 *
 *   [k_timer callback] ──fires every 5 seconds, gives semaphore──> [heartbeat_task] ──prints uptime──
 *
 *   [k_timer ISR] ──submits k_work──> [SYSTEM WORKQUEUE] ──processes work item──> prints to UART
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>

/* ========== Print Group Filter ==========
 *
 * Each task belongs to a "print group." Only groups that are enabled
 * in active_groups will produce output. Change active_groups at
 * compile time or at runtime to select which tasks print.
 *
 * Examples:
 *   active_groups = PG_I2C;              // Only I2C tasks
 *   active_groups = PG_RTOS;             // Only RTOS demo tasks
 *   active_groups = PG_I2C | PG_RTOS;    // Both
 *   active_groups = PG_ALL;              // Everything
 *   active_groups = 0;                   // Silent
 */
/* Individual RTOS primitive groups */
#define PG_THREAD    BIT(0)   /* K_THREAD_DEFINE — basic threading */
#define PG_MSGQ      BIT(1)   /* K_MSGQ — message queue (producer/consumer) */
#define PG_SEM       BIT(2)   /* K_SEM — semaphore (ISR-to-thread signaling) */
#define PG_TIMER     BIT(3)   /* K_TIMER — periodic timer callbacks */
#define PG_MUTEX     BIT(4)   /* K_MUTEX — mutex + priority inheritance */
#define PG_WORKQ     BIT(5)   /* K_WORK — workqueue (deferred ISR work) */
#define PG_SLEEP     BIT(6)   /* K_TIMEOUT_ABS_MS — drift-free timing */
#define PG_I2C       BIT(7)   /* I2C sensor tasks (OPT4048, etc.) */

/* Convenience groups */
#define PG_RTOS      (PG_THREAD | PG_MSGQ | PG_SEM | PG_TIMER | PG_MUTEX | PG_WORKQ | PG_SLEEP)
#define PG_ALL       0xFFFFFFFF
#define PG_NONE      0

/*
 * >>> CHANGE THIS LINE TO SELECT WHAT PRINTS <<<
 *
 * volatile so it could be changed at runtime via debugger (GDB).
 */
static volatile uint32_t active_groups = PG_I2C;

/* Filtered printk — zero overhead when group is disabled */
#define TPRINTK(group, fmt, ...) \
	do { if (active_groups & (group)) printk(fmt, ##__VA_ARGS__); } while (0)

/* ========== FE310 GPIO (LED on pin 5) ========== */
#define GPIO_BASE       0x10012000
#define GPIO_OUTPUT_EN  0x08
#define GPIO_OUTPUT_VAL 0x0C
#define GPIO_IOF_EN     0x38
#define LED_PIN         5

static volatile uint32_t *gpio_reg(uint32_t offset)
{
	return (volatile uint32_t *)(GPIO_BASE + offset);
}

/* ========== Thread stack sizes and priorities ========== */

/*
 * Lower priority number = higher priority.
 * printer_task has highest priority so it processes messages quickly.
 * blink_task has lowest priority since it's just a heartbeat.
 */
#define BLINK_STACK     512
#define BLINK_PRIO      7

#define COUNTER_STACK   512
#define COUNTER_PRIO    6

#define PRINTER_STACK   512
#define PRINTER_PRIO    5

#define HEARTBEAT_STACK 512
#define HEARTBEAT_PRIO  6

/*
 * Mutex demo tasks: three threads at different priorities
 * competing for a shared resource.
 */
#define MUTEX_HI_STACK   512
#define MUTEX_HI_PRIO    3   /* Highest priority */

#define MUTEX_MED_STACK  512
#define MUTEX_MED_PRIO   4   /* Medium priority */

#define MUTEX_LO_STACK   512
#define MUTEX_LO_PRIO    8   /* Lowest priority */


/* ========== Message Queue ==========
 *
 * A message queue lets one thread send structured data to another.
 * The producer (counter_task) puts messages in, the consumer
 * (printer_task) takes them out. If the queue is full, the producer
 * can wait or drop the message. If the queue is empty, the consumer
 * blocks until data arrives.
 */

/* The message structure — sent from counter_task to printer_task */
struct app_msg {
	uint32_t count;         /* incrementing counter */
	uint32_t timestamp_ms;  /* when the message was created */
	char     source[12];    /* which task sent it */
};

/*
 * K_MSGQ_DEFINE(name, message_size, max_messages, alignment)
 *
 * Creates a message queue that holds up to 4 app_msg structs.
 * Memory is allocated at compile time (no malloc needed).
 */
K_MSGQ_DEFINE(app_msgq, sizeof(struct app_msg), 4, 4);

/* ========== Semaphore ==========
 *
 * A semaphore is a signaling mechanism. One thread (or ISR/timer)
 * "gives" the semaphore, another thread "takes" it (blocks until given).
 *
 * K_SEM_DEFINE(name, initial_count, max_count)
 *   initial_count = 0: starts locked (taker will block)
 *   max_count = 1: binary semaphore (like a flag)
 */
K_SEM_DEFINE(heartbeat_sem, 0, 1);

/* ========== Timer ==========
 *
 * A k_timer fires a callback at a set interval WITHOUT needing its
 * own thread. The callback runs in interrupt context (ISR), so it
 * must be fast and cannot call blocking functions like k_msleep().
 *
 * We use it to give a semaphore, which wakes up the heartbeat_task.
 * This is a common RTOS pattern: "defer work from ISR to thread."
 */
static void heartbeat_timer_cb(struct k_timer *timer)
{
	/* This runs in ISR context — keep it fast!
	 * Just signal the semaphore to wake the heartbeat thread. */
	k_sem_give(&heartbeat_sem);
}

/* Define the timer (not started yet — we start it in heartbeat_task) */
K_TIMER_DEFINE(heartbeat_timer, heartbeat_timer_cb, NULL);

/* ========== Task 1: LED Blink (heartbeat) ==========
 *
 * Uses K_TIMEOUT_ABS_MS for drift-free periodic timing.
 * Instead of "sleep for 500ms from now" (which drifts),
 * we sleep until an absolute deadline and advance it by
 * a fixed period each iteration.
 */
#define BLINK_PERIOD_MS 500

void blink_task(void *p1, void *p2, void *p3)
{
	/* Release GPIO 5 from SPI IOF, enable as output */
	*gpio_reg(GPIO_IOF_EN) &= ~BIT(LED_PIN);
	*gpio_reg(GPIO_OUTPUT_EN) |= BIT(LED_PIN);

	TPRINTK(PG_THREAD | PG_SLEEP, "[blink] LED heartbeat started (drift-free, %d ms)\n",
		BLINK_PERIOD_MS);

	/*
	 * Set the first deadline: "now + period"
	 * Each iteration advances by exactly BLINK_PERIOD_MS,
	 * anchored to the original timeline, not to "now."
	 */
	int64_t next = k_uptime_get() + BLINK_PERIOD_MS;

	while (1) {
		*gpio_reg(GPIO_OUTPUT_VAL) ^= BIT(LED_PIN);

		/*
		 * k_sleep(K_TIMEOUT_ABS_MS(next)):
		 *   Sleep until absolute time 'next', not "for N ms."
		 *   If we're already past the deadline (work took too long),
		 *   this returns immediately and we catch up next iteration.
		 *
		 * next += BLINK_PERIOD_MS:
		 *   Always advance from the original timeline.
		 *   This is what prevents drift — the deadline is never
		 *   based on "now", only on the fixed schedule.
		 */
		k_sleep(K_TIMEOUT_ABS_MS(next));
		next += BLINK_PERIOD_MS;
	}
}

/* ========== Task 2: Counter (producer) ==========
 *
 * Counts up every 1 second and sends the count to the message queue.
 * Uses K_TIMEOUT_ABS_MS for drift-free 1-second intervals.
 */
#define COUNTER_PERIOD_MS 1000

void counter_task(void *p1, void *p2, void *p3)
{
	TPRINTK(PG_MSGQ | PG_SLEEP, "[counter] Started — sending counts to queue (drift-free)\n");

	uint32_t count = 0;
	int64_t next = k_uptime_get() + COUNTER_PERIOD_MS;

	while (1) {
		count++;

		/* Build a message */
		struct app_msg msg = {
			.count = count,
			.timestamp_ms = (uint32_t)k_uptime_get(),
		};
		strncpy(msg.source, "counter", sizeof(msg.source));

		/*
		 * k_msgq_put: add message to queue
		 *
		 * K_NO_WAIT: don't block if queue is full (drop the message).
		 * K_FOREVER: block until space is available.
		 * K_MSEC(n): wait up to n milliseconds.
		 */
		int ret = k_msgq_put(&app_msgq, &msg, K_MSEC(100));
		if (ret != 0) {
			TPRINTK(PG_MSGQ, "[counter] Queue full! Dropped count %u\n", count);
		}

		/* Drift-free sleep until next absolute deadline */
		k_sleep(K_TIMEOUT_ABS_MS(next));
		next += COUNTER_PERIOD_MS;
	}
}

/* ========== Task 3: Printer (consumer) ==========
 *
 * Blocks on the message queue, waiting for messages from the
 * counter task. Prints each message to UART as it arrives.
 *
 * This is the "consumer" in a producer-consumer pattern.
 */
void printer_task(void *p1, void *p2, void *p3)
{
	TPRINTK(PG_MSGQ, "[printer] Waiting for messages...\n");

	struct app_msg msg;

	while (1) {
		/*
		 * k_msgq_get: take a message from the queue
		 *
		 * K_FOREVER: block until a message is available.
		 * This thread sleeps here, using zero CPU, until
		 * counter_task puts something in the queue.
		 */
		k_msgq_get(&app_msgq, &msg, K_FOREVER);

		TPRINTK(PG_MSGQ, "[printer] Got msg from '%s': count=%u time=%u ms\n",
			msg.source, msg.count, msg.timestamp_ms);
	}
}

/* ========== Task 4: Heartbeat (timer-driven) ==========
 *
 * Demonstrates the timer + semaphore pattern:
 *   - A k_timer fires every 5 seconds (in ISR context)
 *   - The ISR gives a semaphore
 *   - This thread takes the semaphore and does the "real work"
 *
 * Why not just use k_msleep(5000)?
 *   Because timers are more precise and can be started/stopped
 *   dynamically. Also, this pattern is how you safely defer
 *   work from an interrupt handler to a thread.
 */
void heartbeat_task(void *p1, void *p2, void *p3)
{
	TPRINTK(PG_TIMER, "[heartbeat] Starting 5-second timer\n");

	/*
	 * k_timer_start(timer, initial_delay, repeat_period)
	 *
	 * First fire after 5 seconds, then repeat every 5 seconds.
	 * K_SECONDS() converts seconds to Zephyr's internal ticks.
	 */
	k_timer_start(&heartbeat_timer, K_SECONDS(5), K_SECONDS(5));

	uint32_t beat = 0;

	while (1) {
		/* Block until the timer callback gives the semaphore */
		k_sem_take(&heartbeat_sem, K_FOREVER);
		beat++;

		uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000);
		TPRINTK(PG_SEM | PG_TIMER, "[heartbeat] #%u — uptime: %u seconds\n",
		beat, uptime_s);
	}
}


/*
 * ========== Tasks 5-7: Mutex & Priority Inheritance Demo ==========
 *
 * THE PROBLEM: "Priority Inversion"
 *
 *   Imagine 3 tasks: HIGH, MEDIUM, LOW priority.
 *   LOW takes a lock (mutex) and starts doing work.
 *   HIGH wakes up and needs the same lock — it blocks waiting.
 *   MEDIUM wakes up. It doesn't need the lock, so it runs freely.
 *
 *   Result: HIGH is stuck waiting for LOW, but MEDIUM keeps
 *   preempting LOW. HIGH (most important) waits for MEDIUM
 *   (less important). This is "priority inversion" — it's a bug
 *   that crashed the Mars Pathfinder rover in 1997!
 *
 * THE SOLUTION: "Priority Inheritance"
 *
 *   When HIGH blocks on a mutex held by LOW, Zephyr temporarily
 *   BOOSTS LOW's priority to match HIGH. Now LOW can't be
 *   preempted by MEDIUM, so LOW finishes quickly, releases the
 *   mutex, and HIGH runs. LOW's priority returns to normal.
 *
 *   Zephyr's k_mutex does this automatically. k_sem does NOT.
 *
 * THIS DEMO:
 *
 *   A shared "sensor reading" is protected by a mutex.
 *   - mutex_lo_task (prio 8): locks mutex, does slow work, unlocks
 *   - mutex_med_task (prio 4): runs independently (no mutex)
 *   - mutex_hi_task (prio 3): locks mutex, reads data, unlocks
 *
 *   Watch the serial output — you'll see priority inheritance
 *   boost LOW when HIGH is waiting.
 */

/*
 * K_MUTEX_DEFINE: creates a mutex at compile time.
 *
 * Unlike a semaphore:
 *   - Only the thread that locked it can unlock it (ownership)
 *   - Supports priority inheritance (prevents priority inversion)
 *   - Can be locked recursively by the same thread
 *   - CANNOT be used in ISR context (use k_sem for that)
 */
K_MUTEX_DEFINE(resource_mutex);

/* Shared resource protected by the mutex */
static struct {
	int32_t  value;
	uint32_t updated_by_prio;
	uint32_t update_count;
} shared_resource;

/*
 * mutex_lo_task: LOW priority (8) — the "slow worker"
 *
 * Locks the mutex, does slow work (simulated with k_msleep),
 * then releases. When mutex_hi_task is waiting, Zephyr boosts
 * this task's priority so it can finish and release the lock.
 */
void mutex_lo_task(void *p1, void *p2, void *p3)
{
	TPRINTK(PG_MUTEX, "[mutex-lo] Started (prio %d)\n", MUTEX_LO_PRIO);

	while (1) {
		TPRINTK(PG_MUTEX, "[mutex-lo] Locking mutex...\n");
		k_mutex_lock(&resource_mutex, K_FOREVER);

		/*
		 * We hold the lock. Check our effective priority —
		 * if a higher-priority thread is waiting, Zephyr
		 * boosts us via priority inheritance.
		 */
		int current_prio = k_thread_priority_get(k_current_get());
		TPRINTK(PG_MUTEX, "[mutex-lo] Got lock! effective_prio=%d (base=%d)%s\n",
			current_prio, MUTEX_LO_PRIO,
			current_prio < MUTEX_LO_PRIO ? " ** BOOSTED **" : "");

		/* Simulate slow work while holding the lock */
		shared_resource.value += 1;
		shared_resource.updated_by_prio = MUTEX_LO_PRIO;
		shared_resource.update_count++;

		TPRINTK(PG_MUTEX, "[mutex-lo] Working (holding lock for 500ms)...\n");
		k_msleep(500);

		/* Check priority again before releasing */
		current_prio = k_thread_priority_get(k_current_get());
		TPRINTK(PG_MUTEX, "[mutex-lo] Releasing mutex (effective_prio=%d)\n",
			current_prio);

		k_mutex_unlock(&resource_mutex);

		/* Sleep before next cycle */
		k_msleep(3000);
	}
}

/*
 * mutex_med_task: MEDIUM priority (4)
 *
 * Does NOT use the mutex. Just runs periodically.
 * Without priority inheritance, this task would preempt
 * mutex_lo_task, causing priority inversion.
 * WITH priority inheritance, it can't preempt mutex_lo_task
 * when LOW is boosted to HIGH's priority level.
 */
void mutex_med_task(void *p1, void *p2, void *p3)
{
	TPRINTK(PG_MUTEX, "[mutex-med] Started (prio %d)\n", MUTEX_MED_PRIO);

	while (1) {
		TPRINTK(PG_MUTEX, "[mutex-med] Running (no mutex needed)\n");
		k_msleep(2000);
	}
}

/*
 * mutex_hi_task: HIGH priority (3) — the "urgent reader"
 *
 * Needs the shared resource. If mutex_lo_task holds the lock,
 * this task blocks — and Zephyr boosts LOW's priority to match
 * ours (priority 3) so LOW finishes quickly.
 */
void mutex_hi_task(void *p1, void *p2, void *p3)
{
	TPRINTK(PG_MUTEX, "[mutex-hi] Started (prio %d)\n", MUTEX_HI_PRIO);

	while (1) {
		/* Wait a bit so LOW gets the lock first */
		k_msleep(2200);

		TPRINTK(PG_MUTEX, "[mutex-hi] Need shared data — locking mutex...\n");
		int64_t wait_start = k_uptime_get();

		k_mutex_lock(&resource_mutex, K_FOREVER);

		int64_t wait_ms = k_uptime_get() - wait_start;
		TPRINTK(PG_MUTEX, "[mutex-hi] Got lock! waited %d ms, "
			"value=%d (updated by prio %d, count=%d)\n",
			(int)wait_ms, shared_resource.value,
			shared_resource.updated_by_prio,
			shared_resource.update_count);

		k_mutex_unlock(&resource_mutex);
	}
}

/*
 * ========== Task 8: Workqueue Demo ==========
 *
 * WHAT IS A WORKQUEUE?
 *
 *   A workqueue is a thread that processes "work items" from a queue.
 *   You submit work items, and the workqueue thread runs them one
 *   at a time in its own thread context.
 *
 * WHY USE IT?
 *
 *   1. DEFER WORK FROM ISR:
 *      An ISR can't call blocking functions (printk, k_msleep, I2C).
 *      Instead, the ISR submits a work item, and the workqueue
 *      thread handles it later — safely in thread context.
 *
 *   2. AVOID CREATING TOO MANY THREADS:
 *      Each thread needs its own stack (512+ bytes of RAM).
 *      With workqueues, many different jobs share ONE thread's stack.
 *
 *   3. DELAYED WORK:
 *      k_work_delayable lets you schedule work to run after a delay,
 *      like a one-shot timer but with thread-context execution.
 *
 * SYSTEM WORKQUEUE vs CUSTOM WORKQUEUE:
 *
 *   Zephyr has a built-in "system workqueue" (k_sys_work_q) that's
 *   always available. You can also create your own with K_WORK_DEFINE.
 *   We use the system workqueue here for simplicity.
 *
 * THIS DEMO:
 *
 *   A timer fires every 3 seconds (ISR context).
 *   The ISR submits two types of work:
 *     - Immediate work (k_work): runs ASAP on the workqueue thread
 *     - Delayed work (k_work_delayable): runs 1 second later
 *
 *   Flow:
 *     [Timer ISR] ──submit──> [System Workqueue Thread]
 *                                    │
 *                              runs work_handler() immediately
 *                              runs delayed_work_handler() after 1s
 */

/* Work item counter */
static volatile uint32_t work_count;

/*
 * k_work: an immediate work item.
 *
 * This handler runs on the system workqueue THREAD (not ISR).
 * It CAN call blocking functions, printk, etc.
 */
static void work_handler(struct k_work *work)
{
	work_count++;
	TPRINTK(PG_WORKQ, "[workq] Immediate work #%u executed (thread context)\n",
		work_count);
}

/*
 * k_work_delayable: a work item that runs after a specified delay.
 *
 * Useful for debouncing buttons, retry logic, or scheduled tasks
 * that don't need their own thread.
 */
static void delayed_work_handler(struct k_work *work)
{
	TPRINTK(PG_WORKQ, "[workq] Delayed work executed (1 second after submit)\n");
}

/* Define the work items (not submitted yet) */
K_WORK_DEFINE(my_work, work_handler);
K_WORK_DELAYABLE_DEFINE(my_delayed_work, delayed_work_handler);

/*
 * Timer that submits work items every 3 seconds.
 * This runs in ISR context — it CANNOT call printk or blocking functions.
 * Instead, it submits work to the system workqueue.
 */
static void workq_timer_cb(struct k_timer *timer)
{
	/*
	 * k_work_submit: add work to the system workqueue.
	 * The workqueue thread will call work_handler() when it runs.
	 * This is safe to call from ISR context.
	 *
	 * If the work item is already pending (not yet processed),
	 * this call is ignored — no duplicate submissions.
	 */
	k_work_submit(&my_work);

	/*
	 * k_work_schedule: submit delayed work.
	 * The handler will run after K_SECONDS(1) on the workqueue thread.
	 * Useful for: debouncing, retries, deferred processing.
	 */
	k_work_schedule(&my_delayed_work, K_SECONDS(1));
}

K_TIMER_DEFINE(workq_timer, workq_timer_cb, NULL);

/*
 * workq_task: starts the workqueue demo timer.
 * The actual work is done by the system workqueue thread, not this thread.
 * This thread just starts the timer and exits.
 */
void workq_task(void *p1, void *p2, void *p3)
{
	TPRINTK(PG_WORKQ, "[workq] Starting workqueue demo (every 3 seconds)\n");

	/*
	 * Start timer: first fire in 3s, repeat every 3s.
	 * The timer ISR submits work items to the system workqueue.
	 */
	k_timer_start(&workq_timer, K_SECONDS(3), K_SECONDS(3));

	/* This thread's job is done — it can exit.
	 * The timer + workqueue continue running without it. */
}

#define WORKQ_STACK  512
#define WORKQ_PRIO   6

/* ==========================================================
 * I2C Sensor Tasks (SHTC3, OPT4048, BNO085)
 *
 * All sensors share the I2C bus on GPIO 12 (SDA) / GPIO 13 (SCL).
 * A mutex protects bus access so tasks don't corrupt each other.
 *
 * FE310 I2C DRIVER BUG: The SiFive driver does NOT left-shift
 * the 7-bit address. Confirmed via Saleae logic analyzer.
 * Workaround: pass (addr << 1) for all I2C calls.
 * ==========================================================
 */
#define I2C_SHIFTED(addr7)  ((addr7) << 1)

K_MUTEX_DEFINE(i2c_bus_mutex);

/*
 * FE310 I2C bus recovery.
 * The SiFive I2C controller hangs after several transactions,
 * issuing a START with no follow-up. Full controller re-init
 * clears all pending state.
 *
 * OpenCores I2C register map (byte-addressed on FE310):
 *   0x00 = PRESCALE_LO
 *   0x04 = PRESCALE_HI
 *   0x08 = CTR (control: enable, interrupt enable)
 *   0x0C = TX/RX data
 *   0x10 = CMD/STATUS
 */
#define I2C0_BASE     0x10016000
#define I2C_PRE_LO    0x00
#define I2C_PRE_HI    0x04
#define I2C_CTR       0x08
#define I2C_CMD       0x10

/*
 * FE310 I2C bus recovery.
 *
 * The SiFive I2C controller (OpenCores) can get stuck with a
 * pending START. Before each transaction, ensure the controller
 * is idle by checking the status register and sending a STOP
 * if the bus is busy.
 *
 * Called BEFORE each mutex lock (not after unlock).
 */
#define I2C0_BASE  0x10016000
#define I2C_CMD    0x10  /* Command/Status register (byte offset) */
#define I2C_STA_BUSY   BIT(6)  /* Bus busy */
#define I2C_STA_TIP    BIT(1)  /* Transfer in progress */
#define I2C_CMD_STOP   BIT(6)  /* Generate STOP */

/*
 * Full I2C controller re-init.
 * The FE310 OpenCores I2C controller hangs after ~5 transactions.
 * The only reliable fix: completely re-initialize the controller
 * before each transaction.
 *
 * Prescaler for 100kHz: (16MHz / (100kHz * 5)) - 1 = 31 = 0x1F
 */
static void i2c_reinit(void)
{
	volatile uint8_t *base = (volatile uint8_t *)I2C0_BASE;

	/* Disable controller */
	base[0x08] = 0x00;  /* CTR = 0 (disable) */
	k_busy_wait(10);

	/* Set prescaler for 400kHz: (16MHz / (400kHz * 5)) - 1 = 7 */
	base[0x00] = 0x07;  /* PRESCALE_LO */
	base[0x04] = 0x00;  /* PRESCALE_HI */

	/* Re-enable */
	base[0x08] = 0x80;  /* CTR = EN */
	k_busy_wait(10);
}

static void i2c_bus_recover(void)
{
	i2c_reinit();
}

static inline void i2c_bus_settle(void)
{
	k_busy_wait(100);
}

/* Shared I2C bus init — sets up IOF pins (FE310 pinctrl workaround) */
static const struct device *i2c_bus_init(void)
{
	volatile uint32_t *iof_sel = (volatile uint32_t *)(GPIO_BASE + 0x3C);
	volatile uint32_t *iof_en  = (volatile uint32_t *)(GPIO_BASE + GPIO_IOF_EN);
	*iof_sel &= ~(BIT(12) | BIT(13));
	*iof_en  |= (BIT(12) | BIT(13));
	return DEVICE_DT_GET(DT_NODELABEL(i2c0));
}

/*
 * ========== Task 8: SHTC3 Temperature & Humidity ==========
 *
 * Reads temperature and humidity every 2 seconds.
 * Uses i2c_bus_mutex to share the I2C bus with other sensor tasks.
 */
#define SHTC3_STACK  1024
#define SHTC3_PRIO   9

void shtc3_task(void *p1, void *p2, void *p3)
{
	const struct device *i2c = i2c_bus_init();
	if (!device_is_ready(i2c)) {
		TPRINTK(PG_I2C, "[shtc3] I2C not ready!\n");
		return;
	}

	k_msleep(500); /* Let sensor power up */

	/* Wakeup */
	k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
	uint8_t wakeup[2] = {0x35, 0x17};
	int ret = i2c_write(i2c, wakeup, 2, I2C_SHIFTED(0x70));
	k_mutex_unlock(&i2c_bus_mutex);
	if (ret != 0) {
		TPRINTK(PG_I2C, "[shtc3] Wakeup failed: %d\n", ret);
		return;
	}
	k_msleep(1);

	/* Read ID */
	k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
	uint8_t id_cmd[2] = {0xEF, 0xC8};
	uint8_t id_buf[3];
	ret = i2c_write_read(i2c, I2C_SHIFTED(0x70), id_cmd, 2, id_buf, 3);
	k_mutex_unlock(&i2c_bus_mutex);
	if (ret != 0) {
		TPRINTK(PG_I2C, "[shtc3] ID read failed: %d\n", ret);
		return;
	}
	TPRINTK(PG_I2C, "[shtc3] Found! ID=0x%04X\n",
		(id_buf[0] << 8) | id_buf[1]);

	int64_t next = k_uptime_get() + 2000;

	while (1) {
		/* Wakeup + start measurement (short bus access) */
		i2c_bus_recover();
		k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
		uint8_t wake[2] = {0x35, 0x17};
		i2c_write(i2c, wake, 2, I2C_SHIFTED(0x70));
		uint8_t meas[2] = {0x78, 0x66};
		i2c_write(i2c, meas, 2, I2C_SHIFTED(0x70));
		k_mutex_unlock(&i2c_bus_mutex);
		i2c_bus_settle();

		/* Wait for measurement OFF the bus */
		k_msleep(15);

		/* Read result + sleep sensor */
		i2c_bus_recover();
		k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
		uint8_t data[6];
		ret = i2c_read(i2c, data, 6, I2C_SHIFTED(0x70));
		uint8_t slp[2] = {0xB0, 0x98};
		i2c_write(i2c, slp, 2, I2C_SHIFTED(0x70));
		k_mutex_unlock(&i2c_bus_mutex);
		i2c_bus_settle();

		if (ret == 0) {
			uint16_t raw_t = (data[0] << 8) | data[1];
			uint16_t raw_h = (data[3] << 8) | data[4];
			int32_t temp_100 = -4500 + (int32_t)17500 * raw_t / 65535;
			int32_t hum_100  = (int32_t)10000 * raw_h / 65535;
			TPRINTK(PG_I2C,
				"[shtc3] Temp: %d.%02d C  Humidity: %d.%02d %%RH\n",
				temp_100 / 100, temp_100 % 100,
				hum_100 / 100, hum_100 % 100);
		}

		k_sleep(K_TIMEOUT_ABS_MS(next));
		next += 2000;
	}
}

/*
 * ========== Task 9: OPT4048 Tristimulus Color Sensor ==========
 *
 * Reads 4-channel color data (CIE X, Y, Z, W) every 500ms.
 * CH1 (CIE Y) gives photopic illuminance (lux).
 */
#define OPT4048_STACK  1024
#define OPT4048_PRIO   9

static uint32_t opt4048_decode_ch(uint16_t msb, uint16_t lsb)
{
	uint32_t exp = (msb >> 12) & 0x0F;
	uint32_t man = ((uint32_t)(msb & 0x0FFF) << 8) | (lsb >> 8);
	return man << exp;
}

void opt4048_task(void *p1, void *p2, void *p3)
{
	const struct device *i2c = i2c_bus_init();
	if (!device_is_ready(i2c)) {
		TPRINTK(PG_I2C, "[opt4048] I2C not ready!\n");
		return;
	}

	k_msleep(500);

	/* Read Device ID (register 0x11) */
	k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
	uint8_t id_reg = 0x11;
	uint8_t id_raw[2];
	int ret = i2c_write_read(i2c, I2C_SHIFTED(0x44), &id_reg, 1, id_raw, 2);
	k_mutex_unlock(&i2c_bus_mutex);

	if (ret != 0) {
		TPRINTK(PG_I2C, "[opt4048] Not found (ret=%d)\n", ret);
		return;
	}
	TPRINTK(PG_I2C, "[opt4048] Found! ID=0x%04X\n",
		(id_raw[0] << 8) | id_raw[1]);

	/* Configure: auto-range, 25ms/ch, continuous.
	 * Also set register pointer to CH1 (0x02) for bare reads. */
	k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
	uint16_t cfg = (0x0C << 10) | (0x06 << 6) | (0x03 << 4);
	uint8_t cfg_buf[3] = {0x0A, cfg >> 8, cfg & 0xFF};
	i2c_write(i2c, cfg_buf, 3, I2C_SHIFTED(0x44));
	uint8_t set_ptr = 0x02;
	i2c_write(i2c, &set_ptr, 1, I2C_SHIFTED(0x44));
	k_mutex_unlock(&i2c_bus_mutex);
	i2c_bus_settle();

	TPRINTK(PG_I2C, "[opt4048] Configured\n");
	k_msleep(150);

	int64_t next = k_uptime_get() + 2000;

	while (1) {
		/* Re-set register pointer each time to avoid stale state */
		i2c_bus_recover();
		k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
		uint8_t set_reg = 0x02;
		i2c_write(i2c, &set_reg, 1, I2C_SHIFTED(0x44));
		k_mutex_unlock(&i2c_bus_mutex);
		i2c_bus_settle();

		/* Wait 100ms for OPT4048 to prepare data */
		k_msleep(100);

		/* Now read */
		i2c_bus_recover();
		k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
		uint8_t raw[4];
		ret = i2c_read(i2c, raw, 4, I2C_SHIFTED(0x44));
		k_mutex_unlock(&i2c_bus_mutex);
		i2c_bus_settle();

		if (ret == 0) {
			uint32_t y = opt4048_decode_ch(
				(raw[0]<<8)|raw[1], (raw[2]<<8)|raw[3]);
			uint32_t lux_c = y * 215 / 1000;
			TPRINTK(PG_I2C, "[opt4048] Lux=%u.%02u\n",
				lux_c / 100, lux_c % 100);
		} else {
			TPRINTK(PG_I2C, "[opt4048] Read failed: %d\n", ret);
		}

		k_sleep(K_TIMEOUT_ABS_MS(next));
		next += 2000;
	}
}

/*
 * ========== Task 10: BNO085 IMU ==========
 *
 * Implemented in bno085_task.c using the CEVA SH-2 library.
 * Reads rotation vector (quaternion) and accelerometer data.
 */
#include "bno085_task.h"
#define BNO085_STACK  2048  /* SH-2 library needs larger stack */
#define BNO085_PRIO   9

/* ========== Create all threads at compile time ==========
 *
 * RTOS demo threads commented out to free RAM for I2C sensors.
 * Uncomment them when not using sensors (set active_groups = PG_RTOS).
 */

/* LED disabled — GPIO 5 is now SPI1 SCK for BNO085
K_THREAD_DEFINE(blink_tid, BLINK_STACK,
		blink_task, NULL, NULL, NULL,
		BLINK_PRIO, 0, 0);
*/

/*
K_THREAD_DEFINE(counter_tid, COUNTER_STACK,
		counter_task, NULL, NULL, NULL,
		COUNTER_PRIO, 0, 0);

K_THREAD_DEFINE(printer_tid, PRINTER_STACK,
		printer_task, NULL, NULL, NULL,
		PRINTER_PRIO, 0, 0);

K_THREAD_DEFINE(heartbeat_tid, HEARTBEAT_STACK,
		heartbeat_task, NULL, NULL, NULL,
		HEARTBEAT_PRIO, 0, 0);

K_THREAD_DEFINE(mutex_lo_tid, MUTEX_LO_STACK,
		mutex_lo_task, NULL, NULL, NULL,
		MUTEX_LO_PRIO, 0, 2000);

K_THREAD_DEFINE(mutex_med_tid, MUTEX_MED_STACK,
		mutex_med_task, NULL, NULL, NULL,
		MUTEX_MED_PRIO, 0, 2000);

K_THREAD_DEFINE(mutex_hi_tid, MUTEX_HI_STACK,
		mutex_hi_task, NULL, NULL, NULL,
		MUTEX_HI_PRIO, 0, 2000);

K_THREAD_DEFINE(workq_tid, WORKQ_STACK,
		workq_task, NULL, NULL, NULL,
		WORKQ_PRIO, 0, 3000);
*/

K_THREAD_DEFINE(shtc3_tid, SHTC3_STACK,
		shtc3_task, NULL, NULL, NULL,
		SHTC3_PRIO, 0, 1000);

K_THREAD_DEFINE(opt4048_tid, OPT4048_STACK,
		opt4048_task, NULL, NULL, NULL,
		OPT4048_PRIO, 0, 1000);

K_THREAD_DEFINE(bno085_tid, BNO085_STACK,
		bno085_task, NULL, NULL, NULL,
		BNO085_PRIO, 0, 1500);
