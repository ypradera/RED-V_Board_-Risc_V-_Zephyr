# Loading Zephyr RTOS on the SparkFun RED-V RedBoard (SiFive FE310-G002)

## Overview

The SparkFun RED-V RedBoard uses the **SiFive FE310-G002** RISC-V SoC. It has an
onboard **Segger J-Link OB** debugger (via an NXP K22 ARM Cortex-M4 chip), so
**no external debugger is needed** — just a micro-USB cable.

**Zephyr board target:** `hifive1_revb`
(The RED-V RedBoard shares the same SoC and pinout as the HiFive1 Rev B.
Zephyr does not have a separate board definition for the RED-V RedBoard.)

> **Warning:** Do NOT attempt to reprogram the onboard NXP K22 chip. Doing so
> will permanently destroy the J-Link debugging capability and brick the board's
> debug interface.

---

## What You Need

### Hardware
- SparkFun RED-V RedBoard (DEV-15594)
- Micro-USB cable (data-capable, not charge-only)

### Software Prerequisites

| Tool | Minimum Version | Purpose |
|------|----------------|---------|
| Python | 3.9+ | Required by `west` and Zephyr build scripts |
| CMake | 3.20+ | Build system generator |
| Ninja | any | Fast build backend |
| west | 0.14+ | Zephyr meta-tool for building, flashing, managing workspace |
| 7-Zip | any | Required to extract Zephyr SDK toolchain archives |
| gperf | 3.0+ | Perfect hash function generator (used by Zephyr build) |
| dtc | 1.4.6+ | Device Tree Compiler (used to process hardware descriptions) |
| Zephyr SDK | 1.0.0+ | Contains RISC-V cross-compiler toolchain |
| Segger J-Link Software | 6.46a+ | USB drivers and flash programmer for the onboard J-Link |

---

## Step 1 — Install Core Build Tools (Python, CMake, Ninja, West)

These are the fundamental tools needed before anything else.

### 1.1 — Install Python 3.9+

Download from https://www.python.org/downloads/ and install.

**Critical:** Check **"Add Python to PATH"** during installation.

Verify after install:
```bash
python --version
# Expected: Python 3.12.x (or any 3.9+)
```

### 1.2 — Install CMake 3.20+

Download from https://cmake.org/download/ and install.

**Critical:** Check **"Add CMake to PATH"** during installation.

Verify:
```bash
cmake --version
# Expected: cmake version 3.20+ (we used 4.2.3)
```

### 1.3 — Install Ninja

```bash
pip install ninja
```

Verify:
```bash
ninja --version
# Expected: any version number (we used 1.13.0)
```

### 1.4 — Install West (Zephyr Meta-Tool)

West is the command-line tool that manages Zephyr workspaces, builds firmware,
and flashes boards.

```bash
pip install west
```

Verify:
```bash
west --version
# Expected: West version: v1.5.0 (or similar)
```

---

## Step 2 — Install gperf and dtc (Device Tree Compiler)

These two tools are required by the Zephyr build system. `gperf` generates
perfect hash functions, and `dtc` compiles device tree source files that
describe hardware configuration.

### Windows (using winget — no admin required)

```bash
winget install oss-winget.gperf --accept-source-agreements --accept-package-agreements
winget install oss-winget.dtc --accept-source-agreements --accept-package-agreements
```

After installation, **restart your terminal** so the new PATH entries take
effect.

Verify:
```bash
gperf --version
# Expected: GNU gperf 3.1

dtc --version
# Expected: Version: DTC 1.6.1
```

> **Note:** If `dtc` is not found after restart, it may have been installed to a
> non-standard path. Check:
> `C:\Users\<username>\AppData\Local\Microsoft\WinGet\Packages\oss-winget.dtc_...\usr\bin\dtc.exe`
> and add that directory to your PATH manually.

### Alternative: Windows (using Chocolatey — requires admin)

```powershell
# Run in an elevated (Administrator) PowerShell or CMD:
choco install dtc-msys2 gperf -y
```

### Linux (Ubuntu/Debian)

```bash
sudo apt install device-tree-compiler gperf
```

### macOS

```bash
brew install dtc gperf
```

