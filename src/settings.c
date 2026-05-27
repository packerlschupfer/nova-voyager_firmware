/**
 * @file settings.c
 * @brief Persistent Settings Implementation
 *
 * Storage priority:
 *   1. I2C EEPROM (matches original Teknatool hardware)
 *   2. Flash fallback (if EEPROM not present)
 */

#include "settings.h"
#include "eeprom.h"
#include "eeprom_layout.h"
#include "settings_pack.h"
#include "speed_autosave.h"
#include "shared.h"
#include "motor.h"
#include "tapping.h"
#include <string.h>
#include <stddef.h>

// Forward declarations
extern void motor_set_thermal_threshold(uint8_t threshold_c);
extern void motor_set_vibration_sensitivity(uint8_t level);
extern void print_num(int32_t n);

/*===========================================================================*/
/* Private Variables                                                         */
/*===========================================================================*/

static settings_t current_settings;
static bool dirty = false;
static bool use_eeprom = false;     // True if EEPROM detected

/*===========================================================================*/
/* Flash Operations                                                          */
/*===========================================================================*/

static uint16_t calc_checksum(const settings_t* s) {
    // Simple CRC16-CCITT
    const uint8_t* data = (const uint8_t*)s;
    size_t len = offsetof(settings_t, checksum);
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static bool flash_unlock(void) {
    HAL_FLASH_Unlock();
    return true;
}

static void flash_lock(void) {
    HAL_FLASH_Lock();
}

/* Only reachable from flash_write_settings(), whose body is compiled out in
 * the read-only demo build — so these two are legitimately unused there. Say so
 * rather than letting the demo env carry warnings the other eight do not; the
 * -Wno-unused-function that used to hide this class of thing across every build
 * was removed on purpose. */
#ifdef BUILD_READONLY
__attribute__((unused))
#endif
static bool flash_erase_page(uint32_t addr) {
    FLASH_EraseInitTypeDef erase;
    uint32_t error = 0;

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = addr;
    erase.NbPages = 1;

    if (HAL_FLASHEx_Erase(&erase, &error) != HAL_OK) {
        return false;
    }
    return (error == 0xFFFFFFFF);  // No error
}

#ifdef BUILD_READONLY
__attribute__((unused))
#endif
static bool flash_write_halfword(uint32_t addr, uint16_t data) {
    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, data) == HAL_OK;
}

_Static_assert(sizeof(settings_t) % 2 == 0, "settings_t must be even-sized for halfword flash writes");

static bool flash_write_settings(const settings_t* s) {
#ifdef BUILD_READONLY
    /* Read-only demo build: never write flash. Claim success; nothing persists. */
    (void)s;
    return true;
#else
    const uint16_t* src = (const uint16_t*)s;
    uint32_t addr = SETTINGS_FLASH_ADDR;
    size_t count = sizeof(settings_t) / 2;

    // Erase the page first
    if (!flash_erase_page(SETTINGS_FLASH_ADDR)) {
        return false;
    }

    // Write halfword by halfword (STM32F103 minimum)
    for (size_t i = 0; i < count; i++) {
        if (!flash_write_halfword(addr, src[i])) {
            return false;
        }
        addr += 2;
    }

    return true;
#endif  // BUILD_READONLY
}

static void flash_read_settings(settings_t* s) {
    memcpy(s, (void*)SETTINGS_FLASH_ADDR, sizeof(settings_t));
}

/*===========================================================================*/
/* EEPROM Operations                                                          */
/*===========================================================================*/

/* AUDIT FIX (MEDIUM, settings.c:119): `hi` and `lo` were uninitialised and both
 * status returns were dropped. eeprom_read() bails on an address-phase NACK
 * WITHOUT writing the caller's buffer (eeprom.c:155), so a transient bus error
 * made this return indeterminate stack contents — which eeprom_load_oem_into()
 * then fed straight into its range tests. Garbage that happened to land inside
 * [500, 5500] was accepted as a genuine max_limit / default_rpm / favourite and
 * written back on the next save as though the operator had chosen it.
 *
 * @param ok  Set false if either byte could not be read; left alone otherwise,
 *            so a caller can thread one flag through several reads. */
