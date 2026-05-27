/**
 * @file eeprom.c
 * @brief AT24C02 EEPROM Driver — bit-bang I2C on PC4(SCL)/PC5(SDA)
 *
 * Original Teknatool firmware uses PC4/PC5 for EEPROM access (confirmed from
 * disassembly 0x801842e). The PB6/PB14/PB15 path found earlier is write-protected.
 */

#include "eeprom.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* REVIEW FIX (MEDIUM): the bit-banged I2C had no bus lock, and settings.c
 * compensated with its OWN mutex — which covered only the settings paths.
 * EEDUMP (task_main, priority 1) and the crash-dump reader took none, so a
 * menu Save from task_ui (priority 2) could preempt mid-transaction: both
 * halves then drive SCL/SDA from different byte positions, the resumed
 * transaction addresses the wrong byte, and a write can land anywhere in the
 * 256-byte array — including the OEM region and the crash-dump record.
 *
 * The lock belongs on the bus, not on one caller, so it lives here and every
 * public entry point takes it. Scheduler-aware because the EEPROM is read
 * during settings_init() before vTaskStartScheduler(); skipped from interrupt
 * context because crash_dump_log() runs from the fault hooks, where taking a
 * mutex is illegal. That one transaction is unprotected by necessity — the
 * machine is already dead by then. */
static SemaphoreHandle_t s_i2c_mutex = NULL;
static StaticSemaphore_t s_i2c_mutex_buf;

/* REVIEW FIX: this is called once from main() before vTaskStartScheduler(), NOT
 * lazily from the entry points. The previous version claimed to be explicit
 * while still doing an unguarded test-then-store on every public call — two
 * tasks reaching it concurrently could both run xSemaphoreCreateMutexStatic()
 * on the same buffer and re-initialise the queue underneath a holder, losing
 * mutual exclusion for the rest of the run. It was unreachable only by an
 * accident of boot order, which is not the same as being fixed. */
void eeprom_lock_init(void) {
    if (!s_i2c_mutex) {
        s_i2c_mutex = xSemaphoreCreateMutexStatic(&s_i2c_mutex_buf);
    }
}

/* REVIEW FIX: the ISR check above covers the fault hooks that run in handler
 * mode, but vApplicationMallocFailedHook() and
 * vApplicationStackOverflowHook() run in TASK context — so crash_dump_log()
 * from there took this mutex with portMAX_DELAY, and if the task that failed
 * was the one already holding it, the hook blocked forever on itself and the
 * crash dump was lost. (The watchdog still resets the board, so it was a lost
 * diagnostic rather than a hang, which is exactly why it would never have been
 * noticed.) A crash context abandons the lock for the same reason the ISR one
 * does: nothing is going to run after us. */
static volatile bool s_crash_context = false;

void eeprom_enter_crash_context(void) {
    s_crash_context = true;
}

static bool i2c_bus_lock(void) {
    if (s_i2c_mutex && !s_crash_context &&
        xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xPortIsInsideInterrupt() == pdFALSE) {
        return xSemaphoreTake(s_i2c_mutex, portMAX_DELAY) == pdTRUE;
    }
    return false;
}

static void i2c_bus_unlock(bool taken) {
    if (taken) {
        xSemaphoreGive(s_i2c_mutex);
    }
}
#include "config.h"
#include <string.h>

extern void uart_puts(const char* s);

static bool eeprom_initialized = false;

/*===========================================================================*/
/* Bit-bang I2C primitives                                                    */
/*===========================================================================*/

static inline void scl_high(void) { HAL_GPIO_WritePin(EEPROM_I2C_PORT, EEPROM_SCL_PIN, GPIO_PIN_SET); }
static inline void scl_low(void)  { HAL_GPIO_WritePin(EEPROM_I2C_PORT, EEPROM_SCL_PIN, GPIO_PIN_RESET); }
static inline void sda_high(void) { HAL_GPIO_WritePin(EEPROM_I2C_PORT, EEPROM_SDA_PIN, GPIO_PIN_SET); }
static inline void sda_low(void)  { HAL_GPIO_WritePin(EEPROM_I2C_PORT, EEPROM_SDA_PIN, GPIO_PIN_RESET); }
static inline int  sda_read(void) { return HAL_GPIO_ReadPin(EEPROM_I2C_PORT, EEPROM_SDA_PIN); }

