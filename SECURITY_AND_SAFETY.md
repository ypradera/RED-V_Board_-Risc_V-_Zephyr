# Zephyr Security, Cryptography & Functional Safety Guide

## Part 1: Production Security

### 1.1 — Security Architecture Layers

```
┌──────────────────────────────────────────────────┐
│                  Application                      │
├──────────────────────────────────────────────────┤
│              Zephyr Security APIs                 │
│    (PSA Crypto, Secure Storage, TLS/DTLS)        │
├──────────────────────────────────────────────────┤
│             Crypto Libraries                      │
│    (mbedTLS, tinycrypt, PSA Crypto Driver)        │
├──────────────────────────────────────────────────┤
│          Hardware Security                        │
│    (MPU, TrustZone, Crypto Accelerator, RNG)     │
├──────────────────────────────────────────────────┤
│          Secure Boot (MCUboot)                    │
│    (Image verification, rollback protection)      │
└──────────────────────────────────────────────────┘
```

### 1.2 — Secure Boot (MCUboot)

MCUboot is the standard bootloader for Zephyr production deployments.

**What it does:**
- Verifies firmware image signature before booting (RSA, ECDSA, Ed25519)
- Supports A/B image slots for rollback protection
- Prevents unauthorized firmware from running
- Supports encrypted images (AES-128/256)

**Boot flow:**
```
Power on → MCUboot runs
    │
    ├── Reads image header from flash slot 0
    ├── Verifies digital signature (public key in bootloader)
    ├── If valid → jumps to application
    ├── If invalid → tries slot 1 (backup)
    └── If both invalid → stays in bootloader (recovery mode)
```

**Key Kconfig options:**
```
CONFIG_BOOTLOADER_MCUBOOT=y          # App is MCUboot-compatible
CONFIG_MCUBOOT_SIGNATURE_KEY_FILE="key.pem"  # Signing key
CONFIG_MCUBOOT_GENERATE_UNSIGNED_IMAGE=n     # Require signing
```

**Why it matters:**
Without secure boot, anyone with physical access can flash malicious firmware.
With secure boot, only firmware signed with YOUR private key can run.

### 1.3 — Trusted Firmware-M (TF-M)

For ARM Cortex-M MCUs with TrustZone (ARMv8-M), Zephyr integrates with TF-M.

**Architecture:**
```
┌─────────────────┐  ┌─────────────────┐
│  Non-Secure      │  │  Secure World    │
│  (Zephyr App)    │  │  (TF-M)          │
│                  │  │                  │
│  - Application   │  │  - Crypto        │
│  - Networking    │──│  - Secure Store  │
│  - Sensors       │  │  - Attestation   │
│                  │  │  - Key Mgmt      │
└─────────────────┘  └─────────────────┘
     Normal World        Secure World
     (can't access        (hardware-
      secure memory)       isolated)
```

**Note:** The FE310 (RISC-V) does NOT have TrustZone.
TF-M is available on: nRF5340, STM32L5, LPC55S69, etc.

### 1.4 — Memory Protection Unit (MPU)

The MPU enforces memory access permissions per thread.

**What it prevents:**
- Thread A writing to Thread B's stack (buffer overflow exploit)
- Application code modifying kernel data
- Executing code from data regions (code injection)

**Zephyr MPU Kconfig:**
```
CONFIG_USERSPACE=y           # Enable user-mode threads
CONFIG_MPU=y                 # Enable MPU
CONFIG_MPU_STACK_GUARD=y     # Guard region at bottom of each stack
```

**Thread isolation example:**
```c
/* Kernel thread — full access */
K_THREAD_DEFINE(kern_tid, 512, kern_task, NULL, NULL, NULL, 5, 0, 0);

/* User thread — restricted access */
K_THREAD_DEFINE(user_tid, 512, user_task, NULL, NULL, NULL, 5,
                K_USER, 0);  /* K_USER flag = runs in user mode */
```

**Note:** FE310 has PMP (Physical Memory Protection) but Zephyr's
PMP support for RISC-V is limited. Full MPU support is available on
ARM Cortex-M3/M4/M33.

### 1.5 — Stack Protection

Even without MPU, Zephyr offers stack overflow detection:

