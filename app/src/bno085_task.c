/*
 * BNO085 IMU Task — SPI mode
 *
 * Uses the CEVA SH-2 library for SHTP protocol handling.
 * SPI eliminates the 1.7s clock stretching issue seen with I2C.
 *
 * Wiring (RED-V header pins):
 *   D10 (GPIO 2) → CS
 *   D11 (GPIO 3) → MOSI (SDA/SDI on BNO085)
 *   D12 (GPIO 4) → MISO (SDO/ADO on BNO085)
 *   D13 (GPIO 5) → SCK  (SCL on BNO085)
 *   BNO085 PS0   → 3.3V (SPI mode select)
 *   BNO085 PS1   → GND  (SPI mode select)
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

/*
 * Pin mapping (RED-V silkscreen → FE310 GPIO):
 *   "8"  = GPIO 0 = RST
 *   "9"  = GPIO 1 = INT
 *   "10" = GPIO 2 = CS  (SPI1 CS0 via IOF)
 *   "11" = GPIO 3 = MOSI (SPI1 DQ0 via IOF)
 *   "12" = GPIO 4 = MISO (SPI1 DQ1 via IOF)
 *   "13" = GPIO 5 = SCK  (SPI1 SCK via IOF)
 */
#define BNO_RST_PIN  0   /* GPIO 0 = RED-V "8" */
#define BNO_INT_PIN  1   /* GPIO 1 = RED-V "9" */
#define GPIO_BASE_ADDR 0x10012000

/* SPI device: SPI1 on the FE310 */
static const struct device *spi_dev;
static const struct device *gpio_dev;

static struct spi_config bno_spi_cfg = {
	.frequency = 3000000,  /* 3 MHz */
	.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |
		     SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs = { .gpio = { 0 } },  /* Use hardware CS (GPIO 2 via IOF) */
};

/* ---- SPI helpers ---- */

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

	/* Configure RST as output, INT as input */
	gpio_pin_configure(gpio_dev, BNO_RST_PIN, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure(gpio_dev, BNO_INT_PIN, GPIO_INPUT | GPIO_PULL_UP);

	printk("[bno085] SPI1 ready, resetting BNO085...\n");

	/* Hardware reset sequence:
	 *   RST LOW for 10ms → RST HIGH → wait for boot (~300ms)
	 */
	gpio_pin_set(gpio_dev, BNO_RST_PIN, 0);  /* Assert reset */
	k_msleep(10);
	gpio_pin_set(gpio_dev, BNO_RST_PIN, 1);  /* Release reset */
	k_msleep(300);  /* Wait for BNO085 to boot */

	/* Wait for INT to go low (BNO085 signals ready) */
	printk("[bno085] Waiting for INT...\n");
	int timeout = 100;  /* 100 * 10ms = 1 second max */
	while (timeout-- > 0) {
		if (gpio_pin_get(gpio_dev, BNO_INT_PIN) == 0) {
			printk("[bno085] INT asserted — BNO085 ready!\n");
			break;
		}
		k_msleep(10);
	}
	if (timeout <= 0) {
		printk("[bno085] INT timeout (may still work)\n");
	}

	return SH2_OK;
}

static void bno_hal_close(sh2_Hal_t *self)
{
}

static int bno_hal_read(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len,
			uint32_t *t_us)
{
	/* Only read when INT is asserted (low) — BNO085 has data */
	if (gpio_pin_get(gpio_dev, BNO_INT_PIN) != 0) {
		k_msleep(1);
		return 0;  /* No data ready */
	}

	/* Read full packet in one transaction.
	 * SHTP over SPI: the first 4 bytes are the header,
	 * followed by the payload. Read it all at once. */
	uint16_t read_len = (len < 256) ? len : 256;
	memset(pBuffer, 0, read_len);

	if (bno_spi_read(pBuffer, read_len) != 0) {
		return 0;
	}

	/* Parse packet length from header */
	uint16_t packet_len = (uint16_t)(pBuffer[0] | (pBuffer[1] << 8));
	packet_len &= 0x7FFF;

	if (packet_len == 0 || packet_len == 0x7FFF) {
		return 0;
	}

	if (packet_len > read_len) {
		packet_len = read_len;
	}

	*t_us = (uint32_t)(k_uptime_get() * 1000);
	return packet_len;
}

static uint32_t write_count;

