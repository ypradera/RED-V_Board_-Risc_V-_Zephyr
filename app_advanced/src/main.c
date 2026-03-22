/*
 * RED-V Zephyr Advanced Learning App
 *
 * Demonstrates:
 *   1. k_mem_slab   — fixed-size memory block allocator
 *   2. k_heap       — variable-size dynamic memory allocation
 *   3. k_event      — event flags for multi-condition synchronization
 *   4. k_condvar    — condition variables (wait for a condition)
 *   5. Power management concepts (idle thread, sleep awareness)
 *   6. Security     — stack overflow detection, stack canaries
 *
 * Architecture:
 *
 *   [producer] ── allocates from mem_slab ──> [EVENT FLAGS] ──> [consumer] ── frees block
 *
 *   [writer] ── locks mutex, signals condvar ──> [reader] ── wakes on condvar, reads data
 *
 *   [monitor] ── prints memory stats, thread info, idle time
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>

/* ========== LED ========== */
#define GPIO_BASE       0x10012000
#define GPIO_OUTPUT_EN  0x08
#define GPIO_OUTPUT_VAL 0x0C
#define GPIO_IOF_EN     0x38
#define LED_PIN         5

static volatile uint32_t *gpio_reg(uint32_t offset)
{
	return (volatile uint32_t *)(GPIO_BASE + offset);
}

/*
 * ========== 1. Memory Slab (k_mem_slab) ==========
 *
 * WHY NO MALLOC ON EMBEDDED?
 *
 *   malloc() has problems on small MCUs:
 *   - Memory fragmentation: after many alloc/free cycles, memory becomes
 *     Swiss cheese — plenty of total free bytes but no single block big
 *     enough to satisfy a request.
 *   - Non-deterministic: malloc() takes unpredictable time to search
 *     for free blocks. RTOS tasks need predictable timing.
 *   - No upper bound: you can't guarantee malloc won't fail at runtime.
 *
 * k_mem_slab SOLVES THIS:
 *
 *   Pre-allocates a fixed number of fixed-size blocks at compile time.
 *   - ZERO fragmentation (all blocks are the same size)
 *   - O(1) allocation and free (constant time, deterministic)
 *   - Fixed capacity — you know exactly how many blocks are available
 *
 * Think of it like a stack of identical trays:
 *   alloc = take a tray off the top
 *   free  = put the tray back on the stack
 */

/* Each block holds a 32-byte sensor reading */
struct sensor_reading {
	uint32_t timestamp;
	int16_t  temp;       /* temperature * 100 */
	int16_t  humidity;   /* humidity * 100 */
	char     source[8];
	uint32_t sequence;
	uint8_t  padding[8]; /* pad to 32 bytes total */
};

/*
 * K_MEM_SLAB_DEFINE(name, block_size, num_blocks, alignment)
 *
 * Creates 8 blocks of 32 bytes each = 256 bytes total.
 * All allocated at compile time — no runtime overhead.
 */
K_MEM_SLAB_DEFINE(reading_slab, sizeof(struct sensor_reading), 8, 4);

/*
 * ========== 2. Heap (k_heap) ==========
 *
 * For when you need VARIABLE-SIZE allocations.
 * Less efficient than k_mem_slab (fragmentation possible),
 * but more flexible.
 *
 * CONFIG_HEAP_MEM_POOL_SIZE in prj.conf sets the system heap size.
 * You can also create private heaps with K_HEAP_DEFINE.
 *
 * k_malloc / k_free use the system heap.
 * k_heap_alloc / k_heap_free use a specific heap.
 */
K_HEAP_DEFINE(my_heap, 256); /* 256-byte private heap */

/*
 * ========== 3. Event Flags (k_event) ==========
 *
 * WHY EVENT FLAGS?
 *
 *   A semaphore signals ONE condition ("data ready").
 *   Event flags signal MULTIPLE conditions simultaneously:
 *   "temperature ready AND humidity ready AND calibrated"
 *
 *   A thread can wait for ANY combination of flags:
 *   - Wait for ALL flags (AND): both temp AND humidity ready
 *   - Wait for ANY flag (OR): either temp OR humidity ready
 *
 *   Each flag is a single bit in a 32-bit word.
 */

/* Define event flag bits */
#define EVT_TEMP_READY    BIT(0)   /* Temperature data available */
#define EVT_HUM_READY     BIT(1)   /* Humidity data available */
#define EVT_DATA_CONSUMED BIT(2)   /* Consumer processed the data */