| Method | Config | How it works | Overhead |
|---|---|---|---|
| **Stack sentinel** | `CONFIG_STACK_SENTINEL=y` | Writes canary at stack bottom, checks on context switch | Low |
| **Stack canaries** | `CONFIG_STACK_CANARIES=y` | Compiler inserts checks on function entry/exit | Medium |
| **HW stack guard** | `CONFIG_MPU_STACK_GUARD=y` | MPU triggers fault on stack overflow | Zero (HW) |
| **Stack painting** | `CONFIG_INIT_STACKS=y` | Fills stack with 0xAA, analyzer checks watermark | Debug only |

**Our app uses:** `CONFIG_STACK_SENTINEL=y` (works on FE310).

---

## Part 2: Cryptography

### 2.1 — Crypto Libraries in Zephyr

| Library | Size | Features | Use case |
|---|---|---|---|
| **tinycrypt** | ~10 KB | AES, SHA-256, HMAC, ECC (NIST P-256) | Very constrained MCUs |
| **mbedTLS** | ~50-100 KB | Full TLS/DTLS, X.509, RSA, ECC, AES, SHA | Network-connected devices |
| **PSA Crypto API** | Varies | Standard API, hardware-accelerated backends | Production, portable |

### 2.2 — PSA Crypto API (Recommended for Production)

PSA (Platform Security Architecture) is ARM's standard crypto API.
Zephyr implements it, so your code is portable across MCUs.

```c
#include <psa/crypto.h>

/* Initialize */
psa_crypto_init();

/* Hash (SHA-256) */
psa_hash_operation_t hash_op = PSA_HASH_OPERATION_INIT;
psa_hash_setup(&hash_op, PSA_ALG_SHA_256);
psa_hash_update(&hash_op, data, data_len);
psa_hash_finish(&hash_op, hash_out, sizeof(hash_out), &hash_len);

/* Symmetric encryption (AES-128-CBC) */
psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
psa_set_key_bits(&attr, 128);
psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
psa_set_key_algorithm(&attr, PSA_ALG_CBC_NO_PADDING);
psa_import_key(&attr, key_data, 16, &key_id);

psa_cipher_encrypt(key_id, PSA_ALG_CBC_NO_PADDING,
                   plaintext, plaintext_len,
                   ciphertext, sizeof(ciphertext), &ciphertext_len);

/* Asymmetric signing (ECDSA with P-256) */
psa_sign_message(key_id, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                 message, message_len,
                 signature, sizeof(signature), &sig_len);
```

**Kconfig:**
```
CONFIG_MBEDTLS=y
CONFIG_MBEDTLS_PSA_CRYPTO_C=y
CONFIG_PSA_WANT_ALG_SHA_256=y
CONFIG_PSA_WANT_ALG_ECDSA=y
CONFIG_PSA_WANT_KEY_TYPE_AES=y
```

### 2.3 — Random Number Generation

Secure crypto needs a good random source.

```c
#include <zephyr/random/random.h>

uint8_t random_bytes[32];
sys_rand_get(random_bytes, sizeof(random_bytes));  /* Pseudo-random */

/* For crypto-grade randomness (requires hardware RNG): */
#include <zephyr/drivers/entropy.h>
const struct device *entropy = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
entropy_get_entropy(entropy, random_bytes, sizeof(random_bytes));
```

**Note:** The FE310 does NOT have a hardware RNG.
Production devices should use MCUs with hardware entropy sources
(nRF52, STM32, ESP32 all have them).

### 2.4 — Secure Communication (TLS/DTLS)

For network-connected devices:

```
CONFIG_MBEDTLS=y
CONFIG_MBEDTLS_TLS_VERSION_1_2=y     # TLS 1.2
CONFIG_MBEDTLS_DTLS=y                # For UDP (CoAP, LwM2M)
CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=y
CONFIG_NET_SOCKETS_SOCKOPT_TLS=y     # TLS via socket API
```

### 2.5 — Secure Storage

Store keys and credentials safely:

```
CONFIG_SETTINGS=y                    # Key-value storage
CONFIG_SETTINGS_RUNTIME=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y                        # Non-Volatile Storage
```

```c
#include <zephyr/settings/settings.h>

/* Save a key */
settings_save_one("crypto/device_key", key_data, key_len);

/* Load a key */
settings_load_subtree("crypto");
```

---

## Part 3: Functional Safety (FuSA)

### 3.1 — What is Functional Safety?

Functional safety ensures that a system operates correctly in response to
inputs, including under fault conditions. It's required for:

| Standard | Industry | Safety Levels |
|---|---|---|
| **IEC 61508** | General industrial | SIL 1-4 |
| **ISO 26262** | Automotive | ASIL A-D |
| **IEC 62304** | Medical devices | Class A-C |
| **DO-178C** | Avionics | DAL A-E |
| **EN 50128** | Railway | SIL 0-4 |

### 3.2 — Zephyr's FuSA Efforts

Zephyr has an active **Safety Working Group** pursuing IEC 61508 SIL 3 and
ISO 26262 ASIL D certification.

**Current status (as of 2025):**
- Safety documentation and processes being developed
- MISRA C compliance analysis underway
- Static analysis integration (Coverity, Polyspace, PC-lint)
- Requirements traceability framework
- Safety architecture documentation

**Key safety Kconfig options:**
```
# Runtime checks
CONFIG_ASSERT=y                      # Enable runtime assertions
CONFIG_ASSERT_LEVEL=2                # Assert level (2 = all asserts)
CONFIG_RUNTIME_ERROR_CHECKS=y        # Extra error checking

# Stack protection
CONFIG_STACK_SENTINEL=y
CONFIG_STACK_CANARIES=y
CONFIG_THREAD_STACK_INFO=y

# Watchdog
CONFIG_WATCHDOG=y                    # Hardware watchdog timer
CONFIG_WDT_DISABLE_AT_BOOT=n        # Keep watchdog active

# Fault handling
CONFIG_EXCEPTION_STACK_TRACE=y       # Print stack trace on crash
```

### 3.3 — Safety Architecture Patterns

**Pattern 1: Watchdog Timer (most critical)**

The watchdog resets the system if software hangs.

```c
#include <zephyr/drivers/watchdog.h>

const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

struct wdt_timeout_cfg cfg = {
    .window.max = 5000,          /* 5 second timeout */
    .callback = NULL,            /* NULL = reset on timeout */
    .flags = WDT_FLAG_RESET_SOC, /* Full chip reset */
};

int channel_id = wdt_install_timeout(wdt, &cfg);
wdt_setup(wdt, WDT_OPT_PAUSE_IN_SLEEP);

/* In your main loop — MUST call this periodically */
while (1) {
    do_work();
    wdt_feed(wdt, channel_id);  /* "kick" the watchdog */
}
/* If wdt_feed isn't called within 5s, system resets */
```

**Pattern 2: Redundant Execution**

Run the same calculation twice and compare results:

```c
int32_t calculate_critical_value(int32_t input)
{
    /* Primary calculation */
    int32_t result1 = complex_math(input);

    /* Redundant calculation (different implementation if possible) */
    int32_t result2 = complex_math_v2(input);

    /* Compare results */
    if (result1 != result2) {
        /* FAULT DETECTED — enter safe state */
        enter_safe_state();
    }

    return result1;
}
```

**Pattern 3: Memory Integrity Checking**

Verify critical data hasn't been corrupted:

```c
struct safety_data {
    int32_t  value;
    uint32_t crc;        /* CRC of value */
    int32_t  value_inv;  /* Bitwise inverse of value */
};

void write_safe(struct safety_data *d, int32_t val)
{
    d->value = val;
    d->value_inv = ~val;
    d->crc = crc32(0, (uint8_t *)&val, sizeof(val));
}

bool verify_safe(const struct safety_data *d)
{
    /* Check inverse copy */
    if (d->value != ~d->value_inv) return false;

    /* Check CRC */
    uint32_t expected_crc = crc32(0, (uint8_t *)&d->value, sizeof(d->value));
    if (d->crc != expected_crc) return false;

    return true;
}
```

**Pattern 4: Safe State Machine**

```c
enum system_state {
    STATE_INIT,
    STATE_RUNNING,
    STATE_DEGRADED,    /* Partial failure — limited operation */
    STATE_SAFE,        /* Critical failure — outputs disabled */
};

static enum system_state current_state = STATE_INIT;

void transition(enum system_state new_state)
{
    printk("[safety] State: %d -> %d\n", current_state, new_state);

    switch (new_state) {
    case STATE_SAFE:
        /* Disable all actuators/outputs */
        disable_motor();
        disable_valve();
        set_outputs_to_safe_defaults();
        break;
    case STATE_DEGRADED:
        /* Reduce speed, enable redundant sensors */
        limit_motor_speed(50);
        enable_backup_sensor();
        break;
    default:
        break;
    }
    current_state = new_state;
}
```

### 3.4 — MISRA C Compliance