static int bno_hal_write(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len)
{
	write_count++;
	int ret = bno_spi_write(pBuffer, len);
	if (ret != 0) {
		printk("[bno085] SPI write failed: %d (len=%d)\n", ret, len);
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

static volatile bool needs_report_setup;
static volatile bool init_complete;

static void sh2_event_callback(void *cookie, sh2_AsyncEvent_t *pEvent)
{
	if (pEvent->eventId == SH2_RESET) {
		if (init_complete) {
			printk("[bno085] Reset detected — will re-enable reports\n");
			needs_report_setup = true;
		} else {
			printk("[bno085] Initial reset (expected)\n");
		}
	}
}

static void sh2_sensor_callback(void *cookie, sh2_SensorEvent_t *pEvent)
{
	sh2_decodeSensorEvent(&latest_value, pEvent);
	sensor_data_ready = true;
}

static int enable_report(uint8_t sensor_id, uint32_t interval_us)
{
	sh2_SensorConfig_t config = {0};
	config.reportInterval_us = interval_us;
	return sh2_setSensorConfig(sensor_id, &config);
}

/* ---- BNO085 Task ---- */

void bno085_task(void *p1, void *p2, void *p3)
{
	static sh2_Hal_t bno_hal;

	printk("[bno085] Starting (SPI mode)...\n");

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
	printk("[bno085] SH-2 connected via SPI!\n");

	sh2_ProductIds_t prodIds;
	rc = sh2_getProdIds(&prodIds);
	if (rc == SH2_OK) {
		printk("[bno085] Product entries: %d\n", prodIds.numEntries);
		if (prodIds.numEntries > 0) {
			printk("[bno085] SW Version: %d.%d.%d\n",
			       prodIds.entry[0].swVersionMajor,
			       prodIds.entry[0].swVersionMinor,
			       prodIds.entry[0].swVersionPatch);
		}
	} else {
		printk("[bno085] getProdIds failed: %d\n", rc);
	}

	sh2_setSensorCallback(sh2_sensor_callback, NULL);

	/* Enable biomechanics sensor suite at 50 Hz (20ms = 20000us) */
	#define BIO_INTERVAL_US  20000  /* 50 Hz */

	rc = enable_report(SH2_ROTATION_VECTOR, BIO_INTERVAL_US);
	printk("[bno085] Rotation vector (orientation): %s\n",
	       rc == SH2_OK ? "OK" : "FAIL");

	rc = enable_report(SH2_GYROSCOPE_CALIBRATED, BIO_INTERVAL_US);
	printk("[bno085] Gyroscope (angular velocity):  %s\n",
	       rc == SH2_OK ? "OK" : "FAIL");

	rc = enable_report(SH2_LINEAR_ACCELERATION, BIO_INTERVAL_US);
	printk("[bno085] Linear accel (no gravity):     %s\n",
	       rc == SH2_OK ? "OK" : "FAIL");

	rc = enable_report(SH2_GRAVITY, BIO_INTERVAL_US);
	printk("[bno085] Gravity vector:                %s\n",
	       rc == SH2_OK ? "OK" : "FAIL");

	needs_report_setup = false;
	init_complete = true;
	printk("[bno085] Reading IMU data...\n");

	/* Track data flow to detect stalls */
	uint32_t last_data_count = 0;
	int64_t last_data_time = k_uptime_get();

	uint32_t service_count = 0;
	uint32_t data_count = 0;
	uint32_t last_print = 0;

	while (1) {
		/* Re-enable reports after a BNO085 reset */
		if (needs_report_setup) {
			needs_report_setup = false;
			printk("[bno085] Re-enabling reports after reset...\n");
			k_msleep(100);
			enable_report(SH2_ROTATION_VECTOR, BIO_INTERVAL_US);
			enable_report(SH2_GYROSCOPE_CALIBRATED, BIO_INTERVAL_US);
			enable_report(SH2_LINEAR_ACCELERATION, BIO_INTERVAL_US);
			enable_report(SH2_GRAVITY, BIO_INTERVAL_US);
			printk("[bno085] Reports re-enabled\n");
		}

		/* Service aggressively — process multiple packets per loop */
		sh2_service();
		service_count++;

		/* Print heartbeat every 10 seconds */
		uint32_t now = (uint32_t)(k_uptime_get() / 1000);
		if (now != last_print && (now % 10) == 0) {
			last_print = now;
			printk("[bno085] alive: svc=%u data=%u wr=%u INT=%d\n",
			       service_count, data_count, write_count,
			       gpio_pin_get(gpio_dev, BNO_INT_PIN));
		}

		/* Detect data stall — if no new data for 3 seconds, reset BNO085 */
		if (data_count > last_data_count) {
			last_data_count = data_count;
			last_data_time = k_uptime_get();
		} else if ((k_uptime_get() - last_data_time) > 3000 && data_count > 0) {
			printk("[bno085] Data stall detected — resetting...\n");

			sh2_close();

			/* Hardware reset */
			gpio_pin_set(gpio_dev, BNO_RST_PIN, 0);
			k_msleep(10);
			gpio_pin_set(gpio_dev, BNO_RST_PIN, 1);
			k_msleep(300);

			init_complete = false;
			needs_report_setup = false;

			rc = sh2_open(&bno_hal, sh2_event_callback, NULL);
			if (rc != SH2_OK) {
				printk("[bno085] Re-open failed: %d\n", rc);
				k_msleep(1000);
				continue;
			}

			sh2_setSensorCallback(sh2_sensor_callback, NULL);
			enable_report(SH2_ROTATION_VECTOR, BIO_INTERVAL_US);
			enable_report(SH2_GYROSCOPE_CALIBRATED, BIO_INTERVAL_US);
			enable_report(SH2_LINEAR_ACCELERATION, BIO_INTERVAL_US);
			enable_report(SH2_GRAVITY, BIO_INTERVAL_US);

			init_complete = true;
			last_data_count = data_count;
			last_data_time = k_uptime_get();
			write_count = 0;
			printk("[bno085] Reset complete, reports re-enabled\n");
		}

		if (sensor_data_ready) {
			data_count++;
			sensor_data_ready = false;

			switch (latest_value.sensorId) {
			case SH2_ROTATION_VECTOR: {
				/* Quaternion: orientation in 3D space
				 * Values: -1.0 to 1.0, scaled x10000 */
				int32_t qi = (int32_t)(latest_value.un.rotationVector.i * 10000);
				int32_t qj = (int32_t)(latest_value.un.rotationVector.j * 10000);
				int32_t qk = (int32_t)(latest_value.un.rotationVector.k * 10000);
				int32_t qr = (int32_t)(latest_value.un.rotationVector.real * 10000);
				int32_t acc = (int32_t)(latest_value.un.rotationVector.accuracy * 100);

				printk("[imu] Quat: i=%d j=%d k=%d r=%d acc=%d\n",
				       qi, qj, qk, qr, acc);
				break;
			}
			case SH2_GYROSCOPE_CALIBRATED: {
				/* Angular velocity in rad/s, scaled x100 */
				int32_t gx = (int32_t)(latest_value.un.gyroscope.x * 100);
				int32_t gy = (int32_t)(latest_value.un.gyroscope.y * 100);
				int32_t gz = (int32_t)(latest_value.un.gyroscope.z * 100);

				printk("[imu] Gyro: x=%d y=%d z=%d (x100 rad/s)\n",
				       gx, gy, gz);
				break;
			}
			case SH2_LINEAR_ACCELERATION: {
				/* Acceleration without gravity in m/s^2, scaled x100 */
				int32_t lx = (int32_t)(latest_value.un.linearAcceleration.x * 100);
				int32_t ly = (int32_t)(latest_value.un.linearAcceleration.y * 100);
				int32_t lz = (int32_t)(latest_value.un.linearAcceleration.z * 100);

				printk("[imu] LinAcc: x=%d y=%d z=%d (x100 m/s2)\n",
				       lx, ly, lz);
				break;
			}
			case SH2_GRAVITY: {
				/* Gravity direction in m/s^2, scaled x100
				 * At rest: should read ~0,0,981 (9.81 m/s^2 down) */
				int32_t gvx = (int32_t)(latest_value.un.gravity.x * 100);
				int32_t gvy = (int32_t)(latest_value.un.gravity.y * 100);
				int32_t gvz = (int32_t)(latest_value.un.gravity.z * 100);

				printk("[imu] Grav: x=%d y=%d z=%d (x100 m/s2)\n",
				       gvx, gvy, gvz);
				break;
			}
			default:
				break;
			}
		}

		k_msleep(10);
	}
}
