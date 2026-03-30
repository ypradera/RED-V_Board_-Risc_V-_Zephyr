/*
 * BNO085 IMU Task — reads rotation vector (quaternion) data
 *
 * Uses the CEVA SH-2 library for SHTP protocol handling.
 * Implements the SH-2 HAL interface using Zephyr's I2C API
 * with FE310 workarounds (address shift + controller re-init).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "sh2.h"
#include "sh2_err.h"
#include "sh2_SensorValue.h"

/* ---- FE310 I2C workarounds (shared with main.c) ---- */

/* BNO085 7-bit I2C address */
#define BNO085_ADDR_7BIT  0x4A
#define BNO085_ADDR_WIRE  (BNO085_ADDR_7BIT << 1)

/* FE310 I2C controller re-init (same as main.c) */
#define I2C0_BASE  0x10016000

static void bno_i2c_reinit(void)
{
	volatile uint8_t *base = (volatile uint8_t *)I2C0_BASE;
	base[0x08] = 0x00;
	k_busy_wait(10);
	base[0x00] = 0x1F;
	base[0x04] = 0x00;
	base[0x08] = 0x80;
	k_busy_wait(10);
}

/* External: shared I2C bus mutex from main.c */
extern struct k_mutex i2c_bus_mutex;

/* I2C device handle */
static const struct device *bno_i2c;

/* ---- SH-2 HAL Implementation ---- */

static int bno_hal_open(sh2_Hal_t *self)
{
	/* I2C is already initialized by the sensor task in main.c */
	bno_i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(bno_i2c)) {
		return SH2_ERR;
	}

	/* Wait for BNO085 to boot */
	k_msleep(300);

	return SH2_OK;
}

static void bno_hal_close(sh2_Hal_t *self)
{
	/* Nothing to do */
}

static int bno_hal_read(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len,
			uint32_t *t_us)
{
	/* Read 4-byte SHTP header first */
	uint8_t hdr[4];

	bno_i2c_reinit();
	k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
	int ret = i2c_read(bno_i2c, hdr, 4, BNO085_ADDR_WIRE);
	k_mutex_unlock(&i2c_bus_mutex);
	k_busy_wait(100);

	if (ret != 0) {
		k_msleep(10); /* Yield to other tasks */
		return 0; /* No data */
	}

	/* Parse packet length (little-endian, mask continuation bit) */
	uint16_t packet_len = (uint16_t)(hdr[0] | (hdr[1] << 8));
	packet_len &= 0x7FFF;

	if (packet_len == 0 || packet_len == 0x7FFF) {
		k_msleep(10); /* Yield to other tasks */
		return 0; /* No data available */
	}

	if (packet_len > len) {
		packet_len = len;
	}

	/* Read full packet (BNO085 re-sends header + payload) */
	bno_i2c_reinit();
	k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
	ret = i2c_read(bno_i2c, pBuffer, packet_len, BNO085_ADDR_WIRE);
	k_mutex_unlock(&i2c_bus_mutex);
	k_busy_wait(100);

	if (ret != 0) {
		return 0;
	}

	*t_us = (uint32_t)(k_uptime_get() * 1000);
	return packet_len;
}

static int bno_hal_write(sh2_Hal_t *self, uint8_t *pBuffer, unsigned len)
{
	bno_i2c_reinit();
	k_mutex_lock(&i2c_bus_mutex, K_FOREVER);
	int ret = i2c_write(bno_i2c, pBuffer, len, BNO085_ADDR_WIRE);
	k_mutex_unlock(&i2c_bus_mutex);
	k_busy_wait(100);

	if (ret != 0) {
		return 0;
	}
	return len;
}

static uint32_t bno_hal_getTimeUs(sh2_Hal_t *self)
{
	return (uint32_t)(k_uptime_get() * 1000);
}

/* ---- SH-2 Callbacks ---- */

static volatile bool sh2_reset_occurred;
static volatile bool sensor_data_ready;
static sh2_SensorValue_t latest_value;