static uint16_t ee_read16(uint16_t addr, bool* ok) {
    uint8_t hi = 0, lo = 0;
    if (eeprom_read_byte(addr, &hi) != EEPROM_OK ||
        eeprom_read_byte(addr + 1, &lo) != EEPROM_OK) {
        if (ok) *ok = false;
        return 0;
    }
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

/* AUDIT FIX (MEDIUM, settings.c:131): both writes discarded their status, and
 * a 16-bit value goes out as two independent 5 ms write cycles — it is not
 * atomic. The OEM region carries no checksum of its own, so a NACK or a power
 * cut between the high and low byte of, say, EE_MAX_SPEED left a torn value
 * like 0x15A6 = 5542, which passes eeprom_load_oem_into()'s range test and
 * comes back as the operator's speed cap on the next boot.
 *
 * Tearing cannot be fixed without a checksum over the OEM layout, which is the
 * original firmware's and not ours to change. What can be fixed is silence:
 * report the failure so settings_save() stops claiming success.
 *
 * @return false if either byte was not acknowledged. */
static bool ee_write16(uint16_t addr, uint16_t val) {
    bool ok = eeprom_write_byte(addr, (val >> 8) & 0xFF) == EEPROM_OK;
    ok = (eeprom_write_byte(addr + 1, val & 0xFF) == EEPROM_OK) && ok;
    return ok;
}

static bool eeprom_save_to_oem(const settings_t* s) {
    bool ok = ee_write16(EE_MAX_SPEED, s->speed.max_limit);
    ok = ee_write16(EE_DEFAULT_RPM, s->speed.default_rpm) && ok;
    ok = (eeprom_write_byte(EE_PROFILE_IDX, s->motor.profile) == EEPROM_OK) && ok;
    for (int i = 0; i < 8; i++) {
        ok = ee_write16(EE_PRESETS_BASE + i * 4, s->speed.favorite[i]) && ok;
    }
    // AUDIT FIX (MEDIUM, settings.c:131): stamp the OEM validity magic. The
    // loader (eeprom_load_oem_into) bails at line 237 unless byte 0x02 == 0x7C,
    // so on any EEPROM not pre-initialized by the original Teknatool firmware
    // the entire OEM region was write-only — max_limit, default_rpm, profile,
    // and 8 favorites reverted to defaults after every power cycle.
    ok = (eeprom_write_byte(EE_OEM_MAGIC, 0x7C) == EEPROM_OK) && ok;
    return ok;
}

static bool eeprom_save_custom(const settings_t* s) {
    /* AUDIT FIX (HIGH, settings.c:146): the field-by-field serialisation used
     * to live here and omitted 39 settings, including every jam and
     * belt-break threshold. It now lives in include/settings_pack.h, where
     * test/test_settings_pack round-trips each field individually — a test is
     * the only thing that can notice an omission, and there was none. */
    eeprom_custom_t c;
    settings_pack_custom(s, &c);
    return eeprom_write(EE_CUSTOM_BASE, (const uint8_t*)&c, sizeof(c)) == EEPROM_OK;
}

/**
 * @brief Load the custom block, if there is a valid one.
 *
 * AUDIT FIX (HIGH, settings.c:208): a single bad byte on the bit-banged I2C
 * read failed the checksum, and settings_init() then immediately rewrote the
 * block with factory defaults — one transient error destroyed the operator's
 * tuning instead of being retried. The read is now retried before the blob is
 * declared bad, and a rejected blob is left on the chip untouched (see
 * settings_init) so the next boot gets another chance at it.
 */
static bool eeprom_load_custom(settings_t* s) {
    for (int attempt = 0; attempt < 2; attempt++) {
        eeprom_custom_t c;
        if (eeprom_read(EE_CUSTOM_BASE, (uint8_t*)&c, sizeof(c)) != EEPROM_OK) {
            continue;
        }
        if (settings_unpack_custom(&c, s)) {
            return true;
        }
        /* A wrong-version blob is not a transient error — do not retry it. */
        if (c.magic == EE_CUSTOM_MAGIC_VALUE && c.version != EE_CUSTOM_VERSION_NUM) {
            uart_puts("Settings: EEPROM block is layout v");
            print_num(c.version);
            uart_puts(", firmware expects v");
            print_num(EE_CUSTOM_VERSION_NUM);
            uart_puts(" - using defaults (block left intact)\r\n");
            return false;
        }
    }
    return false;
}

/* Every value here is range-checked before it is accepted, and — since the
 * ee_read16 fix — a failed read is skipped rather than being range-checked as
 * indeterminate stack contents. A read failure leaves the compiled default in
 * place, which is the safe direction: a wrong max-speed cap is worse than a
 * conservative one. */
static void eeprom_load_oem_into(settings_t* s) {
    uint8_t magic = 0;
    if (eeprom_read_byte(EE_OEM_MAGIC, &magic) != EEPROM_OK) return;
    if (magic != 0x7C) return;

    bool ok;
    uint16_t v;

    ok = true;
    v = ee_read16(EE_MAX_SPEED, &ok);
    /* REVIEW FIX (HIGH): this was a hard-coded 500 floor where every
     * neighbouring field uses SPEED_MIN_RPM (50), the write side has no floor
     * at all, and both settings_set_max_speed() and the menu row allow 50
     * upward. Setting Max to 400 RPM for a big forstner bit, saving, and power
     * cycling (OFF is wired to NRST) read the stored value back, discarded it,
     * and silently restored the 5500 cap the encoder could then dial up to. */
    if (ok && v >= SPEED_MIN_RPM && v <= SPEED_MAX_RPM) s->speed.max_limit = v;

    ok = true;
    v = ee_read16(EE_DEFAULT_RPM, &ok);
    if (ok && v >= SPEED_MIN_RPM && v <= SPEED_MAX_RPM) s->speed.default_rpm = v;

    for (int i = 0; i < 8; i++) {
        ok = true;
        v = ee_read16(EE_PRESETS_BASE + i * 4, &ok);
        if (ok && v >= SPEED_MIN_RPM && v <= SPEED_MAX_RPM) s->speed.favorite[i] = v;
    }

    /* REVIEW FIX: this accepted any value that was not 0 or -1. depth.offset
     * is an ADC reading (task_depth.c: calibration_offset = raw_adc), so it
     * cannot be negative or above the 12-bit full scale — and it is subtracted
     * from every sample, so a garbage byte pair out of the EEPROM shifted the
     * whole depth axis by an arbitrary amount with no way to notice. */
    ok = true;
    int16_t depth_off = (int16_t)ee_read16(EE_DEPTH_CAL_B, &ok);
    if (ok && depth_off > 0 && depth_off <= 4095) s->depth.offset = depth_off;

    uint8_t prof = 0;
    if (eeprom_read_byte(EE_PROFILE_IDX, &prof) == EEPROM_OK && prof <= 2) {
        s->motor.profile = prof;
    }
}

/*===========================================================================*/
/* Default Settings                                                          */
/*===========================================================================*/

static void set_defaults(settings_t* s) {
    // Just set magic and version - minimal test
    s->magic = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;

    // Motor defaults - actual factory values from MCB EEPROM
    // These MUST be non-zero or motor won't start on cold boot!
    s->motor.speed_kprop = 100;      // Factory: 100
    s->motor.speed_kint = 50;        // Factory: 50
    s->motor.voltage_kp = 2000;      // Factory: 2000 (CRITICAL for motor start!)
    s->motor.voltage_ki = 9000;      // Factory: 9000 (CRITICAL for motor start!)
    s->motor.ir_gain = 28835;        // Factory: 28835
    s->motor.ir_offset = 400;        // Factory: 400
    s->motor.advance_max = 85;       // Factory: 85
    s->motor.pulse_max = 185;        // Factory: 185 ✓
    s->motor.current_limit = 100;    // Factory: 100% ✓
    s->motor.profile = MOTOR_PROFILE_NORMAL;
    s->motor.speed_ramp = 1000;      // Factory: 1000 RPM/s (NOT 500!)
    s->motor.torque_ramp = 75;       // Factory: 75 (NOT 500!)

    // Speed defaults (CG variant: 250-5500 RPM)
    s->speed.default_rpm = SPEED_DEFAULT_RPM;
    s->speed.favorite[0] = 500;
    s->speed.favorite[1] = 1000;
    s->speed.favorite[2] = 1500;
    s->speed.favorite[3] = 2000;
    s->speed.favorite[4] = 2500;
    s->speed.favorite[5] = 3500;
    s->speed.favorite[6] = 4500;
    s->speed.favorite[7] = 5500;
    s->speed.max_limit = SPEED_MAX_RPM;
    s->speed.slow_start = 400;
    s->speed.anti_tearout = 250;
    s->speed.step_size = 50;  // Coarse
    s->speed.rounding = true;

    // Material-based RPM defaults
    s->speed.material = 0;       // Softwood default
    s->speed.bit_type = 0;       // Twist bit default
    s->speed.bit_diameter = 10;  // 10mm default bit
    s->speed.auto_rpm = false;   // Manual RPM by default

    // Tapping trigger defaults (all disabled initially for safety)
    s->tapping.depth_trigger_enabled = false;
    s->tapping.load_increase_enabled = false;
    s->tapping.load_slip_enabled = false;
    s->tapping.clutch_slip_enabled = false;
    s->tapping.quill_trigger_enabled = false;
    s->tapping.peck_trigger_enabled = false;
    s->tapping.pedal_enabled = false;

    s->tapping.speed_rpm = SPEED_TAP_DEFAULT;

    // Per-trigger defaults
    /* STOP, matching the legacy depth_action default this replaces — the
     * machine stops at target depth today and that should not change silently. */
    s->tapping.depth_completion_action = COMPLETION_STOP;
    s->tapping.quill_pedal_mode = QUILL_PEDAL_OFF;
    /* RESUME preserves the interactive quill: lift reverses, push cuts again. */
    s->tapping.quill_completion_action = COMPLETION_RESUME;
    s->tapping.load_increase_threshold = TAP_DEFAULT_LOAD_INCREASE_THRESHOLD;
    s->tapping.load_increase_reverse_ms = TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS;
    /* RESUME = back off by load_increase_reverse_ms and keep cutting, which is
     * the behaviour this trigger was given when the duration was wired up. */
    s->tapping.load_completion_action = COMPLETION_RESUME;
    s->tapping.load_slip_cv_percent = TAP_DEFAULT_LOAD_SLIP_CV_PERCENT;
    s->tapping.load_slip_completion_action = COMPLETION_REVERSE_OUT;
    s->tapping.clutch_plateau_ms = TAP_DEFAULT_CLUTCH_PLATEAU_MS;
    s->tapping.clutch_action = CLUTCH_ACTION_REVERSE;
    s->tapping.peck_fwd_ms = TAP_DEFAULT_PECK_FWD_MS;
    s->tapping.peck_rev_ms = TAP_DEFAULT_PECK_REV_MS;
    s->tapping.peck_cycles = TAP_DEFAULT_PECK_CYCLES;
    s->tapping.peck_depth_stop = true;
    s->tapping.peck_completion_action = COMPLETION_REVERSE_OUT;
    s->tapping.peck_reverse_out_ms = 2000;  // 2s reverse-out
    s->tapping.pedal_action = PEDAL_ACTION_HOLD;
    s->tapping.pedal_chip_break_ms = TAP_DEFAULT_PEDAL_CHIP_BREAK_MS;
    s->tapping.brake_delay_ms = TAP_DEFAULT_BRAKE_DELAY;

    // Depth defaults
    s->depth.mode = DEPTH_MODE_OFF;
    s->depth.action = DEPTH_ACTION_STOP;
    s->depth.target = 0;
    s->depth.offset = 0;
    s->depth.enabled = false;

    // Step drill defaults (example: 12-step 6-42mm drill)
    s->step_drill.enabled = false;
    s->step_drill.start_diameter = 6;      // 6mm starting diameter
    s->step_drill.diameter_increment = 3;   // 3mm increase per step
    s->step_drill.step_depth_x2 = 11;      // 5.5mm step depth (11 * 0.5mm)
    s->step_drill.base_rpm = 1500;         // 1500 RPM at 6mm
    s->step_drill.target_diameter = 0;     // 0 = disabled, or set target diameter to auto-stop

    // Display defaults
    s->display.units = UNITS_METRIC;

    // Sensor defaults
    s->sensor.jam_detect = true;
    s->sensor.spike_detect = true;
    s->sensor.vibration_sensitivity = 3;  // 0=OFF, 1=LOW, 2=MED, 3=HIGH (default HIGH)
    s->sensor.vibration_thresh = 800;     // Legacy
    s->sensor.spike_thresh = 90;
    s->sensor.step_thresh = 20;           // OEM-style: +20% KR step trips jam
    s->sensor.low_load_detect = true;
    s->sensor.low_load_thresh = 5;        // KR<5% + CV<25 RPM = belt break / tool detach
    s->sensor.stall_sensitivity = 50;     // 50% default stall sensitivity
    s->sensor.stall_time_ms = 500;        // 500ms stall detection time
    s->sensor.guard_check_enabled = true; // Chuck guard safety (default: ON)
    s->sensor.pedal_enabled = true;       // Foot pedal (default: ON)
    s->sensor.overload_threshold = 50;    // MCB overload trip point (LD register, factory default)

    // Interface defaults
    s->interface.key_sound = false;  // Button beeps off by default
    s->interface.show_shortcuts = true;
    s->interface.f1_function = 0;  // Default function
    s->interface.f2_function = 0;
    s->interface.f3_function = 0;
    s->interface.f4_function = 0;
    s->interface.menu_locked = false;
    s->interface.password = 0;     // No password

    // Power defaults
    s->power.braking_enabled = false;  // Brake OFF by default (prevents motor overheating!)
    s->power.spindle_hold = false;
    s->power.power_limit = 100;    // 100% (legacy)
    s->power.power_output = 2;     // High (70%) - default from Teknatool manual
    s->power.low_voltage_thresh = 180;  // 18.0V
    s->power.dc_bus_voltage = 3600;     // 360.0V DC bus (from PDF manual)
    s->power.temp_threshold = 60;       // 60°C temperature limit (from PDF)
    s->power.self_start = false;        // Self-start off by default
    s->power.pilot_hole = false;        // Pilot hole mode off by default

    s->checksum = calc_checksum(s);
}

// Migrate old mode-based settings to new trigger-based settings
// This is called when settings version doesn't match current version
/* migrate_tapping_settings() lived here. Removed with the v1->v2 migration it
 * served — see the note in settings_init(). It only re-defaulted the tapping
 * sub-struct anyway, which set_defaults() already does. */

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

/* REVIEW FIX: a plain flag cannot serialise these. The autosave poll runs in
 * task_main (prio 1) and settings_save() is reached from the menu in task_ui
 * (prio 2), so task_ui preempts at any instruction: the poll can test the flag
 * as clear, be preempted while SAVE stamps the CRC and begins writing, then
 * resume and mutate default_rpm after the checksum was taken — the very
 * failure the flag was added to prevent. It is also not just the struct: both
 * paths drive the bit-banged I2C in eeprom.c, which has no bus lock and whose
 * i2c_delay() is a busy-wait, so a preemption mid-byte leaves SCL/SDA in an
 * arbitrary state and the resumed transaction writes to a mis-addressed byte.
 *
 * A real mutex covers both. It is held across the ~300 ms of EEPROM writes,
 * which is fine — it is a mutex, not a critical section, so everything except
 * another settings writer keeps running. */
/* Written only by settings_save(), under s_settings_mutex — see the comment
 * there for why the save serialises a copy rather than the live struct. */
static settings_t s_save_snapshot;

static SemaphoreHandle_t s_settings_mutex = NULL;
static StaticSemaphore_t s_settings_mutex_buf;

static void settings_lock(void) {
    if (s_settings_mutex && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    }
}

/* Non-blocking variant for callers that must not stall — see the autosave
 * poll. Returns false if a save is holding the mutex. */
static bool settings_try_lock(void) {
    if (!s_settings_mutex || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return true;
    }
    return xSemaphoreTake(s_settings_mutex, 0) == pdTRUE;
}

static void settings_unlock(void) {
    if (s_settings_mutex && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreGive(s_settings_mutex);
    }
}

void settings_init(void) {
    if (!s_settings_mutex) {
        s_settings_mutex = xSemaphoreCreateMutexStatic(&s_settings_mutex_buf);
    }

    use_eeprom = eeprom_init();
    extern void uart_puts(const char* s);
    uart_puts(use_eeprom ? "Settings: EEPROM\r\n" : "Settings: Flash\r\n");

    // Try loading settings
    settings_t loaded;
    bool loaded_ok = false;

    if (use_eeprom) {
        /* Layering, weakest source first:
         *   1. compiled-in defaults
         *   2. the full struct mirrored to flash by the last idle SAVE — this
         *      is the only source for the fields too big for the EEPROM block
         *   3. the OEM EEPROM fields
         *   4. the custom EEPROM block, which is written on every SAVE
         *      including mid-cut ones and so is the freshest for what it holds
         * See the mirror in settings_save(). */
        set_defaults(&loaded);

        settings_t mirrored;
        flash_read_settings(&mirrored);
        if (mirrored.magic == SETTINGS_MAGIC &&
            mirrored.version == SETTINGS_VERSION &&
            mirrored.checksum == calc_checksum(&mirrored)) {
            memcpy(&loaded, &mirrored, sizeof(settings_t));
            uart_puts("Settings: flash mirror valid\r\n");
        }

        eeprom_load_oem_into(&loaded);
        if (eeprom_load_custom(&loaded)) {
            uart_puts("Settings: EEPROM loaded OK\r\n");
            memcpy(&current_settings, &loaded, sizeof(settings_t));
            dirty = false;
            return;
        }
        /* AUDIT FIX (HIGH, settings.c:208): this used to call
         * eeprom_save_custom() right here, which overwrote a blob that had
         * merely failed to validate — so a transient I2C bit error, or a
         * firmware upgrade that changed the layout, destroyed the operator's
         * settings on the spot with nothing but "No custom block" in the log.
         * Rejection is now non-destructive: run on defaults, leave the bytes
         * alone, and let the operator decide by pressing Save. */
        uart_puts("Settings: No usable custom block, using OEM + defaults\r\n");
        uart_puts("Settings: existing EEPROM block left intact - SAVE to overwrite\r\n");
        memcpy(&current_settings, &loaded, sizeof(settings_t));
        dirty = true;
        return;
    }

    // Flash path: load and validate
    flash_read_settings(&loaded);
    loaded_ok = true;

    if (loaded_ok &&
        loaded.magic == SETTINGS_MAGIC &&
        loaded.version == SETTINGS_VERSION) {
        uint16_t expected_crc = calc_checksum(&loaded);
        if (loaded.checksum == expected_crc) {
            uart_puts("Settings: CRC valid\r\n");
            memcpy(&current_settings, &loaded, sizeof(settings_t));
            dirty = false;
            return;
        } else {
            uart_puts("Settings: CRC mismatch! Using defaults.\r\n");
        }
    } else {
        if (!loaded_ok) {
            uart_puts("Settings: Load failed. Using defaults.\r\n");
        } else if (loaded.magic != SETTINGS_MAGIC) {
            uart_puts("Settings: Bad magic. Using defaults.\r\n");
        } else if (loaded.version != SETTINGS_VERSION) {
            uart_puts("Settings: stored layout is v");
            print_num(loaded.version);
            uart_puts(", firmware expects v");
            print_num(SETTINGS_VERSION);
            uart_puts("\r\n");

            /* REVIEW FIX + AUDIT FIX (MEDIUM, settings.c:533): the v1->v2
             * migration that used to live here has been removed rather than
             * repaired, because it could not be made honest.
             *
             * It memcpy'd the stored image over current_settings assuming the
             * two layouts were identical, re-defaulted the tapping sub-struct,
             * then stamped a FRESH VALID checksum and wrote it back — with no
             * CRC check on the way in, so a bit-rotted or partially programmed
             * page had its corrupted voltage_kp/voltage_ki, current_limit,
             * max_limit and depth.offset blessed permanently while
             * "Migration successful" was printed over the top.
             *
             * Adding a CRC gate did not fix it. calc_checksum() spans up to
             * offsetof(settings_t, checksum) in the CURRENT layout and reads
             * the checksum at the current offset; migrate_tapping_settings()'s
             * own comment says "we can't directly read old structure layout".
             * If that is true the gate can never pass and the branch is dead
             * code; if it is false the memcpy was fine and the migration was
             * never needed. Either way one of the two is wrong, and the tree
             * carries no v1 definition to settle it.
             *
             * It is also unreachable in practice: v0.1.0 is the first public
             * release, so no older image exists in any user's hands, and the
             * project has explicitly chosen not to carry back compatibility. A migration nobody can execute and nobody can
             * verify is worse than none — it just launders unauthenticated
             * bytes into a validated struct. Defaults are the safe answer, and
             * they are now what happens, visibly. */
            {
                uart_puts("Settings: Unknown version, using defaults\r\n");
            }
        }
    }

    // Invalid or no settings — build from defaults + EEPROM
    uart_puts("Settings: Initializing with defaults\r\n");
    set_defaults(&current_settings);

    if (use_eeprom) {
        // Import OEM values (speed presets, max RPM, depth cal, profile)
        uart_puts("Settings: Importing OEM EEPROM values\r\n");
        eeprom_load_oem_into(&current_settings);

        // Try custom block at 0xB0 (sensor, PID, tapping, step drill)
        if (eeprom_load_custom(&current_settings)) {
            uart_puts("Settings: Custom EEPROM block loaded\r\n");
        }
    }

    dirty = true;

    // Save defaults to storage
    /* REVIEW FIX (MEDIUM): every return here was discarded and `dirty` was
     * cleared unconditionally with "Defaults saved". On a unit whose erase or
     * program fails, that leaves nothing on the chip and dirty == false — and
     * settings_save()'s `if (!dirty) return SETTINGS_SAVE_OK` then makes every
     * later save a silent no-op while the menu keeps reporting "Settings
     * Saved!". settings_save() itself was fixed to check these; this path was
     * missed. Keep `dirty` set when the write did not land, so the next SAVE
     * retries instead of short-circuiting. */
    uart_puts("Settings: Saving defaults...\r\n");
    bool saved_ok;
    if (use_eeprom) {
        saved_ok  = eeprom_save_to_oem(&current_settings);
        saved_ok  = eeprom_save_custom(&current_settings) && saved_ok;
    } else {
        flash_unlock();
        saved_ok = flash_write_settings(&current_settings);
        flash_lock();
    }
    dirty = !saved_ok;
    uart_puts(saved_ok ? "Settings: Defaults saved\r\n"
                       : "Settings: DEFAULTS NOT SAVED - write failed\r\n");

    // Refresh watchdog after flash write (can take 20-50ms)
    extern void IWDG_refresh(void);  // Defined in main.c
    IWDG->KR = 0xAAAA;
}

const settings_t* settings_get(void) {
    return &current_settings;
}


settings_save_result_t settings_save(void) {
    if (!dirty) {
        return SETTINGS_SAVE_OK;  // Nothing to save
    }

    /* Held across the checksum AND the writes. Deliberately not
     * g_state.flash_in_progress: that flag is consulted by
     * safety_can_start_motor(), and holding it across a whole EEPROM save
     * (~60 byte writes, ~300 ms) would refuse a motor start for the duration —
     * exactly the property the EEPROM path exists to provide, and which the
     * README advertises. flash_in_progress keeps meaning "the CPU is stalled
     * in a flash program cycle". */
    settings_lock();

    /* REVIEW FIX (HIGH): this used to stamp the checksum on current_settings
     * and then spend ~300 ms serialising that same live struct — while the
     * ~40 settings_set_*() functions mutate it with no lock at all. A
     * front-panel menu edit (task_ui) landing inside a console SAVE
     * (task_main) therefore wrote a struct whose contents no longer matched
     * the checksum already computed for it; the next boot rejected the flash
     * mirror and every flash-only field (interface.*, material/bit/diameter,
     * favourites) silently reverted to defaults.
     *
     * Locking all forty setters would have meant forty chances to leak the
     * lock on an early return. Snapshotting instead is one change and closes
     * the window completely: the copy is taken atomically, the checksum
     * describes exactly the bytes that get written, and a setter that lands
     * during the write simply re-marks `dirty` for the next save — which is
     * why `dirty` is cleared BEFORE the snapshot, not after the write.
     *
     * Static, not stack: settings_t is 192 bytes and task_main's stack is
     * 1 KB. */
    taskENTER_CRITICAL();
    s_save_snapshot = current_settings;
    dirty = false;
    taskEXIT_CRITICAL();

    s_save_snapshot.checksum = calc_checksum(&s_save_snapshot);
    current_settings.checksum = s_save_snapshot.checksum;

    bool ok;
    bool mirror_deferred = false;
    if (use_eeprom) {
        // EEPROM: byte-at-a-time writes, no CPU stall, safe while motor running
        /* AUDIT FIX (MEDIUM, settings.c:131): `ok` used to come from the
         * custom block alone, so a failed OEM write still reported success. */
        ok = eeprom_save_to_oem(&s_save_snapshot);
        ok = eeprom_save_custom(&s_save_snapshot) && ok;

        /* AUDIT FIX (HIGH, settings.c:146): the EEPROM block is 61 bytes and
         * settings_t is 192, so the block alone cannot hold everything —
         * interface.*, display brightness/contrast/divisions, and the
         * material/bit-type/diameter used by CalcRPM do not fit. In v0.1.0
         * those simply vanished on every power cycle while SAVE reported
         * success.
         *
         * Flash holds the whole struct, so mirror there as well. It costs a
         * ~20 ms CPU stall, which is why it only happens when the motor is
         * stopped; the EEPROM write above has already secured everything that
         * matters mid-cut, so skipping the mirror while running loses nothing
         * that affects the cut. settings_init() reads flash first and then
         * overlays the EEPROM block, so the fresher of the two always wins for
         * the fields both can hold. */
        /* REVIEW FIX: read the motor state and CLAIM flash_in_progress in one
         * critical region. Split across two, a START pressed in the gap passes
         * safety_can_start_motor() — flash_in_progress still false, motor not
         * yet running — and the ~20 ms page erase then stalls the CPU, EXTI
         * E-Stop ISR included (it executes from flash), while the spindle
         * spins up. Claiming the flag under the same lock as the check means
         * any start still in the motor queue is refused by local_motor_start()'s
         * in-task re-check, which is exactly what that re-check is for.
         *
         * NOTE: this race predates today's work — the original code had the
         * same check-then-set split. Raising the flag for the whole of
         * settings_save() masked it for one revision; putting the flag back
         * where it belongs re-exposed it, so fix it properly. */
        STATE_LOCK();
        bool motor_running_now = g_state.motor_running;
        if (!motor_running_now) {
            g_state.flash_in_progress = true;
        }
        STATE_UNLOCK();

        if (!motor_running_now) {
            /* REVIEW FIX: the result was discarded, so a failed page erase or
             * halfword program left `ok` true, `dirty` cleared and the return
             * value positive — silently losing exactly the flash-only fields
             * the mirror exists to save. `mirror_deferred` covers the SKIPPED
             * case; this covers the ATTEMPTED-AND-FAILED one. */
            flash_unlock();
            ok = flash_write_settings(&s_save_snapshot) && ok;
            flash_lock();

            STATE_LOCK();
            g_state.flash_in_progress = false;
            STATE_UNLOCK();
        } else {
            /* REVIEW FIX: the mirror was skipped, so the flash-only fields are
             * NOT saved. `dirty` must stay set or this "deferred" is a lie —
             * nothing re-marks it when the motor stops, and the next
             * settings_save() returns immediately at `if (!dirty)`. A SAVE
             * made with the spindle running would have permanently lost every
             * flash-only field: interface.*, display, material/bit selection —
             * exactly the loss the mirror was added to repair. */
            mirror_deferred = true;
            uart_puts("Settings: saved to EEPROM; full mirror deferred until motor stops\r\n");
            uart_puts("Settings: SAVE again once stopped to store the rest\r\n");
        }
    } else {
        // Flash: 20ms CPU stall during erase — refuse while motor running
        /* REVIEW FIX: same check-then-set race as the mirror above. */
        STATE_LOCK();
        bool motor_running = g_state.motor_running;
        if (!motor_running) {
            g_state.flash_in_progress = true;
        }
        STATE_UNLOCK();
        if (motor_running) {
            /* Flash-only unit with the spindle turning: nothing at all was
             * written, and RAM already holds the new values. `dirty` was
             * cleared before the snapshot, so put it back. */
            dirty = true;
            settings_unlock();
            return SETTINGS_SAVE_BLOCKED;
        }

        flash_unlock();
        ok = flash_write_settings(&s_save_snapshot);
        flash_lock();

        STATE_LOCK();
        g_state.flash_in_progress = false;
        STATE_UNLOCK();
    }

    /* `dirty` was cleared before the snapshot. Put it back if anything that
     * had to be written did not get written — and note that a setter which ran
     * during the write has already re-marked it, which is exactly what we
     * want: its value is not in the snapshot we just stored. */
    if (!ok || mirror_deferred) {
        dirty = true;
    }

    settings_unlock();

    if (!ok) {
        return SETTINGS_SAVE_ERROR;
    }
    return mirror_deferred ? SETTINGS_SAVE_DEFERRED : SETTINGS_SAVE_OK;
}

/**
 * @brief Persist the live spindle speed once it has settled.
 *
 * Call periodically from task_main. Writes only EE_DEFAULT_RPM (2 bytes), not
 * settings_save() — see include/speed_autosave.h for why.
 *
 * On flash-only units there is no cheap write, so the value is folded into
 * current_settings and marked dirty; it is then captured by the next explicit
 * SAVE rather than erasing a flash page every time the knob stops moving.
 */
static speed_autosave_t s_speed_autosave;

void settings_note_operator_speed(uint16_t rpm, uint32_t now_ms) {
    speed_autosave_note(&s_speed_autosave, rpm, now_ms);
}

void settings_speed_autosave_poll(uint32_t now_ms) {
    /* REVIEW FIX: cheap test first. This runs every task_main iteration, and
     * settings_lock() blocks for as long as a save holds the mutex — up to
     * ~300 ms of bit-banged EEPROM writes, which on EEPROM units happen
     * deliberately WITH THE SPINDLE TURNING. Blocking here would stop
     * task_main draining g_event_queue for that long, and EVT_BTN_GUARD /
     * EVT_BTN_ESTOP are dispatched from that queue: the hardware cutoff in the
     * EXTI handlers still fires, but the braking, spindle hold and state
     * machine that follow it would be deferred. Nothing to commit is the
     * common case by far, so check that without taking anything. */
    if (!speed_autosave_armed(&s_speed_autosave)) {
        return;
    }

    /* The menu owns current_settings while it is open; leave it alone. */
    STATE_LOCK();
    const bool menu_open = g_state.menu_active;
    STATE_UNLOCK();
    if (menu_open) {
        return;
    }

    /* Same mutex settings_save() holds, for two reasons: it keeps this
     * mutation out of the window between that function's checksum and its
     * writes, and it stops two tasks bit-banging the I2C bus at once —
     * eeprom.c has no bus lock and its i2c_delay() is a busy-wait, so a
     * preemption mid-byte leaves SCL/SDA arbitrary and the resumed
     * transaction addresses the wrong byte. */
    /* And even when armed, never wait: a save in progress will still be in
     * progress on the next iteration, and the commit is not urgent. */
    if (!settings_try_lock()) {
        return;
    }

    uint16_t rpm = 0;
    if (speed_autosave_due(&s_speed_autosave,
                           current_settings.speed.default_rpm, now_ms, &rpm) &&
        rpm >= SPEED_MIN_RPM && rpm <= current_settings.speed.max_limit) {

        current_settings.speed.default_rpm = rpm;

        if (use_eeprom) {
            /* Stamping EE_DEFAULT_RPM alone is not enough:
             * eeprom_load_oem_into() bails unless EE_OEM_MAGIC holds 0x7C, and
             * that byte is written only by eeprom_save_to_oem(), which this
             * path deliberately bypasses. On a fresh or replacement AT24C02 —
             * or any unit that has never completed a full SAVE — the speed was
             * written and then ignored on every boot, so the feature did
             * nothing at all and said nothing about it. */
            /* REVIEW FIX: stamp the magic only if the value landed.
             * ee_write16 is two independent 5 ms cycles, so a NACK on the low
             * byte leaves a torn value — declaring the region valid on top of
             * that invites the load side to accept a torn RPM that happens to
             * fall inside its range check. */
            bool ok = ee_write16(EE_DEFAULT_RPM, rpm);
            if (ok) {
                ok = (eeprom_write_byte(EE_OEM_MAGIC, 0x7C) == EEPROM_OK);
            }
            /* Deliberately NOT marking dirty on success: the value is on the
             * chip. On a write failure, fall back to dirty so a later explicit
             * SAVE retries it. */
            if (!ok) {
                dirty = true;
            }
        } else {
            /* Flash-only unit: no cheap write, so let the next SAVE carry it
             * rather than erasing a page every time the knob stops moving. */
            dirty = true;
        }
    }

    settings_unlock();
}

bool settings_using_eeprom(void) {
    return use_eeprom;
}

void settings_reset_defaults(void) {
    set_defaults(&current_settings);
    /* REVIEW FIX: same "most recent explicit action wins" rule as
     * settings_set_speed(). Without this, a knob turn made before entering the
     * menu stays armed while the poll is gated off, and the first poll after
     * Reset defaults writes that stale speed straight back over the reset. */
    speed_autosave_forget(&s_speed_autosave);
    dirty = true;
}

bool settings_is_dirty(void) {
    return dirty;
}

void settings_mark_dirty(void) {
    dirty = true;
}

/*===========================================================================*/
/* Setter Macros (Code Generation)                                          */
/*===========================================================================*/

/**
 * Simple setter - no range validation
 * Usage: SETTINGS_SETTER(func_name, field_access, type)
 * Example: SETTINGS_SETTER(speed_step, speed.step_size, uint8_t)
 * Generates: void settings_set_speed_step(uint8_t value)
 *
 * Note: value is const for scalars (compiler optimizes)
 */
#define SETTINGS_SETTER(func_name, field_access, type) \
void settings_set_##func_name(const type value) { \
    if (current_settings.field_access != value) { \
        current_settings.field_access = value; \
        dirty = true; \
    } \
}