K_EVENT_DEFINE(sensor_events);

/*
 * ========== 4. Condition Variable (k_condvar) ==========
 *
 * WHY CONDITION VARIABLES?
 *
 *   A mutex protects data. A condvar lets a thread WAIT for
 *   that data to reach a certain STATE.
 *
 *   Pattern:
 *     1. Lock mutex
 *     2. Check condition (e.g., "buffer not empty")
 *     3. If false: k_condvar_wait() — atomically unlocks mutex + sleeps
 *     4. When signaled: wakes up with mutex re-locked
 *     5. Re-check condition (might be spurious wakeup)
 *     6. Use the data
 *     7. Unlock mutex
 *
 *   This is the standard producer-consumer pattern in POSIX/RTOS.
 */
K_MUTEX_DEFINE(condvar_mutex);
K_CONDVAR_DEFINE(data_ready_cv);

/* Shared buffer protected by mutex + condvar */
static struct {
	int32_t value;
	bool    new_data;
} condvar_buffer;

/* ========== Thread configs ========== */
#define BLINK_STACK     512
#define BLINK_PRIO      7

#define PRODUCER_STACK  512
#define PRODUCER_PRIO   5

#define CONSUMER_STACK  512
#define CONSUMER_PRIO   4

#define WRITER_STACK    512
#define WRITER_PRIO     5

#define READER_STACK    512
#define READER_PRIO     4

#define MONITOR_STACK   768
#define MONITOR_PRIO    8

/* ========== Blink task (heartbeat) ========== */
#define BLINK_PERIOD_MS 500

void blink_task(void *p1, void *p2, void *p3)
{
	*gpio_reg(GPIO_IOF_EN) &= ~BIT(LED_PIN);
	*gpio_reg(GPIO_OUTPUT_EN) |= BIT(LED_PIN);
	printk("[blink] LED heartbeat started\n");

	int64_t next = k_uptime_get() + BLINK_PERIOD_MS;
	while (1) {
		*gpio_reg(GPIO_OUTPUT_VAL) ^= BIT(LED_PIN);
		k_sleep(K_TIMEOUT_ABS_MS(next));
		next += BLINK_PERIOD_MS;
	}
}

/*
 * ========== Demo 1: Memory Slab + Event Flags ==========
 *
 * Producer:
 *   1. Allocates a block from the slab (O(1), no fragmentation)
 *   2. Fills it with simulated sensor data
 *   3. Sets event flags to signal the consumer
 *
 * Consumer:
 *   1. Waits for BOTH temp AND humidity flags (AND condition)
 *   2. Reads the data
 *   3. Frees the slab block
 *   4. Signals back that data was consumed
 */

/* Shared pointer — producer allocates, consumer frees */
static struct sensor_reading *shared_reading;

void slab_producer_task(void *p1, void *p2, void *p3)
{
	printk("[slab-prod] Started — allocating from mem_slab\n");
	uint32_t seq = 0;

	while (1) {
		/*
		 * k_mem_slab_alloc: get a fixed-size block.
		 *
		 * K_MSEC(100): wait up to 100ms if no blocks available.
		 * Returns 0 on success, -ENOMEM if timeout.
		 *
		 * This is O(1) — constant time, no fragmentation.
		 */
		struct sensor_reading *reading;
		int ret = k_mem_slab_alloc(&reading_slab, (void **)&reading,
					   K_MSEC(100));
		if (ret != 0) {
			printk("[slab-prod] Slab full! (%d/%d used)\n",
			       reading_slab.info.num_used,
			       reading_slab.info.num_blocks);
			k_msleep(500);
			continue;
		}

		/* Fill the block with simulated data */
		reading->timestamp = (uint32_t)k_uptime_get();
		reading->temp = 2350 + (seq % 20);     /* 23.50 - 23.70 C */
		reading->humidity = 4500 + (seq % 30);  /* 45.00 - 45.30 % */
		reading->sequence = ++seq;
		strncpy(reading->source, "sim", sizeof(reading->source));

		/* Share the pointer with the consumer */
		shared_reading = reading;

		printk("[slab-prod] Alloc'd block #%d (%d/%d used) "
		       "temp=%d.%02d hum=%d.%02d\n",
		       seq, reading_slab.info.num_used,
		       reading_slab.info.num_blocks,
		       reading->temp / 100, reading->temp % 100,
		       reading->humidity / 100, reading->humidity % 100);

		/*
		 * Set event flags to signal the consumer.
		 * k_event_set: sets bits. Multiple threads can set different bits.
		 * Consumer waits for ALL bits with k_event_wait.
		 */
		k_event_set(&sensor_events,
			    EVT_TEMP_READY | EVT_HUM_READY);

		/* Wait for consumer to process before producing next */
		k_event_wait(&sensor_events, EVT_DATA_CONSUMED,
			     true, /* reset flags after wait */
			     K_SECONDS(5));

		k_msleep(2000);
	}
}