static void sh2_event_callback(void *cookie, sh2_AsyncEvent_t *pEvent)
{
	if (pEvent->eventId == SH2_RESET) {
		sh2_reset_occurred = true;
	}
}

static void sh2_sensor_callback(void *cookie, sh2_SensorEvent_t *pEvent)
{
	sh2_decodeSensorEvent(&latest_value, pEvent);
	sensor_data_ready = true;
}

/* ---- Enable a sensor report ---- */

static int enable_report(uint8_t sensor_id, uint32_t interval_us)
{
	sh2_SensorConfig_t config = {
		.changeSensitivityEnabled = false,
		.wakeupEnabled = false,
		.changeSensitivityRelative = false,
		.alwaysOnEnabled = false,
		.changeSensitivity = 0,
		.reportInterval_us = interval_us,
		.batchInterval_us = 0,
		.sensorSpecific = 0,
	};
	return sh2_setSensorConfig(sensor_id, &config);
}

/* ---- BNO085 Task ---- */

void bno085_task(void *p1, void *p2, void *p3)
{
	static sh2_Hal_t bno_hal;
	int rc;

	printk("[bno085] Starting...\n");

	/* Set up HAL */
	bno_hal.open      = bno_hal_open;
	bno_hal.close     = bno_hal_close;
	bno_hal.read      = bno_hal_read;
	bno_hal.write     = bno_hal_write;
	bno_hal.getTimeUs = bno_hal_getTimeUs;

	/* Open SH-2 connection */
	rc = sh2_open(&bno_hal, sh2_event_callback, NULL);
	if (rc != SH2_OK) {
		printk("[bno085] sh2_open failed: %d\n", rc);
		return;
	}
	printk("[bno085] SH-2 connected!\n");

	/* Read product IDs */
	sh2_ProductIds_t prodIds;
	rc = sh2_getProdIds(&prodIds);
	if (rc == SH2_OK && prodIds.numEntries > 0) {
		printk("[bno085] SW Version: %d.%d.%d\n",
		       prodIds.entry[0].swVersionMajor,
		       prodIds.entry[0].swVersionMinor,
		       prodIds.entry[0].swVersionPatch);
	}

	/* Register sensor callback */
	sh2_setSensorCallback(sh2_sensor_callback, NULL);

	/* Enable rotation vector at 10 Hz (100ms = 100000us) */
	rc = enable_report(SH2_ROTATION_VECTOR, 100000);
	if (rc != SH2_OK) {
		printk("[bno085] Failed to enable rotation vector: %d\n", rc);
	} else {
		printk("[bno085] Rotation vector enabled at 10 Hz\n");
	}

	/* Enable accelerometer at 10 Hz */
	enable_report(SH2_ACCELEROMETER, 100000);

	printk("[bno085] Reading IMU data...\n");

	while (1) {
		/* Service the SH-2 library (processes incoming SHTP packets) */
		sh2_service();

		if (sensor_data_ready) {
			sensor_data_ready = false;

			if (latest_value.sensorId == SH2_ROTATION_VECTOR) {
				/* Quaternion: i, j, k, real */
				int32_t qi = (int32_t)(latest_value.un.rotationVector.i * 10000);
				int32_t qj = (int32_t)(latest_value.un.rotationVector.j * 10000);
				int32_t qk = (int32_t)(latest_value.un.rotationVector.k * 10000);
				int32_t qr = (int32_t)(latest_value.un.rotationVector.real * 10000);

				printk("[bno085] Quat: i=%d j=%d k=%d r=%d (x10000)\n",
				       qi, qj, qk, qr);
			} else if (latest_value.sensorId == SH2_ACCELEROMETER) {
				int32_t ax = (int32_t)(latest_value.un.accelerometer.x * 100);
				int32_t ay = (int32_t)(latest_value.un.accelerometer.y * 100);
				int32_t az = (int32_t)(latest_value.un.accelerometer.z * 100);

				printk("[bno085] Accel: x=%d y=%d z=%d (x100 m/s2)\n",
				       ax, ay, az);
			}
		}

		k_msleep(10); /* Service at ~100 Hz */
	}
}