/**
 * Setter with range clamping
 * Usage: SETTINGS_SETTER_RANGE(func_name, field_access, type, min_val, max_val)
 *
 * Note: value is non-const as it may be modified by clamping
 */
#define SETTINGS_SETTER_RANGE(func_name, field_access, type, min_val, max_val) \
void settings_set_##func_name(type value) { \
    if (value < (min_val)) value = (min_val); \
    if (value > (max_val)) value = (max_val); \
    if (current_settings.field_access != value) { \
        current_settings.field_access = value; \
        dirty = true; \
    } \
}

/**
 * Setter with upper-bound clamp only — for unsigned fields whose natural
 * floor is 0 (a "value < 0" lower check is always false and warns under
 * -Wtype-limits).
 */
#define SETTINGS_SETTER_MAX(func_name, field_access, type, max_val) \
void settings_set_##func_name(type value) { \
    if (value > (max_val)) value = (max_val); \
    if (current_settings.field_access != value) { \
        current_settings.field_access = value; \
        dirty = true; \
    } \
}

/*===========================================================================*/
/* Speed Settings                                                            */
/*===========================================================================*/

void settings_set_speed(uint16_t rpm) {
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > current_settings.speed.max_limit) rpm = current_settings.speed.max_limit;

    /* REVIEW FIX: an explicit set supersedes any pending encoder choice. The
     * autosave poll is gated off while the menu is open but stayed ARMED, so
     * a knob turn made before opening the menu would fire on the first poll
     * after it closed and silently overwrite the speed the operator had just
     * set and saved in the menu. */
    speed_autosave_forget(&s_speed_autosave);

    if (current_settings.speed.default_rpm != rpm) {
        current_settings.speed.default_rpm = rpm;
        dirty = true;
    }
}