void slab_consumer_task(void *p1, void *p2, void *p3)
{
	printk("[slab-con] Started — waiting for event flags\n");

	while (1) {
		/*
		 * k_event_wait: block until specified flags are set.
		 *
		 * EVT_TEMP_READY | EVT_HUM_READY: wait for BOTH flags (AND).
		 * true: automatically reset the flags after we wake up.
		 * K_FOREVER: wait indefinitely.
		 *
		 * Returns the flags that were set when we woke up.
		 */
		uint32_t events = k_event_wait(&sensor_events,
					       EVT_TEMP_READY | EVT_HUM_READY,
					       true, /* auto-reset */
					       K_FOREVER);

		if (shared_reading == NULL) {
			continue;
		}

		printk("[slab-con] Got events 0x%02x — "
		       "reading #%d: temp=%d.%02d hum=%d.%02d\n",
		       events,
		       shared_reading->sequence,
		       shared_reading->temp / 100,
		       shared_reading->temp % 100,
		       shared_reading->humidity / 100,
		       shared_reading->humidity % 100);

		/*
		 * k_mem_slab_free: return the block to the slab.
		 * O(1), no fragmentation.
		 */
		k_mem_slab_free(&reading_slab, shared_reading);
		shared_reading = NULL;

		/* Signal producer that we're done */
		k_event_set(&sensor_events, EVT_DATA_CONSUMED);
	}
}

/*
 * ========== Demo 2: Condition Variable ==========
 *
 * Writer:
 *   1. Locks mutex
 *   2. Updates shared data
 *   3. Signals condvar ("new data available")
 *   4. Unlocks mutex
 *
 * Reader:
 *   1. Locks mutex
 *   2. While no new data: k_condvar_wait (unlocks mutex + sleeps)
 *   3. Wakes up with mutex re-locked
 *   4. Reads data
 *   5. Unlocks mutex
 */
void condvar_writer_task(void *p1, void *p2, void *p3)
{
	printk("[cv-writer] Started — writing with condvar signal\n");
	int32_t measurement = 100;

	while (1) {
		k_msleep(3000);

		/*
		 * Lock the mutex before modifying shared data.
		 * This is required — condvar only works with a mutex.
		 */
		k_mutex_lock(&condvar_mutex, K_FOREVER);

		/* Update the shared buffer */
		condvar_buffer.value = measurement++;
		condvar_buffer.new_data = true;

		printk("[cv-writer] Wrote value %d, signaling reader\n",
		       condvar_buffer.value);

		/*
		 * k_condvar_signal: wake up ONE thread waiting on this condvar.
		 * Use k_condvar_broadcast to wake ALL waiting threads.
		 */
		k_condvar_signal(&data_ready_cv);

		k_mutex_unlock(&condvar_mutex);
	}
}

void condvar_reader_task(void *p1, void *p2, void *p3)
{
	printk("[cv-reader] Started — waiting on condvar\n");

	while (1) {
		k_mutex_lock(&condvar_mutex, K_FOREVER);

		/*
		 * ALWAYS use a while loop, not an if statement!
		 * Condvar can have "spurious wakeups" where the thread
		 * wakes up but the condition isn't actually true.
		 * The while loop re-checks the condition after waking.
		 */
		while (!condvar_buffer.new_data) {
			/*
			 * k_condvar_wait: atomically does TWO things:
			 *   1. Unlocks the mutex (so writer can access data)
			 *   2. Puts this thread to sleep
			 *
			 * When signaled, it atomically:
			 *   1. Wakes this thread
			 *   2. Re-locks the mutex
			 *
			 * This atomic unlock+sleep is what makes condvar
			 * safe — no window where data could change between
			 * checking and sleeping.
			 */
			k_condvar_wait(&data_ready_cv,
				       &condvar_mutex,
				       K_FOREVER);
		}

		/* We have the mutex and the condition is true */
		printk("[cv-reader] Read value: %d\n", condvar_buffer.value);
		condvar_buffer.new_data = false;

		k_mutex_unlock(&condvar_mutex);
	}
}