---

## Step 3 — Install 7-Zip

7-Zip is needed to extract the Zephyr SDK toolchain archives (`.7z` format).

- **Windows:** Download from https://www.7-zip.org/ and install.
  It typically installs to `C:\Program Files\7-Zip\`.
- **Linux:** `sudo apt install p7zip-full`
- **macOS:** `brew install p7zip`

Verify:
```bash
# Windows (Git Bash):
"/c/Program Files/7-Zip/7z.exe" --help | head -2

# Linux/macOS:
7z --help | head -2
```

---

## Step 4 — Install Segger J-Link Software

The RED-V RedBoard has an onboard **Segger J-Link OB** debugger. You need the
J-Link software pack for the USB drivers and flash tools.

### 4.1 — Download and Install

1. Go to https://www.segger.com/downloads/jlink/
2. Download the **"J-Link Software and Documentation Pack"** for your OS.
3. On Windows, run the installer **as Administrator**.
4. Accept all defaults. The installer places files in:
   `C:\Program Files\SEGGER\JLink_V928\` (version may vary).

### 4.2 — Connect the RED-V Board

1. Plug the RED-V RedBoard into your computer via micro-USB.
2. Wait for drivers to install automatically.
3. **Windows:** Open Device Manager → Ports (COM & LPT).
   You should see **two COM ports** listed under "J-Link" (e.g., COM3, COM4).

### 4.3 — Verify J-Link Connection

You can test the connection using the J-Link Commander:

```bash
# Windows (Git Bash):
"/c/Program Files/SEGGER/JLink_V928/JLink.exe" -nogui 1 -if JTAG \
  -speed 4000 -device FE310 -CommanderScript /dev/null -autoconnect 1
```

Expected output should include:
```
Connecting to J-Link via USB...O.K.
Firmware: J-Link OB-K22-SiFive compiled Nov  7 2022 16:21:52
Hardware version: V1.00
VTref=3.300V
```

If you see `Connecting to J-Link via USB...FAILED`, check your USB cable
(make sure it's a data cable, not charge-only).

---

## Step 5 — Download and Set Up the Zephyr SDK

The Zephyr SDK provides the RISC-V cross-compiler needed to build firmware
for the FE310 SoC.

### 5.1 — Download the Minimal SDK

Download the **minimal** SDK bundle from:
https://github.com/zephyrproject-rtos/sdk-ng/releases

For Windows, download: `zephyr-sdk-1.0.0_windows-x86_64_minimal.7z`

### 5.2 — Extract the SDK

Extract to a short path to avoid Windows path-length issues:

```bash
# Extract to your home directory:
cd ~
"/c/Program Files/7-Zip/7z.exe" x zephyr-sdk-1.0.0_windows-x86_64_minimal.7z
```

This creates: `C:\Users\<username>\zephyr-sdk-1.0.0_windows-x86_64_minimal\zephyr-sdk-1.0.0\`

### 5.3 — Download and Install the RISC-V Toolchain

The minimal SDK does not include any toolchains — you must download them
separately. For the RED-V (FE310), you need the **riscv64-zephyr-elf** toolchain.

```bash
SDK_DIR="$HOME/zephyr-sdk-1.0.0_windows-x86_64_minimal/zephyr-sdk-1.0.0"
WGET="$SDK_DIR/hosttools/wget/bin/wget.exe"
CERT="$SDK_DIR/hosttools/wget/etc/ssl/cert.pem"

# Create the gnu toolchains directory
mkdir -p "$SDK_DIR/gnu"
cd "$SDK_DIR/gnu"

# Download the RISC-V toolchain (~125 MB)
URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.0/toolchain_gnu_windows-x86_64_riscv64-zephyr-elf.7z"
"$WGET" --ca-certificate "$CERT" -q --show-progress -N -O toolchain_riscv64.7z "$URL"

# Extract the toolchain (~1.1 GB extracted)
"/c/Program Files/7-Zip/7z.exe" x -o. toolchain_riscv64.7z