SETTINGS_SETTER(speed_step, speed.step_size, uint8_t)

void settings_set_favorite_speed(uint8_t slot, uint16_t rpm) {
    if (slot >= NUM_FAVORITE_SPEEDS) return;
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > SPEED_MAX_RPM) rpm = SPEED_MAX_RPM;

    if (current_settings.speed.favorite[slot] != rpm) {
        current_settings.speed.favorite[slot] = rpm;
        dirty = true;
    }
}

void settings_set_max_speed(uint16_t rpm) {
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > SPEED_MAX_RPM) rpm = SPEED_MAX_RPM;

    if (current_settings.speed.max_limit != rpm) {
        current_settings.speed.max_limit = rpm;
        dirty = true;
    }
}

SETTINGS_SETTER(speed_rounding, speed.rounding, bool)

/*===========================================================================*/
/* Tapping Settings                                                          */
/*===========================================================================*/

// Legacy mode setter removed - use trigger enables instead

void settings_set_tap_speed(uint16_t rpm) {
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > SPEED_MAX_RPM) rpm = SPEED_MAX_RPM;

    if (current_settings.tapping.speed_rpm != rpm) {
        current_settings.tapping.speed_rpm = rpm;
        dirty = true;
    }
}

/*===========================================================================*/
/* Depth Settings                                                            */
/*===========================================================================*/