/* Bus speed multiplier, for diagnosing a device that will not answer at the
 * speed the EEPROM is happy with. 1 = normal (~150-200 kHz). The EEPROM is a
 * very tolerant slave; a part that NAKs at this rate but answers slower would
 * look identical to an absent one from a single scan, and that ambiguity is
 * worth being able to eliminate rather than argue about. Test hook only —
 * nothing in normal operation changes it. */
static volatile uint16_t s_i2c_slow = 1;

void i2c_set_slow_factor(uint16_t factor) {
    s_i2c_slow = (factor == 0) ? 1 : factor;
}

static inline void i2c_delay(void) {
    for (uint16_t r = 0; r < s_i2c_slow; r++) {
        for (volatile int d = 0; d < 60; d++);
    }
}

static inline void i2c_delay_short(void) {
    for (uint16_t r = 0; r < s_i2c_slow; r++) {
        for (volatile int d = 0; d < 40; d++);
    }
}

static void i2c_start(void) {
    sda_high(); scl_high(); i2c_delay();
    sda_low(); i2c_delay();
    scl_low(); i2c_delay_short();
}

static void i2c_stop(void) {
    sda_low(); i2c_delay_short();
    scl_high(); i2c_delay();
    sda_high(); i2c_delay();
}

static int i2c_send_byte(uint8_t byte) {
    for (int bit = 7; bit >= 0; bit--) {
        if (byte & (1 << bit)) sda_high(); else sda_low();
        i2c_delay_short();
        scl_high(); i2c_delay();
        scl_low(); i2c_delay_short();
    }
    sda_high(); i2c_delay_short();
    scl_high(); i2c_delay();
    int ack = !sda_read();
    scl_low(); i2c_delay_short();
    return ack;
}

static uint8_t i2c_recv_byte(int send_ack) {
    uint8_t byte = 0;
    sda_high();
    for (int bit = 7; bit >= 0; bit--) {
        scl_high(); i2c_delay();
        if (sda_read()) byte |= (1 << bit);
        scl_low(); i2c_delay_short();
    }
    if (send_ack) sda_low(); else sda_high();
    i2c_delay_short();
    scl_high(); i2c_delay();
    scl_low(); i2c_delay_short();
    sda_high();
    return byte;
}

/**
 * @brief Read one register from an arbitrary device on this I2C bus.
 *
 * PC4/PC5 is not the EEPROM's private bus — the original Teknatool firmware
 * puts a 3-axis accelerometer on it at 7-bit address 0x1D and reads the
 * vibration sensor through exactly this sequence. This lives here because
 * eeprom.c owns the pins, the timing and the mutex; a second bit-bang driver
 * on the same two pins would race with EEPROM writes.
 *
 * Sequence matches the OEM's byte for byte:
 *   START, addr<<1|W, reg, REPEATED START, addr<<1|R, read+NACK, STOP
 *
 * @param addr7 7-bit device address (0x50 = the AT24C02, 0x1D = accelerometer)
 * @param reg   register index
 * @param value out; untouched on failure
 * @return false if the device did not ACK its address or the register
 */
/**
 * @brief Address-only presence probe: START, addr+W, note ACK, STOP.
 *
 * A scan built on i2c_read_device_reg() cannot distinguish "no device" from
 * "device present but unhappy with that register", and reporting the second as
 * the first would have had us conclude a part was unfitted on the strength of
 * one unlucky register choice. This asks the only question a scan should ask.
 */
bool i2c_probe_device(uint8_t addr7) {
    const bool taken = i2c_bus_lock();
    i2c_start();
    const bool ack = i2c_send_byte((uint8_t)((addr7 << 1) | 0)) != 0;
    i2c_stop();
    i2c_bus_unlock(taken);
    return ack;
}

