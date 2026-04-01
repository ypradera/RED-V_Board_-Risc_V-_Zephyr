# Sensor Driver Reference — Lessons from Arduino Libraries

What we learned by analyzing the SparkFun/Adafruit Arduino source code
and applying it to our Zephyr RTOS implementation on the FE310.

---

## 1. SHTC3 — Temperature & Humidity (I2C)

**Source:** [SparkFun_SHTC3_Arduino_Library](https://github.com/sparkfun/SparkFun_SHTC3_Arduino_Library)

### I2C Address
- 7-bit: `0x70`
- Max clock: 1 MHz

### Command Protocol
Every command is 2 bytes sent MSB-first:

| Command | Hex | Purpose |
|---|---|---|
| Wake | `0x35 0x17` | Wake sensor from sleep |
| Sleep | `0xB0 0x98` | Put sensor back to sleep |
| Read ID | `0xEF 0xC8` | Returns 3 bytes: ID_MSB, ID_LSB, CRC |
| Soft Reset | `0x80 0x5D` | Reset sensor (needs 500µs after) |
| Measure (polling) | `0x78 0x66` | Temp first, no clock stretching, normal power |
| Measure (clock stretch) | `0x5C 0x24` | RH first, clock stretching, normal power |

### Complete Measurement Cycle

The Arduino library follows this exact sequence for every reading:

```
1. WAKE:    [START][0xE0][0x35][0x17][STOP]     → wait 240µs
2. MEASURE: [START][0xE0][0x78][0x66][STOP]     → wait 12ms (polling mode)
3. READ:    [START][0xE1][6 bytes][STOP]
4. SLEEP:   [START][0xE0][0xB0][0x98][STOP]
```

The 6 bytes returned are:
```
[TEMP_MSB] [TEMP_LSB] [TEMP_CRC] [HUM_MSB] [HUM_LSB] [HUM_CRC]
```

### Key Timing
- **240µs** after wake (the sensor needs this to be ready)
- **12ms** measurement time in normal-power polling mode
- **0.8ms** measurement time in low-power polling mode

### Clock Stretching vs Polling
The Arduino library uses **clock stretching mode** exclusively — after sending
the measure command, it calls `Wire.requestFrom()` and the sensor holds SCL low
until data is ready.

**We use polling mode** (`0x7866`) because the FE310's polled I2C driver
busy-waits during clock stretching, blocking all other threads. In polling mode,
we send the measure command, sleep 15ms, then read the result.

### CRC Validation
The Arduino library validates every read with CRC-8:
- Polynomial: `0x31` (x^8 + x^5 + x^4 + 1)
- Init value: `0xFF`
- Input: 2 data bytes (MSB first)
- The computed CRC is compared against the 3rd byte

**Our code skips CRC validation** — this is a future improvement.

### Conversion Formulas
```
Temperature (°C) = -45 + 175 × (raw_temp / 65535)
Humidity (%RH)   = 100 × (raw_hum / 65535)
```

In integer math (no floats on FE310):
```c
int32_t temp_100 = -4500 + (int32_t)17500 * raw_t / 65535;  // hundredths of °C
int32_t hum_100  = (int32_t)10000 * raw_h / 65535;           // hundredths of %RH
```

### What the Arduino Library Does That We Don't
1. CRC validation on every read
2. Auto-sleep management (`startProcess()` / `endProcess()`)
3. ID register validation during init
4. Callback system for error reporting

---

## 2. OPT4048 — Tristimulus Color Sensor (I2C)

**Source:** [SparkFun_OPT4048_Arduino_Library](https://github.com/sparkfun/SparkFun_OPT4048_Arduino_Library)

### I2C Address
- 7-bit: `0x44` (ADDR pin = GND, default on SparkFun board)

### Register Access Pattern
The OPT4048 uses standard I2C register reads — all registers are 16-bit, big-endian:

**Write a register:**
```
[START][addr+W][register][data_MSB][data_LSB][STOP]
```

**Read a register:**
```
[START][addr+W][register][STOP]     ← set register pointer
[START][addr+R][data_MSB][data_LSB][STOP]   ← read data
```

The Arduino library uses **STOP + START** (not repeated START) between the
register pointer write and the data read. This is what our code does too.

### Key Registers

| Address | Name | Purpose |
|---|---|---|
| `0x00-0x01` | CH0 | Channel 0 result (CIE X / red) |
| `0x02-0x03` | CH1 | Channel 1 result (CIE Y / green / lux) |
| `0x04-0x05` | CH2 | Channel 2 result (CIE Z / blue) |
| `0x06-0x07` | CH3 | Channel 3 result (wide / clear) |
| `0x0A` | CONTROL | Configuration (range, conversion time, mode) |
| `0x0B` | INT_CONTROL | Interrupt + burst enable |
| `0x0C` | FLAGS | Status flags (overload, conversion ready) |
| `0x11` | DEVICE_ID | Expected: `0x2084` |

### Configuration (Control Register 0x0A)

```
Bit [15]    : qwake           - Quick wake-up
Bits [13:10]: range           - 0x0C = auto-range (recommended)
Bits [9:6]  : conversion_time - 0x06 = 25ms/channel (100ms total)
Bits [5:4]  : op_mode         - 0x03 = continuous conversion
Bit [3]     : latch           - Interrupt latch mode
```

Our config: `(0x0C << 10) | (0x06 << 6) | (0x03 << 4)` = auto-range, 25ms, continuous.

### Channel Decode (Exponent + Mantissa)

Each channel's raw data spans two 16-bit registers:

```
Register N:     [exponent(4 bits)][result_MSB(12 bits)]
Register N+1:   [result_LSB(8 bits)][counter(4 bits)][CRC(4 bits)]
```

Decode:
```c
uint32_t mantissa = (result_MSB << 8) | result_LSB;  // 20-bit
uint32_t adc_code = mantissa << exponent;              // shift by exponent
```

### Lux Conversion
```
Lux = ADC_Ch1 × 0.00215
```

In integer math:
```c
uint32_t lux_centi = adc_ch1 * 215 / 1000;  // hundredths of lux
```

### Burst Read
The Arduino library's `getAllChannelData()` reads all 4 channels in one
16-byte I2C transaction (registers 0x00-0x07 auto-increment). This requires
setting `i2c_burst=1` in register 0x0B.

**Our code uses burst read** — one `i2c_read(16 bytes)` from register 0x00.

### CIE XYZ Color Matrix (from datasheet)

```
         Ch0           Ch1           Ch2
X:  2.34893e-4   -1.89652e-5    1.20812e-5
Y:  4.07467e-5    1.98958e-4   -1.58848e-5
Z:  9.28619e-5   -1.69740e-5    6.74022e-4
```

```
CIE_x = X / (X + Y + Z)
CIE_y = Y / (X + Y + Z)
```

### What the Arduino Library Does That We Don't
1. Read-modify-write for all register changes (preserves other bits)
2. CRC validation (4-bit CRC per channel, disabled by default)
3. CIE x,y chromaticity calculation
4. CCT (Correlated Color Temperature) via McCamy's formula
5. `i2c_burst` bit set in register 0x0B before burst reads

---

## 3. BNO085 — 9-DOF IMU (SPI)

**Source:** [Adafruit_BNO08x](https://github.com/adafruit/Adafruit_BNO08x)

### Protocol: SHTP over SPI
The BNO085 does NOT use simple register reads. It uses the **Sensor Hub
Transport Protocol (SHTP)** — a packet-based protocol with headers, channels,
and flow control. The CEVA SH-2 library handles this protocol; we only
implement the HAL (Hardware Abstraction Layer).

### SPI Configuration
- **Mode 3** (CPOL=1, CPHA=1)
- **1 MHz** clock speed (Adafruit default)
- MSB first, 8-bit words
- Hardware CS on GPIO 2

### The Critical HAL Functions

The entire interface between the SH-2 library and the hardware is 5 functions:

#### `hal_open` — Initialize and reset
```
1. Configure RST pin as output, INT pin as input
2. Hardware reset: RST HIGH → LOW → HIGH (10ms each)
3. Wait for INT to go LOW (BNO085 ready, up to 500ms)
```

#### `hal_read` — Two-phase SPI read (THE MOST CRITICAL FUNCTION)

This is where every previous attempt failed. The **exact sequence** that works
(matching the Adafruit Arduino library):

```
1. Wait for INT LOW (up to 500ms, auto-reset on timeout)
2. SPI read 4 bytes  ← SHTP header (one CS transaction)
3. Parse packet_size from header bytes 0-1 (mask bit 15)
4. Wait for INT LOW again  ← BNO085 prepares full packet
5. SPI read packet_size bytes  ← full packet (second CS transaction)
```

**Why two transactions:** The BNO085 re-sends the header on each CS assertion.
The first read gets just the header to determine length. The second read gets
the complete packet (header + payload).

**Why wait for INT between reads:** After the first CS deasserts, the BNO085
needs time to prepare the full packet for retransmission. It signals readiness
by pulling INT LOW. Without this wait, the second read gets corrupt data.

**What happens if INT is always LOW:** The `wait_for_int()` returns immediately.
This works because the BNO085 is always ready — it just means we can't use
INT for flow control optimization.

#### `hal_write` — Half-duplex write

```
1. Wait for INT LOW
2. SPI write N bytes (standard spi_write, MISO data discarded)
```

The Adafruit library uses **half-duplex writes** — it does NOT capture MISO
data during writes. Our earlier attempt with a circular buffer to capture
MISO data during writes actually **corrupted the SHTP packet stream** and
caused the BNO085 to stop sending data.

#### `hal_getTimeUs` — Microsecond timestamp

```c
return k_uptime_get() * 1000;  // ms → µs
```

### Sensor Report Configuration

The SH-2 library handles enabling sensor reports via `sh2_setSensorConfig()`.
Each call sends an SHTP command on channel 2 (control). The BNO085 responds
asynchronously with a Get Feature Response confirming the configured rate.

The Arduino library enables all reports **back-to-back with no delays**:
```c
bno08x.enableReport(SH2_ACCELEROMETER);
bno08x.enableReport(SH2_GYROSCOPE_CALIBRATED);
bno08x.enableReport(SH2_ROTATION_VECTOR);
// etc. — no delays between calls
```

Internally, each `enableReport()` calls `sh2_setSensorConfig()` which calls
`opProcess()`, which spins calling `shtp_service()` until the BNO085
acknowledges. So the bus IS serviced between enables automatically.

### Reset Recovery

The BNO085 can reset itself at any time. The SH-2 library detects this via
the `SH2_RESET` async event. The Arduino example handles it:

```c
void loop() {
    if (bno08x.wasReset()) {
        setReports();  // Re-enable all sensor reports
    }
    // ... read data
}
```

Our code does the same — checks `was_reset` flag in the main loop.

### Service Loop

The Arduino example services once per loop iteration with a 10ms delay:
```c
void loop() {
    delay(10);
    if (bno08x.wasReset()) { setReports(); }
    if (!bno08x.getSensorEvent(&sensorValue)) { return; }
    // process data
}
```

Each `getSensorEvent()` calls `sh2_service()` exactly once, which calls
`hal_read()` once. One packet per iteration.

### What Failed in Our Earlier Attempts

| Attempt | What Went Wrong |
|---|---|
| Single 256-byte read | Too small for multi-report cargoes; no header parse |
| Single 1024-byte read | BNO085 overwhelmed by long CS assertion |
| Circular buffer for write-RX | Corrupted SHTP framing; SH-2 library choked |
| Full-duplex spi_transceive | Made things worse (data corruption) |
| No INTN wait between header+body | Second read hit BNO085 before ready |
| 3 MHz SPI speed | Works but 1 MHz matches reference and is safer |
| sh2_service() between enables | Drained FIFO during setup, no data for main loop |

### What Finally Worked
Matching the Adafruit Arduino library **exactly**:
1. Two-phase read with INT wait between header and body
2. Half-duplex write (discard MISO)
3. Blocking INT wait (500ms timeout, auto hardware reset)
4. 1 MHz SPI Mode 3
5. 10ms delay in service loop
6. Back-to-back enable_report calls (library handles servicing)
7. Reset detection via `was_reset` flag

---

## 4. FE310 I2C Driver Bugs (Patched)

The Zephyr `i2c_sifive.c` driver had 4 bugs that we patched directly:

### Bug 1: Missing Address Left-Shift
**Line 90:** `sys_write8((addr | rw_flag), ...)` should be `((addr << 1) | rw_flag)`

The Zephyr I2C API passes 7-bit addresses. The wire format needs the address
left-shifted by 1 with R/W in bit 0. Without the shift, every address on the
wire is wrong — the slave NACKs.

(Zephyr Issue #25713, filed 2020, closed without fix)

### Bug 2: No STOP After NACK
When `send_addr()` detects a NACK, it returned `-EIO` without sending a STOP
condition. The OpenCores I2C controller keeps the bus in START state. Next
transaction attempts START while bus is busy → controller state machine hangs.

**Fix:** Send `SF_CMD_STOP` before returning error.

### Bug 3: No Timeouts
All `while (i2c_sifive_busy(dev)) {}` loops are unbounded. If the bus gets
stuck, the CPU hangs forever with no recovery.

**Fix:** Added `i2c_sifive_wait()` with 100K iteration timeout. On timeout,
forces a STOP and returns `-ETIMEDOUT`.

### Bug 4: Ignored Return Value in read_msg
`i2c_sifive_read_msg()` called `i2c_sifive_send_addr()` without checking
the return value. If the address phase NACKed, it continued reading garbage.

**Fix:** Check return value, propagate error.

### IOF Pinctrl (Still Needed)
The SiFive I2C driver does not configure GPIO 12/13 as I2C pins via IOF.
This is a device tree / pinctrl issue, not a driver bug per se. We still
manually set `IOF_EN` and `IOF_SEL` for GPIO 12/13 in `i2c_bus_init()`.

---

## 5. Summary — What Each Sensor Needs

### SHTC3 (Simplest)
```
I2C write: [0x35][0x17]          → wake, wait 240µs
I2C write: [0x78][0x66]          → measure, wait 15ms
I2C read:  6 bytes               → temp + humidity + CRCs
I2C write: [0xB0][0x98]          → sleep
Convert:   T = -45 + 175*(raw/65535)
           H = 100*(raw/65535)
```

### OPT4048 (Medium)
```
I2C write: [0x0A][config_MSB][config_LSB]  → set continuous mode
I2C write: [0x00]                           → set register pointer to CH0
I2C read:  16 bytes                         → all 4 channels
Decode:    mantissa = (MSB_12bit << 8) | LSB_8bit
           ADC = mantissa << exponent
Convert:   Lux = ADC_Ch1 * 0.00215
```

### BNO085 (Complex — uses CEVA SH-2 library)
```
SPI read 4 bytes                → SHTP header (get packet length)
Wait for INT LOW                → BNO085 ready for full read
SPI read packet_size bytes      → full SHTP packet
Pass to sh2_service()           → library decodes sensor reports
SPI write command bytes         → library sends enable/config commands
Repeat at 10ms intervals        → ~100 service calls/sec
```