SETTINGS_SETTER(depth_mode, depth.mode, depth_mode_t)

SETTINGS_SETTER(depth_target, depth.target, int16_t)

SETTINGS_SETTER(depth_enabled, depth.enabled, bool)

SETTINGS_SETTER(depth_offset, depth.offset, int16_t)

/*===========================================================================*/
/* Display Settings                                                          */
/*===========================================================================*/



SETTINGS_SETTER(units, display.units, units_mode_t)


/*===========================================================================*/
/* Sensor Settings                                                           */
/*===========================================================================*/

SETTINGS_SETTER(jam_detect, sensor.jam_detect, bool)

SETTINGS_SETTER(spike_detect, sensor.spike_detect, bool)

/*===========================================================================*/
/* Power Settings                                                            */
/*===========================================================================*/

SETTINGS_SETTER(braking, power.braking_enabled, bool)

SETTINGS_SETTER(spindle_hold, power.spindle_hold, bool)

SETTINGS_SETTER_MAX(power_limit, power.power_limit, uint8_t, 100)

SETTINGS_SETTER_MAX(power_output, power.power_output, uint8_t, 3)

/*===========================================================================*/
/* Interface Settings                                                        */
/*===========================================================================*/

SETTINGS_SETTER(key_sound, interface.key_sound, bool)
SETTINGS_SETTER(show_shortcuts, interface.show_shortcuts, bool)