# Clean up the archive
rm toolchain_riscv64.7z
```

Verify the toolchain is installed:
```bash
ls "$SDK_DIR/gnu/riscv64-zephyr-elf/bin/" | head -5
# Expected:
# riscv64-zephyr-elf-addr2line.exe
# riscv64-zephyr-elf-ar.exe
# riscv64-zephyr-elf-as.exe
# riscv64-zephyr-elf-c++.exe
# riscv64-zephyr-elf-c++filt.exe
```

### 5.4 — Register the SDK with CMake

This allows the Zephyr build system to automatically find the SDK:

```bash
SDK_DIR="$HOME/zephyr-sdk-1.0.0_windows-x86_64_minimal/zephyr-sdk-1.0.0"
cmake -P "$SDK_DIR/cmake/zephyr_sdk_export.cmake"
```

Expected output:
```
Zephyr-sdk (C:/Users/<username>/zephyr-sdk-1.0.0.../cmake)
has been added to the user package registry in:
HKEY_CURRENT_USER\Software\Kitware\CMake\Packages\Zephyr-sdk
```

---

## Step 6 — Initialize the Zephyr Workspace

The Zephyr workspace contains the Zephyr kernel source, HAL modules, libraries,
and everything needed to build firmware.

### 6.1 — Initialize with West

```bash
cd ~
west init zephyrproject
```

This clones the Zephyr manifest repository (~59,000 files). Takes 1-3 minutes.

### 6.2 — Download All Modules

```bash
cd ~/zephyrproject
west update
```

> **This downloads ~2 GB of data** including HALs for all supported chips,
> libraries, test frameworks, etc. Expect 5-15 minutes depending on your
> internet connection.

### 6.3 — Install Zephyr Python Dependencies

```bash
pip install -r ~/zephyrproject/zephyr/scripts/requirements.txt
```

This installs Python packages needed by the build system (pyelftools, PyYAML,
devicetree tools, etc.).

---

## Step 7 — Set Environment Variables

Before building, you need to set up the environment so the build system can
find the Zephyr source and SDK.

### Option A: Set Manually Each Session

```bash
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.0_windows-x86_64_minimal/zephyr-sdk-1.0.0"
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"
cd ~/zephyrproject/zephyr
source zephyr-env.sh
```

### Option B: Add to Shell Profile (Recommended)

Add these lines to your `~/.bashrc` or `~/.bash_profile`:

```bash
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.0_windows-x86_64_minimal/zephyr-sdk-1.0.0"
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"
```

Then source the Zephyr environment when you need to build:
```bash
cd ~/zephyrproject/zephyr
source zephyr-env.sh
```

---

## Step 8 — Build the Hello World Sample

### 8.1 — Run the Build

```bash
cd ~/zephyrproject/zephyr
west build -b hifive1_revb samples/hello_world
```

### 8.2 — Verify the Build Output

A successful build ends with output like:
```
[128/128] Linking C executable zephyr\zephyr.elf
Memory region         Used Size  Region Size  %age Used
             ROM:       13344 B    3934464 B      0.34%
             RAM:        4208 B        16 KB     25.68%
        IDT_LIST:           0 B         4 KB      0.00%
