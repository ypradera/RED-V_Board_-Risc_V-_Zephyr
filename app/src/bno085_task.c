/*
 * BNO085 IMU Task — SPI mode
 *
 * HAL implementation matches the Adafruit BNO08x Arduino library:
 *   - Two-phase SPI read: 4-byte header → wait INTN → full packet
 *   - Half-duplex write (no circular buffer, no MISO capture)
 *   - Blocking INTN wait (500ms timeout with auto hardware reset)
 *   - SPI Mode 3 (CPOL=1, CPHA=1) at 1 MHz
 *
 * Wiring (RED-V silkscreen):
 *   "8"  (GPIO 0) → RST
 *   "9"  (GPIO 1) → INT
 *   "10" (GPIO 2) → CS
 *   "11" (GPIO 3) → DI  (MOSI)
 *   "12" (GPIO 4) → SDA (MISO)
 *   "13" (GPIO 5) → SCL (SCK)
 *   3.3V → VIN
 *   GND  → GND
 *   PS0/PS1 solder bridges closed (SPI mode)
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include "sh2.h"
#include "sh2_err.h"
#include "sh2_SensorValue.h"

#define BNO_RST_PIN  0   /* GPIO 0 = RED-V "8" */
#define BNO_INT_PIN  1   /* GPIO 1 = RED-V "9" */

static const struct device *spi_dev;
static const struct device *gpio_dev;

/* SPI Mode 3, 1 MHz (matches Adafruit library) */
static struct spi_config bno_spi_cfg = {
	.frequency = 1000000,
	.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |
		     SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs = { .gpio = { 0 } },
};

/* ---- SPI helpers (half-duplex, matching Adafruit) ---- */

static int bno_spi_read(uint8_t *buf, uint16_t len)
{
	struct spi_buf rx = {.buf = buf, .len = len};
	struct spi_buf_set rx_set = {.buffers = &rx, .count = 1};
	return spi_read(spi_dev, &bno_spi_cfg, &rx_set);
}

static int bno_spi_write(uint8_t *buf, uint16_t len)
{
	struct spi_buf tx = {.buf = buf, .len = len};
	struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};
	return spi_write(spi_dev, &bno_spi_cfg, &tx_set);
}

/* ---- Hardware reset (matches Adafruit: HIGH-LOW-HIGH 10ms each) ---- */

static void hal_hardware_reset(void)
{
	gpio_pin_set(gpio_dev, BNO_RST_PIN, 1);
	k_msleep(10);
	gpio_pin_set(gpio_dev, BNO_RST_PIN, 0);
	k_msleep(10);
	gpio_pin_set(gpio_dev, BNO_RST_PIN, 1);
	k_msleep(10);
}

/*
 * Wait for INTN to go LOW (BNO085 has data ready).
 * Polls for up to 500ms. If timeout, performs hardware reset.
 * Matches Adafruit spihal_wait_for_int() exactly.
 */
static bool hal_wait_for_int(void)
{
	for (int i = 0; i < 500; i++) {
		if (gpio_pin_get(gpio_dev, BNO_INT_PIN) == 0) {
			return true;
		}
		k_msleep(1);
	}
	/* Timeout — reset the BNO085 */
	hal_hardware_reset();
	return false;
}

/* ---- SH-2 HAL Implementation ---- */

static int bno_hal_open(sh2_Hal_t *self)
{
	spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));
	if (!device_is_ready(spi_dev)) {
		printk("[bno085] SPI1 not ready!\n");
		return SH2_ERR;
	}

	gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	if (!device_is_ready(gpio_dev)) {
		printk("[bno085] GPIO not ready!\n");
		return SH2_ERR;
	}

	gpio_pin_configure(gpio_dev, BNO_RST_PIN, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure(gpio_dev, BNO_INT_PIN, GPIO_INPUT | GPIO_PULL_UP);

	printk("[bno085] Resetting BNO085...\n");
	hal_hardware_reset();

	printk("[bno085] Waiting for INT...\n");
	if (hal_wait_for_int()) {
		printk("[bno085] INT asserted — ready!\n");
	} else {
		printk("[bno085] INT timeout\n");
	}

	return SH2_OK;
}

static void bno_hal_close(sh2_Hal_t *self)
{
}

/*
 * hal_read: Two-phase SPI read (matches Adafruit spihal_read exactly).
 *
 *   1. Wait for INTN LOW
 *   2. Read 4-byte SHTP header (one CS transaction)
 *   3. Parse packet length from header
 *   4. Wait for INTN LOW again
 *   5. Read full packet (second CS transaction — BNO085 re-sends header)
 */
static int bno_hal_read(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len,
			uint32_t *t_us)
{
	/* Step 1: Wait for INTN */
	if (!hal_wait_for_int()) {
		return 0;
	}

	/* Step 2: Read 4-byte SHTP header */
	if (bno_spi_read(pBuffer, 4) != 0) {
		return 0;
	}

	/* Step 3: Parse packet length */
	uint16_t packet_size = (uint16_t)pBuffer[0] | ((uint16_t)pBuffer[1] << 8);
	packet_size &= ~0x8000;  /* Clear continuation bit */

	if (packet_size == 0) {
		return 0;
	}

	if (packet_size > len) {
		return 0;  /* Adafruit returns 0 if packet > buffer */
	}

	/* Step 4: Wait for INTN again */
	if (!hal_wait_for_int()) {
		return 0;
	}

	/* Step 5: Read full packet */
	if (bno_spi_read(pBuffer, packet_size) != 0) {
		return 0;
	}

	*t_us = (uint32_t)(k_uptime_get() * 1000);
	return packet_size;
}