/*
 * ========== Demo 3: System Monitor (Memory + Security) ==========
 *
 * Prints memory usage stats and demonstrates security concepts.
 *
 * SECURITY ON EMBEDDED:
 *
 *   On small MCUs like the FE310 (no MMU/MPU), security is limited to:
 *
 *   1. Stack overflow detection (CONFIG_STACK_SENTINEL):
 *      Writes a canary value at the bottom of each thread's stack.
 *      If the canary is overwritten, the kernel detects stack overflow
 *      and calls the fatal error handler.
 *
 *   2. Stack canaries (CONFIG_STACK_CANARIES):
 *      Compiler inserts check values on the stack before/after function
 *      calls. Detects buffer overflows that corrupt the return address.
 *      (Requires compiler support — GCC -fstack-protector)
 *
 *   3. Memory isolation (requires MPU — FE310 does NOT have one):
 *      On ARM MCUs with MPU, Zephyr can isolate threads so they
 *      can't access each other's memory. Not available on RISC-V FE310.
 *
 * POWER MANAGEMENT:
 *
 *   When ALL threads are sleeping, Zephyr's idle thread runs.
 *   On MCUs with PM support, the idle thread can enter low-power modes.
 *   The FE310 supports WFI (Wait For Interrupt) which halts the CPU
 *   until the next interrupt — saving power during idle periods.
 *
 *   In production, you'd use:
 *     CONFIG_PM=y
 *     pm_state_force(PM_STATE_SUSPEND_TO_IDLE)
 *   But the FE310 Zephyr port has limited PM support.
 */
void monitor_task(void *p1, void *p2, void *p3)
{
	printk("[monitor] System monitor started\n");
	printk("[monitor] Stack sentinel: %s\n",
	       IS_ENABLED(CONFIG_STACK_SENTINEL) ? "ENABLED" : "disabled");

	while (1) {
		k_msleep(10000); /* Every 10 seconds */

		/* Memory slab stats */
		printk("[monitor] === System Status ===\n");
		printk("[monitor] Mem slab: %d/%d blocks used (%d bytes each)\n",
		       reading_slab.info.num_used,
		       reading_slab.info.num_blocks,
		       reading_slab.info.block_size);

		/* Heap stats */
		void *test = k_heap_alloc(&my_heap, 64, K_NO_WAIT);
		if (test) {
			printk("[monitor] Heap: 64-byte alloc OK (free space available)\n");
			k_heap_free(&my_heap, test);
		} else {
			printk("[monitor] Heap: 64-byte alloc FAILED (fragmented or full)\n");
		}

		/* System k_malloc test */
		void *sys_test = k_malloc(32);
		if (sys_test) {
			printk("[monitor] System heap: 32-byte k_malloc OK\n");
			k_free(sys_test);
		} else {
			printk("[monitor] System heap: k_malloc FAILED\n");
		}

		/* Uptime */
		uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000);
		printk("[monitor] Uptime: %u seconds\n", uptime_s);
		printk("[monitor] ===================\n");
	}
}

/* ========== Create all threads ========== */

K_THREAD_DEFINE(blink_tid, BLINK_STACK,
		blink_task, NULL, NULL, NULL,
		BLINK_PRIO, 0, 0);

/* Memory slab + event flags demo */
K_THREAD_DEFINE(slab_prod_tid, PRODUCER_STACK,
		slab_producer_task, NULL, NULL, NULL,
		PRODUCER_PRIO, 0, 1000);

K_THREAD_DEFINE(slab_con_tid, CONSUMER_STACK,
		slab_consumer_task, NULL, NULL, NULL,
		CONSUMER_PRIO, 0, 1000);

/* Condition variable demo */
K_THREAD_DEFINE(cv_writer_tid, WRITER_STACK,
		condvar_writer_task, NULL, NULL, NULL,
		WRITER_PRIO, 0, 1500);

K_THREAD_DEFINE(cv_reader_tid, READER_STACK,
		condvar_reader_task, NULL, NULL, NULL,
		READER_PRIO, 0, 1500);

/* System monitor */
K_THREAD_DEFINE(monitor_tid, MONITOR_STACK,
		monitor_task, NULL, NULL, NULL,
		MONITOR_PRIO, 0, 2000);