Generating files from .../build/zephyr/zephyr.elf for board: hifive1_revb/fe310_g002
```

The build artifacts are in `~/zephyrproject/zephyr/build/zephyr/`:
- `zephyr.elf` — ELF binary (for debugging)
- `zephyr.hex` — Intel HEX file (for flashing)
- `zephyr.bin` — Raw binary

### 8.3 — Building Other Samples

To build a different sample, **you must clean the build directory first**:

```bash
rm -rf build/
west build -b hifive1_revb samples/basic/blinky
```

> **Note about Blinky:** The `hifive1_revb` devicetree defines `LED0` on a pin
> that may not match the RED-V RedBoard's onboard LED (which is on **GPIO pin 5**).
> See the [Troubleshooting](#troubleshooting) section for a devicetree overlay fix.

---

## Step 9 — Flash the Firmware to the RED-V Board

### 9.1 — Connect the Board

Plug the RED-V RedBoard into your computer via micro-USB. Make sure J-Link
drivers are installed (Step 4).

### 9.2 — Flash

```bash
west flash
```

The default flash runner for `hifive1_revb` is **JLink**. This matches the
RED-V's onboard J-Link OB debugger, so no extra flags are needed.

Expected output:
```
-- west flash: using runner jlink
-- runners.jlink: reset after flashing requested
-- runners.jlink: JLink version: 9.28
-- runners.jlink: Flashing file: C:\Users\...\build\zephyr\zephyr.hex
```

### 9.3 — What Happens During Flashing

1. West invokes `JLink.exe` with a JTAG connection at 4000 kHz
2. JLink connects to the FE310 SoC via the onboard K22 J-Link OB
3. The `zephyr.hex` file is written to the FE310's flash memory
4. The board is reset automatically after flashing

> **If flashing fails:**
> - Press the **reset button** on the board and retry.
> - On Windows, try running your terminal **as Administrator**.
> - Do NOT use WSL for flashing — use a native Windows terminal (Git Bash, CMD,
>   or PowerShell).

---

## Step 10 — View Serial Output

After flashing `hello_world`, you need a serial terminal to see the output.

### 10.1 — Find the COM Port

**Windows:**
1. Open Device Manager → Ports (COM & LPT)
2. Find the J-Link COM ports (e.g., COM3, COM4)
3. The RED-V creates **two COM ports** — if one shows no output, try the other

**Linux:** Look for `/dev/ttyACM0` or `/dev/ttyACM1`
**macOS:** Look for `/dev/tty.usbmodem*`

### 10.2 — Connect a Serial Terminal

**Settings:** 115200 baud, 8 data bits, No parity, 1 stop bit (8N1)

**Windows — Using PuTTY:**
1. Download PuTTY from https://www.putty.org/
2. Select "Serial" connection type
3. Enter the COM port (e.g., COM3)
4. Set speed to 115200
5. Click "Open"

**Windows — Using Tera Term:**
1. Select Serial → COM port
2. Setup → Serial Port → 115200 baud

**Linux:**
```bash
screen /dev/ttyACM0 115200
# If no output, try: screen /dev/ttyACM1 115200
```

**macOS:**
```bash
screen /dev/tty.usbmodem* 115200
```

### 10.3 — See the Output

Press the **reset button** on the RED-V board. You should see:
```
*** Booting Zephyr OS build v4.3.0-9347-gfce68e83cd13 ***
Hello World! hifive1_revb/fe310_g002
```

> **Note:** Hello World prints once at boot. Press reset again to see it repeated.

---

## Step 11 — Debug with GDB (Optional)

To start a GDB debug session:

```bash
west debug
```

This:
1. Launches `JLinkGDBServer` on port 2331
2. Starts `riscv64-zephyr-elf-gdb` and connects to the server
3. Halts the CPU at the reset vector

You can then use standard GDB commands:
```gdb
(gdb) break main
(gdb) continue
(gdb) next
(gdb) print variable_name
(gdb) info registers
```

---

## Troubleshooting

### Flashing fails or times out
- Press the **reset button** on the board, then retry `west flash`.
- Ensure J-Link Software v6.46a or later is installed.
- On Windows, run the terminal as Administrator.
- Make sure only one application is using the J-Link (close JFlash, IDE debuggers, etc.).

### No serial output
- Try the **other COM port** — the RED-V enumerates two ports.
- Press the reset button after connecting your serial terminal.
- Verify baud rate is **115200**.
- Make sure your USB cable is a **data cable**, not charge-only.

### `west build` fails with "Could NOT find Dtc"
This is a warning, not an error. The Zephyr build system uses its own Python-based
DTS parser and can proceed without the native `dtc` binary. If you still want to
install it:
```bash
winget install oss-winget.dtc --accept-source-agreements --accept-package-agreements
```

### `west build` fails with missing toolchain
Make sure `ZEPHYR_SDK_INSTALL_DIR` is set:
```bash
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.0_windows-x86_64_minimal/zephyr-sdk-1.0.0"
```
And that the RISC-V toolchain exists at:
`$ZEPHYR_SDK_INSTALL_DIR/gnu/riscv64-zephyr-elf/`

### Blinky doesn't blink the onboard LED

Three things must be fixed for the RED-V's built-in LED (blue, GPIO pin 5):

**1. Pin mismatch:** The `hifive1_revb` devicetree defines LED0 on GPIO 19
(HiFive1 Rev B's RGB LED). The RED-V's LED is on GPIO 5.

**2. SPI pin conflict:** SPI1 and SPI2 both claim GPIO pin 5 via the FE310's
IOF (I/O Function) mux. When IOF is enabled on a pin, it overrides GPIO —
so both SPI peripherals must be disabled.

**3. Clock mismatch:** The default `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=16000000`
is wrong. The FE310's `mtime` counter is clocked by the 32.768 kHz RTC
oscillator, not the 16 MHz core clock. This 488x mismatch causes `k_msleep()`
and `k_busy_wait()` to wait hundreds of times longer than expected.

**Fix — devicetree overlay** (`boards/hifive1_revb.overlay`):
```dts
/ {
    leds {
        compatible = "gpio-leds";
        led0: led_0 {
            gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>;
            label = "RED-V Built-in LED";
        };
    };
};