/**
 * @brief Register read that IGNORES the ACK, exactly as the OEM does.
 *
 * The original firmware's FUN_0800744c never tests the acknowledge — it clocks
 * out a byte and uses it whatever happened. On a board with the accelerometer
 * fitted that is harmless. On one without, it means the vibration logic is fed
 * whatever a floating, pulled-up bus happens to produce, and the OEM's own
 * arithmetic then puts the Y axis at 250 against a MAX-sensitivity threshold
 * of 251 — one count from a false "Significant Vibration".
 *
 * This exists to reproduce that faithfully so the theory can be tested on our
 * firmware instead of reflashing the machine to stock. Diagnostic only:
 * vibration_evaluate() uses the ACK-checking read, because shipping the OEM's
 * bug would be a poor tribute to it.
 */
bool i2c_read_device_reg_noack(uint8_t addr7, uint8_t reg, uint8_t* value) {
    if (!value) return false;
    const bool taken = i2c_bus_lock();
    i2c_start();
    (void)i2c_send_byte((uint8_t)((addr7 << 1) | 0));
    (void)i2c_send_byte(reg);
    i2c_start();
    (void)i2c_send_byte((uint8_t)((addr7 << 1) | 1));
    *value = i2c_recv_byte(0);
    i2c_stop();
    i2c_bus_unlock(taken);
    return true;
}

bool i2c_read_device_reg(uint8_t addr7, uint8_t reg, uint8_t* value) {
    if (!value) return false;

    const bool taken = i2c_bus_lock();

    i2c_start();
    if (!i2c_send_byte((uint8_t)((addr7 << 1) | 0))) { goto fail; }
    if (!i2c_send_byte(reg))                          { goto fail; }

    i2c_start();   /* repeated start */
    if (!i2c_send_byte((uint8_t)((addr7 << 1) | 1))) { goto fail; }

    *value = i2c_recv_byte(0);   /* NACK the last (only) byte */
    i2c_stop();
    i2c_bus_unlock(taken);
    return true;

fail:
    i2c_stop();
    i2c_bus_unlock(taken);
    return false;
}

/*===========================================================================*/
/* EEPROM ACK polling                                                         */
/*===========================================================================*/

static eeprom_status_t wait_for_eeprom(void) {
    for (int i = 0; i < 100; i++) {
        i2c_start();
        if (i2c_send_byte(EEPROM_I2C_ADDR << 1)) {
            i2c_stop();
            return EEPROM_OK;
        }
        i2c_stop();
        for (volatile int d = 0; d < 1000; d++);
    }
    return EEPROM_TIMEOUT;
}

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

bool eeprom_init(void) {
    /* REVIEW FIX: eeprom_init() is a PUBLIC bus entry point and was the one
     * left unlocked, which made the header's "everything goes through these"
     * claim false. It is called at runtime, not only at boot — EEDUMP and a
     * debug command both re-run it from task_main — and it re-inits the pins,
     * clocks SCL nine times and issues a STOP. Preempting a menu Save with
     * that is worse than an interleave: the recovery sequence actively
     * terminates the other task's in-flight write at an arbitrary byte. */
    const bool held = i2c_bus_lock();

    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gp = {0};
    gp.Mode = GPIO_MODE_OUTPUT_OD;
    gp.Pull = GPIO_PULLUP;
    gp.Speed = GPIO_SPEED_FREQ_LOW;
    gp.Pin = EEPROM_SCL_PIN;
    HAL_GPIO_Init(EEPROM_I2C_PORT, &gp);
    gp.Pin = EEPROM_SDA_PIN;
    HAL_GPIO_Init(EEPROM_I2C_PORT, &gp);

    scl_high();
    sda_high();

    for (volatile int d = 0; d < 10000; d++);

    // AUDIT FIX (LOW, eeprom.c:115): 9-clock SDA release. A reset landing
    // mid-transaction can leave the slave driving SDA low. Since the probe
    // reads SDA-low as ACK (`ack = !sda_read()`), a stuck slave would be
    // read as "EEPROM present" and every subsequent read would return 0x00
    // (making the boot silently fall back to defaults + never persist any
    // save). Clock SCL 9 times so the slave sees a full byte-plus-ACK gap
    // and releases the bus, then re-issue START.
    for (int i = 0; i < 9; i++) {
        scl_low();
        for (volatile int d = 0; d < 100; d++);
        scl_high();
        for (volatile int d = 0; d < 100; d++);
    }
    // Issue a STOP condition to reset any partially-clocked transaction.
    sda_low();
    for (volatile int d = 0; d < 100; d++);
    scl_high();
    for (volatile int d = 0; d < 100; d++);
    sda_high();
    for (volatile int d = 0; d < 1000; d++);

    i2c_start();
    int ack = i2c_send_byte(EEPROM_I2C_ADDR << 1);
    i2c_stop();

    if (ack) {
        eeprom_initialized = true;
        uart_puts("[EEPROM] AT24C02 found on PC4/PC5\r\n");
        i2c_bus_unlock(held);
        return true;
    }

    uart_puts("[EEPROM] not found on PC4/PC5\r\n");
    i2c_bus_unlock(held);
    return false;
}