MISRA C is a set of coding rules required for safety-critical software.
Zephyr is working toward MISRA C:2012 compliance.

**Key MISRA rules that affect your code:**

| Rule | What it requires | Example |
|---|---|---|
| No dynamic memory | Don't use malloc/free | Use k_mem_slab instead |
| No recursion | Functions can't call themselves | Use iterative algorithms |
| No function pointers (advisory) | Minimize indirect calls | Use switch/case instead |
| Initialize all variables | No undefined behavior | `int x = 0;` not `int x;` |
| Check all return values | Handle every error | `if (ret != 0) { handle_error(); }` |
| Single exit point | One return per function | Use goto for cleanup |
| No implicit type conversions | Explicit casts | `(uint32_t)value` |

**Compiler flags for MISRA-like strictness:**
```cmake
target_compile_options(app PRIVATE
    -Wall -Wextra -Werror
    -Wconversion -Wsign-conversion
    -Wcast-align -Wcast-qual
    -Wswitch-enum -Wswitch-default
    -Wuninitialized -Wmaybe-uninitialized
)
```

### 3.5 — FuSA Verification & Validation

| Activity | Tools | Purpose |
|---|---|---|
| **Static analysis** | Coverity, PC-lint, cppcheck | Find bugs without running code |
| **Dynamic testing** | Zephyr ztest, Twister | Automated test execution |
| **Code coverage** | gcov, lcov | Verify all code paths are tested |
| **Requirements tracing** | DOORS, Polarion | Link requirements to tests |
| **FMEA** | Manual analysis | Identify failure modes and effects |
| **Formal verification** | CBMC, SPARK | Mathematically prove correctness |

### 3.6 — IEC 61508 SIL Requirements

| SIL | Failure Rate | Code Coverage | What it means |
|---|---|---|---|
| SIL 1 | < 10^-5 /hr | Statement | Low risk (e.g., building automation) |
| SIL 2 | < 10^-6 /hr | Branch | Medium risk (e.g., industrial control) |
| SIL 3 | < 10^-7 /hr | MC/DC | High risk (e.g., emergency shutdown) |
| SIL 4 | < 10^-8 /hr | Formal | Very high risk (e.g., nuclear) |

**MC/DC** = Modified Condition/Decision Coverage — every condition in every
decision must independently affect the outcome.

---

## Part 4: Production Checklist

### Before shipping a Zephyr product:

**Security:**
- [ ] Secure boot enabled (MCUboot + signed images)
- [ ] All debug interfaces disabled (JTAG, SWD) or locked
- [ ] Firmware encryption enabled
- [ ] Crypto keys stored in secure element (not flash)
- [ ] TLS/DTLS for all network communication
- [ ] No default passwords or keys
- [ ] Stack overflow protection enabled
- [ ] Input validation on all external interfaces

**Functional Safety:**
- [ ] Watchdog timer enabled and fed from main loop
- [ ] Safe state defined and reachable from all states
- [ ] All return values checked
- [ ] Memory integrity verification for critical data
- [ ] Redundant calculations for safety-critical outputs
- [ ] MISRA C compliance verified (static analysis)
- [ ] 100% branch coverage on safety-critical code
- [ ] FMEA completed for all failure modes
- [ ] Requirements traced to test cases

**General:**
- [ ] Power-on self-test (POST) implemented
- [ ] Error logging to non-volatile storage
- [ ] OTA update mechanism tested (including rollback)
- [ ] EMC/ESD testing passed
- [ ] Temperature range testing completed
- [ ] Long-duration stress testing (72+ hours)

---

## References

- [Zephyr Security Overview](https://docs.zephyrproject.org/latest/security/index.html)
- [Zephyr Safety Certification](https://docs.zephyrproject.org/latest/safety/index.html)
- [MCUboot Documentation](https://docs.mcuboot.com/)
- [PSA Crypto API Specification](https://armmbed.github.io/mbed-crypto/html/)
- [IEC 61508 Summary](https://www.iec.ch/functionalsafety)
- [ISO 26262 Overview](https://www.iso.org/standard/68383.html)
- [MISRA C:2012 Guidelines](https://www.misra.org.uk/misra-c/)
- [Zephyr mbedTLS Integration](https://docs.zephyrproject.org/latest/connectivity/networking/api/tls_credentials.html)
- [Zephyr Safety Working Group](https://github.com/zephyrproject-rtos/zephyr/wiki/Safety-Working-Group)