&spi1 {
    status = "disabled";
};

&spi2 {
    status = "disabled";
};
```

**Fix — prj.conf** (add the correct clock frequency):
```
CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=32768
CONFIG_SYS_CLOCK_TICKS_PER_SEC=1000
```

Then rebuild:
```bash
rm -rf build/
west build -b hifive1_revb samples/basic/blinky
west flash
```

### Windows path too long
Extract the Zephyr SDK and workspace to short paths:
- SDK: `C:\zephyr-sdk\`
- Workspace: `C:\zephyrproject\`

### Chocolatey requires admin rights
Use **winget** instead (no admin needed):
```bash
winget install oss-winget.gperf
winget install oss-winget.dtc
```

---

## Quick Reference

| Action | Command |
|--------|---------|
| Set environment | `export ZEPHYR_SDK_INSTALL_DIR=... && source zephyr-env.sh` |
| Build hello_world | `west build -b hifive1_revb samples/hello_world` |
| Build blinky | `west build -b hifive1_revb samples/basic/blinky` |
| Flash | `west flash` |
| Debug | `west debug` |
| Clean build | `rm -rf build/` |
| Serial (Linux) | `screen /dev/ttyACM0 115200` |
| Test J-Link | `JLink.exe -nogui 1 -if JTAG -speed 4000 -device FE310 ...` |

---

## Directory Layout After Setup

```
~/
├── zephyr-sdk-1.0.0_windows-x86_64_minimal/
│   └── zephyr-sdk-1.0.0/
│       ├── cmake/                    # CMake package registration scripts
│       ├── gnu/
│       │   └── riscv64-zephyr-elf/   # RISC-V cross-compiler (gcc, g++, ld, etc.)
│       ├── hosttools/                # wget, OpenOCD, QEMU
│       └── setup.cmd                 # SDK setup script
│
└── zephyrproject/
    ├── .west/                        # West workspace metadata
    ├── zephyr/                       # Zephyr kernel source
    │   ├── boards/sifive/hifive1/    # Board definition for hifive1_revb
    │   ├── samples/                  # Sample applications
    │   │   ├── hello_world/
    │   │   └── basic/blinky/
    │   ├── build/                    # Build output (after west build)
    │   │   └── zephyr/
    │   │       ├── zephyr.elf
    │   │       ├── zephyr.hex        # ← This gets flashed
    │   │       └── zephyr.bin
    │   └── scripts/
    │       └── requirements.txt      # Python dependencies
    └── modules/                      # HALs, libraries, etc.
```

---

## References

- [Zephyr: HiFive1 Rev B Board Docs](https://docs.zephyrproject.org/latest/boards/sifive/hifive1/doc/hifive1_revb.html)
- [SparkFun RED-V Development Guide](https://learn.sparkfun.com/tutorials/red-v-development-guide/all)
- [SparkFun RED-V Hookup Guide](https://learn.sparkfun.com/tutorials/red-v-redboard-hookup-guide/all)
- [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
- [Zephyr SDK Releases](https://github.com/zephyrproject-rtos/sdk-ng/releases)
- [Segger J-Link Downloads](https://www.segger.com/downloads/jlink/)