static eeprom_status_t eeprom_read_locked(uint16_t addr, uint8_t* data, size_t len) {
    if (!eeprom_initialized || data == NULL || len == 0) return EEPROM_ERROR;
    if (addr + len > EEPROM_SIZE) return EEPROM_ERROR;

    i2c_start();
    if (!i2c_send_byte(EEPROM_I2C_ADDR << 1)) { i2c_stop(); return EEPROM_ERROR; }
    if (!i2c_send_byte((uint8_t)addr))          { i2c_stop(); return EEPROM_ERROR; }
    i2c_stop();

    for (volatile int d = 0; d < 500; d++);

    i2c_start();
    if (!i2c_send_byte((EEPROM_I2C_ADDR << 1) | 1)) { i2c_stop(); return EEPROM_ERROR; }

    for (size_t i = 0; i < len; i++) {
        data[i] = i2c_recv_byte(i < len - 1);
    }
    i2c_stop();

    return EEPROM_OK;
}

static eeprom_status_t eeprom_write_locked(uint16_t addr, const uint8_t* data, size_t len) {
    if (!eeprom_initialized || data == NULL || len == 0) return EEPROM_ERROR;
    if (addr + len > EEPROM_SIZE) return EEPROM_ERROR;

#ifdef BUILD_READONLY
    /* Read-only demo build: never touch the user's EEPROM. Report success so
     * callers (settings_save dirty-clear, cmd_save) behave normally. */
    (void)data;
    return EEPROM_OK;
#endif

    size_t written = 0;
    while (written < len) {
        uint16_t page_offset = (addr + written) % EEPROM_PAGE_SIZE;
        size_t chunk = EEPROM_PAGE_SIZE - page_offset;
        if (chunk > len - written) chunk = len - written;

        i2c_start();
        if (!i2c_send_byte(EEPROM_I2C_ADDR << 1)) { i2c_stop(); return EEPROM_ERROR; }
        if (!i2c_send_byte((uint8_t)(addr + written))) { i2c_stop(); return EEPROM_ERROR; }

        for (size_t i = 0; i < chunk; i++) {
            if (!i2c_send_byte(data[written + i])) { i2c_stop(); return EEPROM_ERROR; }
        }
        i2c_stop();

        eeprom_status_t status = wait_for_eeprom();
        if (status != EEPROM_OK) return status;

        written += chunk;
    }

    return EEPROM_OK;
}

eeprom_status_t eeprom_read_byte(uint16_t addr, uint8_t* value) {
    return eeprom_read(addr, value, 1);
}

eeprom_status_t eeprom_write_byte(uint16_t addr, uint8_t value) {
    return eeprom_write(addr, &value, 1);
}

/* Public entry points: one bus transaction at a time. Everything that reaches
 * the AT24C02 goes through these — including eeprom_read_byte()/write_byte(),
 * which delegate — so no caller can forget the lock. That is how EEDUMP and
 * the crash-dump reader came to be unprotected while settings.c guarded only
 * its own paths. */
eeprom_status_t eeprom_read(uint16_t addr, uint8_t* data, size_t len) {
    const bool held = i2c_bus_lock();
    const eeprom_status_t r = eeprom_read_locked(addr, data, len);
    i2c_bus_unlock(held);
    return r;
}

eeprom_status_t eeprom_write(uint16_t addr, const uint8_t* data, size_t len) {
    const bool held = i2c_bus_lock();
    const eeprom_status_t r = eeprom_write_locked(addr, data, len);
    i2c_bus_unlock(held);
    return r;
}
