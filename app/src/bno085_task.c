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
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "sh2.h"
#include "sh2_err.h"
#include "sh2_SensorValue.h"

/* SPI device: SPI1 on the FE310 */
static const struct device *spi_dev;

static struct spi_config bno_spi_cfg = {
	.frequency = 3000000,  /* 3 MHz */
	.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |
		     SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs = { .gpio = { 0 } },  /* Use hardware CS (GPIO 2 via IOF) */
};

/* ---- SPI read/write helpers ---- */

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

	printk("[bno085] SPI1 ready, waiting for BNO085 boot...\n");
	k_msleep(500);
	return SH2_OK;
}

static void bno_hal_close(sh2_Hal_t *self)
{
}

static int bno_hal_read(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len,
			uint32_t *t_us)
{
	/* Read 4-byte SHTP header */
	uint8_t hdr[4] = {0};
	if (bno_spi_read(hdr, 4) != 0) {
		k_msleep(10);
		return 0;
	}

	/* Parse packet length (little-endian, mask continuation bit) */
	uint16_t packet_len = (uint16_t)(hdr[0] | (hdr[1] << 8));
	packet_len &= 0x7FFF;

	if (packet_len == 0 || packet_len == 0x7FFF) {
		k_msleep(10);
		return 0;
	}

	if (packet_len > len) {
		packet_len = len;
	}

	/* Read full packet */
	if (bno_spi_read(pBuffer, packet_len) != 0) {
		return 0;
	}

	*t_us = (uint32_t)(k_uptime_get() * 1000);
	return packet_len;
}

static int bno_hal_write(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len)
{
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

static void sh2_event_callback(void *cookie, sh2_AsyncEvent_t *pEvent)
{
	if (pEvent->eventId == SH2_RESET) {
		printk("[bno085] Reset detected\n");
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
	if (rc == SH2_OK && prodIds.numEntries > 0) {
		printk("[bno085] SW Version: %d.%d.%d\n",
		       prodIds.entry[0].swVersionMajor,
		       prodIds.entry[0].swVersionMinor,
		       prodIds.entry[0].swVersionPatch);
	}

	sh2_setSensorCallback(sh2_sensor_callback, NULL);

	rc = enable_report(SH2_ROTATION_VECTOR, 100000);
	if (rc == SH2_OK) {
		printk("[bno085] Rotation vector at 10 Hz\n");
	}

	enable_report(SH2_ACCELEROMETER, 100000);

	printk("[bno085] Reading IMU data...\n");

	while (1) {
		sh2_service();

		if (sensor_data_ready) {
			sensor_data_ready = false;

			if (latest_value.sensorId == SH2_ROTATION_VECTOR) {
				int32_t qi = (int32_t)(latest_value.un.rotationVector.i * 10000);
				int32_t qj = (int32_t)(latest_value.un.rotationVector.j * 10000);
				int32_t qk = (int32_t)(latest_value.un.rotationVector.k * 10000);
				int32_t qr = (int32_t)(latest_value.un.rotationVector.real * 10000);

				printk("[bno085] Quat: i=%d j=%d k=%d r=%d\n",
				       qi, qj, qk, qr);
			} else if (latest_value.sensorId == SH2_ACCELEROMETER) {
				int32_t ax = (int32_t)(latest_value.un.accelerometer.x * 100);
				int32_t ay = (int32_t)(latest_value.un.accelerometer.y * 100);
				int32_t az = (int32_t)(latest_value.un.accelerometer.z * 100);

				printk("[bno085] Accel: x=%d y=%d z=%d\n",
				       ax, ay, az);
			}
		}

		k_msleep(10);
	}
}