/*
 * hal_write: Wait for INTN, then write (matches Adafruit spihal_write).
 * Half-duplex — MISO data during writes is discarded.
 */
static int bno_hal_write(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len)
{
	if (!hal_wait_for_int()) {
		return 0;
	}
	if (bno_spi_write(pBuffer, len) != 0) {
		return 0;
	}
	return len;
}

static uint32_t bno_hal_getTimeUs(sh2_Hal_t *self)
{
	return (uint32_t)(k_uptime_get() * 1000);
}

/* ---- SH-2 Callbacks ---- */

static volatile bool sensor_data_ready;
static sh2_SensorValue_t latest_value;
static volatile bool was_reset;

static void sh2_event_callback(void *cookie, sh2_AsyncEvent_t *pEvent)
{
	if (pEvent->eventId == SH2_RESET) {
		was_reset = true;
		printk("[bno085] Reset detected\n");
	}
}

static void sh2_sensor_callback(void *cookie, sh2_SensorEvent_t *pEvent)
{
	sh2_decodeSensorEvent(&latest_value, pEvent);
	sensor_data_ready = true;
}

static void enable_reports(void)
{
	sh2_SensorConfig_t config = {0};
	config.reportInterval_us = 20000;  /* 50 Hz */

	sh2_setSensorConfig(SH2_ROTATION_VECTOR, &config);
	sh2_setSensorConfig(SH2_GYROSCOPE_CALIBRATED, &config);
	sh2_setSensorConfig(SH2_LINEAR_ACCELERATION, &config);
	sh2_setSensorConfig(SH2_GRAVITY, &config);

	printk("[bno085] Reports enabled at 50 Hz\n");
}

/* ---- BNO085 Task ---- */

void bno085_task(void *p1, void *p2, void *p3)
{
	static sh2_Hal_t bno_hal;

	printk("[bno085] Starting (SPI mode, 1 MHz)...\n");

	bno_hal.open      = bno_hal_open;
	bno_hal.close     = bno_hal_close;
	bno_hal.read      = bno_hal_read;
	bno_hal.write     = bno_hal_write;
	bno_hal.getTimeUs = bno_hal_getTimeUs;

	int rc = sh2_open(&bno_hal, sh2_event_callback, NULL);
	if (rc != SH2_OK) {
		printk("[bno085] sh2_open failed: %d\n", rc);
		return;
	}
	printk("[bno085] SH-2 connected!\n");

	sh2_ProductIds_t prodIds;
	rc = sh2_getProdIds(&prodIds);
	if (rc == SH2_OK && prodIds.numEntries > 0) {
		printk("[bno085] SW: %d.%d.%d\n",
		       prodIds.entry[0].swVersionMajor,
		       prodIds.entry[0].swVersionMinor,
		       prodIds.entry[0].swVersionPatch);
	} else {
		printk("[bno085] getProdIds: %d\n", rc);
	}

	sh2_setSensorCallback(sh2_sensor_callback, NULL);
	enable_reports();
	was_reset = false;

	printk("[bno085] Reading IMU data...\n");

	uint32_t data_count = 0;
	uint32_t last_print = 0;

	while (1) {
		/* Check for reset (matches Adafruit wasReset() pattern) */
		if (was_reset) {
			was_reset = false;
			printk("[bno085] Re-enabling reports after reset\n");
			enable_reports();
		}

		/* Service SH-2 — one packet per call (matches Adafruit) */
		sh2_service();

		if (sensor_data_ready) {
			data_count++;
			sensor_data_ready = false;

			switch (latest_value.sensorId) {
			case SH2_ROTATION_VECTOR: {
				int32_t qi = (int32_t)(latest_value.un.rotationVector.i * 10000);
				int32_t qj = (int32_t)(latest_value.un.rotationVector.j * 10000);
				int32_t qk = (int32_t)(latest_value.un.rotationVector.k * 10000);
				int32_t qr = (int32_t)(latest_value.un.rotationVector.real * 10000);
				printk("[imu] Q: %d %d %d %d\n", qi, qj, qk, qr);
				break;
			}
			case SH2_GYROSCOPE_CALIBRATED: {
				int32_t gx = (int32_t)(latest_value.un.gyroscope.x * 100);
				int32_t gy = (int32_t)(latest_value.un.gyroscope.y * 100);
				int32_t gz = (int32_t)(latest_value.un.gyroscope.z * 100);
				printk("[imu] G: %d %d %d\n", gx, gy, gz);
				break;
			}
			case SH2_LINEAR_ACCELERATION: {
				int32_t lx = (int32_t)(latest_value.un.linearAcceleration.x * 100);
				int32_t ly = (int32_t)(latest_value.un.linearAcceleration.y * 100);
				int32_t lz = (int32_t)(latest_value.un.linearAcceleration.z * 100);
				printk("[imu] A: %d %d %d\n", lx, ly, lz);
				break;
			}
			case SH2_GRAVITY: {
				int32_t gvx = (int32_t)(latest_value.un.gravity.x * 100);
				int32_t gvy = (int32_t)(latest_value.un.gravity.y * 100);
				int32_t gvz = (int32_t)(latest_value.un.gravity.z * 100);
				printk("[imu] V: %d %d %d\n", gvx, gvy, gvz);
				break;
			}
			default:
				break;
			}
		}

		/* Heartbeat every 10 seconds */
		uint32_t now = (uint32_t)(k_uptime_get() / 1000);
		if (now != last_print && (now % 10) == 0) {
			last_print = now;
			printk("[bno085] data=%u\n", data_count);
		}

		/* 10ms delay (matches Adafruit example loop) */
		k_msleep(10);
	}
}