/*===========================================================================*/
/* Motor Parameters (Advanced)                                               */
/*===========================================================================*/

SETTINGS_SETTER(motor_kprop, motor.speed_kprop, int16_t)
SETTINGS_SETTER(motor_kint, motor.speed_kint, int16_t)

SETTINGS_SETTER(voltage_kp, motor.voltage_kp, int16_t)

SETTINGS_SETTER(voltage_ki, motor.voltage_ki, int16_t)

SETTINGS_SETTER_MAX(current_limit, motor.current_limit, uint16_t, 500)

/* AUDIT FIX (MEDIUM, jam.c:412): a MAX-only setter accepted 0 from the
 * console. jam_load_update() uses spike_thresh as a CAP on the trip point, so
 * 0 makes `filtered >= 0` unconditionally true and fires an emergency stop
 * seconds into every cut. A spike threshold below the sustained-jam delta is
 * never a meaningful setting, so clamp at the low end too. */
SETTINGS_SETTER_RANGE(spike_thresh, sensor.spike_thresh, uint16_t, 20, 100)

/*===========================================================================*/
/* Additional Speed Settings                                                 */
/*===========================================================================*/

SETTINGS_SETTER_RANGE(slow_start, speed.slow_start, uint16_t, 100, 1000)

SETTINGS_SETTER_RANGE(anti_tearout, speed.anti_tearout, uint16_t, 100, 500)

SETTINGS_SETTER_MAX(material, speed.material, uint8_t, 11)

SETTINGS_SETTER_MAX(bit_type, speed.bit_type, uint8_t, 8)

SETTINGS_SETTER_RANGE(bit_diameter, speed.bit_diameter, uint8_t, 1, 50)

SETTINGS_SETTER(auto_rpm, speed.auto_rpm, bool)

/*===========================================================================*/
/* Tapping Trigger Settings                                                  */
/*===========================================================================*/

// Trigger enables
SETTINGS_SETTER(depth_trigger_enabled, tapping.depth_trigger_enabled, bool)

SETTINGS_SETTER(load_increase_enabled, tapping.load_increase_enabled, bool)

SETTINGS_SETTER(load_slip_enabled, tapping.load_slip_enabled, bool)

SETTINGS_SETTER(clutch_slip_enabled, tapping.clutch_slip_enabled, bool)

SETTINGS_SETTER(quill_trigger_enabled, tapping.quill_trigger_enabled, bool)

SETTINGS_SETTER(peck_trigger_enabled, tapping.peck_trigger_enabled, bool)

SETTINGS_SETTER(pedal_enabled, tapping.pedal_enabled, bool)

// Per-trigger settings
SETTINGS_SETTER_MAX(depth_completion_action, tapping.depth_completion_action, uint8_t, 2)
/* 3 = COMPLETION_RESUME is valid for these three; depth caps at 2 above. */
SETTINGS_SETTER_MAX(quill_completion_action, tapping.quill_completion_action, uint8_t, 3)
SETTINGS_SETTER_MAX(load_completion_action, tapping.load_completion_action, uint8_t, 3)
SETTINGS_SETTER_MAX(load_slip_completion_action, tapping.load_slip_completion_action, uint8_t, 3)

SETTINGS_SETTER_MAX(quill_pedal_mode, tapping.quill_pedal_mode, uint8_t, QUILL_PEDAL_TOGGLE)

SETTINGS_SETTER_MAX(load_increase_threshold, tapping.load_increase_threshold, uint8_t, 100)

/* Floor added with the reverse-duration work: 0 means "open-ended" to
 * task_tapping, i.e. exactly the runaway 30 s reverse this setting exists to
 * prevent. The menu row has always started at 50. */
SETTINGS_SETTER_RANGE(load_increase_reverse_ms, tapping.load_increase_reverse_ms, uint16_t, 50, 2000)

