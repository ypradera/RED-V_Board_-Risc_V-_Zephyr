/*
 * BNO085 IMU Task
 *
 * Uses CEVA SH-2 library over I2C with FE310 workarounds:
 *   - I2C_SHIFTED(0x4A) for address bug
 *   - i2c_reinit() before each transaction
 *   - i2c_bus_mutex for shared bus access
 */
#ifndef BNO085_TASK_H
#define BNO085_TASK_H

void bno085_task(void *p1, void *p2, void *p3);

#endif