SETTINGS_SETTER_RANGE(load_slip_cv_percent, tapping.load_slip_cv_percent, uint16_t, 110, 200)

SETTINGS_SETTER_RANGE(clutch_plateau_ms, tapping.clutch_plateau_ms, uint16_t, 50, 500)

SETTINGS_SETTER_MAX(clutch_action, tapping.clutch_action, uint8_t, 1)

/* pedal_action_t has two values, not three. A stored 2 fell through
 * check_pedal_wants_action()'s else branch as CHIP_BREAK but failed the
 * `== PEDAL_ACTION_CHIP_BREAK` test at the dispatch, so it would have behaved
 * as neither mode cleanly. */
SETTINGS_SETTER_MAX(pedal_action, tapping.pedal_action, uint8_t, 1)

SETTINGS_SETTER_RANGE(pedal_chip_break_ms, tapping.pedal_chip_break_ms, uint16_t, 50, 500)

SETTINGS_SETTER_RANGE(peck_fwd_ms, tapping.peck_fwd_ms, uint16_t, TAP_PECK_FWD_MS_MIN, TAP_PECK_FWD_MS_MAX)

SETTINGS_SETTER_RANGE(peck_rev_ms, tapping.peck_rev_ms, uint16_t, TAP_PECK_REV_MS_MIN, TAP_PECK_REV_MS_MAX)

/* 0 is a MEANING, not an out-of-range value: config.h documents peck_cycles as
 * "0 = infinite until depth", the menu row offers 0-99, and the TAPPECK usage
 * text says the same. The minimum of 1 here silently CLAMPED — TAPPECK with 0
 * set the live tapping module to 0, printed "cycles=0", and persisted 1, so
 * the next boot turned an until-depth peck into a single peck. SETTER_MAX
 * rather than SETTER_RANGE with a 0 minimum: the type is unsigned, so a low
 * clamp against 0 is always-false and -Wtype-limits says so. */
SETTINGS_SETTER_MAX(peck_cycles, tapping.peck_cycles, uint8_t, 99)

// AUDIT FIX (MEDIUM, menu.c:545): AtEnd + RevTime setters used to be missing,
// so menu edits and console changes were silently discarded on save.
SETTINGS_SETTER_MAX(peck_completion_action, tapping.peck_completion_action, uint8_t, 2)

SETTINGS_SETTER_RANGE(peck_reverse_out_ms, tapping.peck_reverse_out_ms, uint16_t, 100, 10000)

void settings_set_peck_depth_stop(bool enabled) {
    if (current_settings.tapping.peck_depth_stop != enabled) {
        current_settings.tapping.peck_depth_stop = enabled;
        dirty = true;
    }
}

void settings_set_brake_delay(uint16_t delay_ms) {
    if (delay_ms < 50) delay_ms = 50;      // Min 50ms
    if (delay_ms > 500) delay_ms = 500;    // Max 500ms

    if (current_settings.tapping.brake_delay_ms != delay_ms) {
        current_settings.tapping.brake_delay_ms = delay_ms;
        dirty = true;
    }
}

/*===========================================================================*/
/* Additional Display Settings                                               */
/*===========================================================================*/


/*===========================================================================*/
/* Additional Sensor Settings                                                */
/*===========================================================================*/

void settings_set_vibration_sensitivity(uint8_t level) {
    // 0=OFF, 1=LOW, 2=MED, 3=HIGH
    if (level > 3) level = 3;

    if (current_settings.sensor.vibration_sensitivity != level) {
        current_settings.sensor.vibration_sensitivity = level;
        dirty = true;

        // Apply to MCB immediately
        motor_set_vibration_sensitivity(level);
    }
}

void settings_set_vibration_sensor(bool enabled) {
    // Legacy function - convert bool to sensitivity level
    uint8_t level = enabled ? 3 : 0;  // enabled=HIGH, disabled=OFF
    settings_set_vibration_sensitivity(level);
}

void settings_set_vibration_thresh(uint16_t value) {
    if (value > 2000) value = 2000;

    if (current_settings.sensor.vibration_thresh != value) {
        current_settings.sensor.vibration_thresh = value;
        dirty = true;
    }
}

/*===========================================================================*/
/* Additional Power Settings                                                 */
/*===========================================================================*/

void settings_set_low_voltage_thresh(uint16_t value) {
    if (value < 100) value = 100;
    if (value > 250) value = 250;

    if (current_settings.power.low_voltage_thresh != value) {
        current_settings.power.low_voltage_thresh = value;
        dirty = true;
    }
}

/*===========================================================================*/
/* Additional Interface Settings                                             */
/*===========================================================================*/

void settings_set_menu_locked(bool enabled) {
    if (current_settings.interface.menu_locked != enabled) {
        current_settings.interface.menu_locked = enabled;
        dirty = true;
    }
}

/*===========================================================================*/
/* Additional Motor Parameters                                               */
/*===========================================================================*/

void settings_set_ir_gain(int16_t value) {
    if (current_settings.motor.ir_gain != value) {
        current_settings.motor.ir_gain = value;
        dirty = true;
    }
}

void settings_set_ir_offset(int16_t value) {
    if (current_settings.motor.ir_offset != value) {
        current_settings.motor.ir_offset = value;
        dirty = true;
    }
}

void settings_set_advance_max(int16_t value) {
    if (value < 0) value = 0;
    if (value > 100) value = 100;

    if (current_settings.motor.advance_max != value) {
        current_settings.motor.advance_max = value;
        dirty = true;
    }
}

void settings_set_pulse_max(int16_t value) {
    if (value < 50) value = 50;
    if (value > 200) value = 200;

    if (current_settings.motor.pulse_max != value) {
        current_settings.motor.pulse_max = value;
        dirty = true;
    }
}

void settings_set_motor_profile(uint8_t profile) {
    if (profile > MOTOR_PROFILE_HARD) profile = MOTOR_PROFILE_NORMAL;

    if (current_settings.motor.profile != profile) {
        current_settings.motor.profile = profile;
        dirty = true;
    }
}

void settings_set_speed_ramp(uint16_t value) {
    if (value < 50) value = 50;
    if (value > 2000) value = 2000;

    if (current_settings.motor.speed_ramp != value) {
        current_settings.motor.speed_ramp = value;
        dirty = true;
    }
}

void settings_set_torque_ramp(uint16_t value) {
    if (value < 50) value = 50;
    if (value > 2000) value = 2000;

    if (current_settings.motor.torque_ramp != value) {
        current_settings.motor.torque_ramp = value;
        dirty = true;
    }
}

/*===========================================================================*/
/* Extended Sensor Settings                                                  */
/*===========================================================================*/

void settings_set_stall_sensitivity(uint8_t value) {
    if (value > 100) value = 100;

    if (current_settings.sensor.stall_sensitivity != value) {
        current_settings.sensor.stall_sensitivity = value;
        dirty = true;
    }
}

void settings_set_stall_time(uint16_t ms) {
    if (ms < 100) ms = 100;
    if (ms > 5000) ms = 5000;

    if (current_settings.sensor.stall_time_ms != ms) {
        current_settings.sensor.stall_time_ms = ms;
        dirty = true;
    }
}

void settings_set_guard_check(bool enabled) {
    if (current_settings.sensor.guard_check_enabled != enabled) {
        current_settings.sensor.guard_check_enabled = enabled;
        dirty = true;
    }
}

void settings_set_pedal_enable(bool enabled) {
    if (current_settings.sensor.pedal_enabled != enabled) {
        current_settings.sensor.pedal_enabled = enabled;
        dirty = true;
    }
}

/*===========================================================================*/
/* Extended Power Settings                                                   */
/*===========================================================================*/

void settings_set_dc_bus_voltage(uint16_t voltage) {
    if (voltage < 1000) voltage = 1000;   // Min 100V
    if (voltage > 5000) voltage = 5000;   // Max 500V

    if (current_settings.power.dc_bus_voltage != voltage) {
        current_settings.power.dc_bus_voltage = voltage;
        dirty = true;
    }
}

void settings_set_temp_threshold(uint8_t temp) {
    if (temp < 40) temp = 40;     // Min 40°C
    if (temp > 100) temp = 100;   // Max 100°C

    if (current_settings.power.temp_threshold != temp) {
        current_settings.power.temp_threshold = temp;
        dirty = true;

        // Send threshold to MCB (TH command)
        motor_set_thermal_threshold(temp);
    }
}

void settings_set_overload_threshold(uint8_t value) {
    if (value < 10) value = 10;
    if (value > 100) value = 100;

    if (current_settings.sensor.overload_threshold != value) {
        current_settings.sensor.overload_threshold = value;
        dirty = true;

        motor_send_command(CMD_LD, value);
    }
}

void settings_set_step_thresh(uint8_t value) {
    /* REVIEW FIX (MEDIUM): upper bound only. jam.c's step detector compares a
     * RAW KR delta against this with no debounce, so a threshold of 1 or 2
     * emergency-stops on ordinary sample jitter. The sibling spike_thresh has
     * both a settings floor of 20 and a JAM_SPIKE_MIN_THRESH defence in jam.c;
     * step had neither, and the menu's own step size of 5 already lands inside
     * the danger zone. Floor it at the smallest delta that can mean something. */
    if (value < JAM_STEP_MIN_THRESH) value = JAM_STEP_MIN_THRESH;
    if (value > 100) value = 100;
    if (current_settings.sensor.step_thresh != value) {
        current_settings.sensor.step_thresh = value;
        dirty = true;
    }
}

void settings_set_low_load_detect(bool enabled) {
    if (current_settings.sensor.low_load_detect != enabled) {
        current_settings.sensor.low_load_detect = enabled;
        dirty = true;
    }
}

void settings_set_low_load_thresh(uint8_t value) {
    if (value > 100) value = 100;
    if (current_settings.sensor.low_load_thresh != value) {
        current_settings.sensor.low_load_thresh = value;
        dirty = true;
    }
}

void settings_set_self_start(bool enabled) {
    if (current_settings.power.self_start != enabled) {
        current_settings.power.self_start = enabled;
        dirty = true;
    }
}

void settings_set_pilot_hole(bool enabled) {
    if (current_settings.power.pilot_hole != enabled) {
        current_settings.power.pilot_hole = enabled;
        dirty = true;
    }
}

// Step drill settings
void settings_set_step_drill_enabled(bool enabled) {
    if (current_settings.step_drill.enabled != enabled) {
        current_settings.step_drill.enabled = enabled;
        dirty = true;
    }
}

void settings_set_step_drill_start_dia(uint8_t diameter) {
    if (diameter >= 5 && diameter <= 50 && current_settings.step_drill.start_diameter != diameter) {
        current_settings.step_drill.start_diameter = diameter;
        dirty = true;
    }
}

void settings_set_step_drill_dia_inc(uint8_t increment) {
    if (increment >= 1 && increment <= 10 && current_settings.step_drill.diameter_increment != increment) {
        current_settings.step_drill.diameter_increment = increment;
        dirty = true;
    }
}

void settings_set_step_drill_step_depth(uint8_t depth_x2) {
    if (depth_x2 >= 10 && depth_x2 <= 40 && current_settings.step_drill.step_depth_x2 != depth_x2) {
        current_settings.step_drill.step_depth_x2 = depth_x2;
        dirty = true;
    }
}

void settings_set_step_drill_base_rpm(uint16_t rpm) {
    if (rpm >= SPEED_MIN_RPM && rpm <= SPEED_MAX_RPM && current_settings.step_drill.base_rpm != rpm) {
        current_settings.step_drill.base_rpm = rpm;
        dirty = true;
    }
}

void settings_set_step_drill_target_dia(uint8_t diameter) {
    if (diameter <= 50 && current_settings.step_drill.target_diameter != diameter) {
        current_settings.step_drill.target_diameter = diameter;
        dirty = true;
    }
}

/*===========================================================================*/
/* Tapping runtime sync                                                        */
/*===========================================================================*/

// AUDIT FIX (HIGH, task_tapping.c:379): the tapping state machine reads its
// trigger enables + parameters from tapping.c's static tap_settings, not from
// settings.c's persisted store. The two were only kept in sync by
// menu_apply_settings(), which never runs at boot — so after every power
// cycle every tapping trigger was silently disabled even though the menu and
// DUMP showed them ON. Fix: bulk-push the persisted fields into tapping.c
// after settings_init(), using the same setters the menu path already uses.
extern void tapping_set_load_threshold(uint8_t threshold);   // reuses load_increase
extern void tapping_set_reverse_time(uint16_t time_ms);      // reuses load_increase_reverse_ms
extern void tapping_set_peck_params(uint16_t fwd_ms, uint16_t rev_ms, uint8_t cycles);
extern void tapping_set_peck_depth_stop(bool stop_at_depth);
extern void tapping_set_load_slip_cv_percent(uint16_t percent);
extern void tapping_set_clutch_plateau_ms(uint16_t ms);
extern void tapping_set_clutch_action(uint8_t action);
extern void tapping_set_pedal_action(uint8_t action);
extern void tapping_set_pedal_chip_break_ms(uint16_t ms);
extern void tapping_set_load_increase_reverse_ms(uint16_t time_ms);
extern void tapping_set_load_increase_threshold(uint8_t threshold);
extern void tapping_set_peck_completion_action(uint8_t action);
extern void tapping_set_depth_completion_action(uint8_t action);
extern void tapping_set_quill_completion_action(uint8_t action);
extern void tapping_set_load_completion_action(uint8_t action);
extern void tapping_set_load_slip_completion_action(uint8_t action);
extern void tapping_set_peck_reverse_out_ms(uint16_t ms);

void settings_sync_to_tapping(void) {
    const tap_settings_t* t = &current_settings.tapping;

    tapping_set_speed(t->speed_rpm);

    tapping_set_depth_trigger_enabled(t->depth_trigger_enabled);
    tapping_set_load_increase_enabled(t->load_increase_enabled);
    tapping_set_load_slip_enabled(t->load_slip_enabled);
    tapping_set_clutch_slip_enabled(t->clutch_slip_enabled);
    tapping_set_quill_trigger_enabled(t->quill_trigger_enabled);
    tapping_set_peck_trigger_enabled(t->peck_trigger_enabled);
    tapping_set_pedal_enabled(t->pedal_enabled);

    tapping_set_depth_completion_action(t->depth_completion_action);
    /* These three were absent from this copy, which is why wiring the trigger
     * chain to tap_cfg would otherwise have read stale defaults forever. */
    tapping_set_quill_completion_action(t->quill_completion_action);
    tapping_set_load_completion_action(t->load_completion_action);
    tapping_set_load_slip_completion_action(t->load_slip_completion_action);
    tapping_set_quill_pedal_mode((quill_pedal_mode_t)t->quill_pedal_mode);

    tapping_set_load_increase_threshold(t->load_increase_threshold);
    tapping_set_load_increase_reverse_ms(t->load_increase_reverse_ms);
    tapping_set_load_slip_cv_percent(t->load_slip_cv_percent);
    tapping_set_clutch_plateau_ms(t->clutch_plateau_ms);
    tapping_set_clutch_action(t->clutch_action);

    tapping_set_peck_params(t->peck_fwd_ms, t->peck_rev_ms, t->peck_cycles);
    tapping_set_peck_depth_stop(t->peck_depth_stop);
    tapping_set_peck_completion_action(t->peck_completion_action);
    tapping_set_peck_reverse_out_ms(t->peck_reverse_out_ms);

    tapping_set_pedal_action(t->pedal_action);
    tapping_set_pedal_chip_break_ms(t->pedal_chip_break_ms);
}
