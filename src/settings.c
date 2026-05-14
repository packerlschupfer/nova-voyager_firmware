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
#include "shared.h"
#include "motor.h"
#include "tapping.h"
#include <string.h>

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

static bool flash_write_halfword(uint32_t addr, uint16_t data) {
    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, data) == HAL_OK;
}

static bool flash_write_settings(const settings_t* s) {
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
}

static void flash_read_settings(settings_t* s) {
    memcpy(s, (void*)SETTINGS_FLASH_ADDR, sizeof(settings_t));
}

/*===========================================================================*/
/* EEPROM Operations                                                          */
/*===========================================================================*/

static bool eeprom_write_settings(const settings_t* s) {
    return (eeprom_write(EEPROM_SETTINGS_ADDR, (const uint8_t*)s,
                         sizeof(settings_t)) == EEPROM_OK);
}

static bool eeprom_read_settings(settings_t* s) {
    return (eeprom_read(EEPROM_SETTINGS_ADDR, (uint8_t*)s,
                        sizeof(settings_t)) == EEPROM_OK);
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
    s->tapping.depth_action = TAP_DEPTH_ACTION_STOP;
    s->tapping.depth_completion_action = COMPLETION_REVERSE_OUT;
    s->tapping.quill_pedal_mode = QUILL_PEDAL_OFF;
    s->tapping.quill_completion_action = COMPLETION_REVERSE_OUT;
    s->tapping.load_increase_threshold = TAP_DEFAULT_LOAD_INCREASE_THRESHOLD;
    s->tapping.load_increase_reverse_ms = TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS;
    s->tapping.load_completion_action = COMPLETION_REVERSE_OUT;
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
    s->display.main_div = DIVISION_1_16;
    s->display.sub_div = DIVISION_OFF;
    s->display.main_round = true;
    s->display.sub_round = false;
    s->display.load_display = true;
    s->display.speed_round = true;
    s->display.brightness = 100;
    s->display.contrast = 32;

    // Sensor defaults
    s->sensor.jam_detect = true;
    s->sensor.spike_detect = true;
    s->sensor.vibration_sensitivity = 3;  // 0=OFF, 1=LOW, 2=MED, 3=HIGH (default HIGH)
    s->sensor.vibration_thresh = 800;     // Legacy
    s->sensor.spike_thresh = 90;
    s->sensor.stall_sensitivity = 50;     // 50% default stall sensitivity
    s->sensor.stall_time_ms = 500;        // 500ms stall detection time
    s->sensor.guard_check_enabled = true; // Chuck guard safety (default: ON)
    s->sensor.pedal_enabled = true;       // Foot pedal (default: ON)

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
static void migrate_tapping_settings(settings_t* s) {
    // NOTE: This migration is approximate since we can't directly read
    // old structure layout. We check EEPROM size and known field positions.
    // If migration fails, set_defaults() will be called instead.

    // For now, we'll just initialize to defaults and let user reconfigure
    // A more sophisticated migration would read raw EEPROM bytes at known offsets

    // Disable all triggers first
    s->tapping.depth_trigger_enabled = false;
    s->tapping.load_increase_enabled = false;
    s->tapping.load_slip_enabled = false;
    s->tapping.clutch_slip_enabled = false;
    s->tapping.quill_trigger_enabled = false;
    s->tapping.peck_trigger_enabled = false;
    s->tapping.pedal_enabled = false;

    // Set safe defaults for all trigger settings
    s->tapping.speed_rpm = SPEED_TAP_DEFAULT;
    s->tapping.depth_action = TAP_DEPTH_ACTION_STOP;
    s->tapping.quill_pedal_mode = QUILL_PEDAL_OFF;
    s->tapping.load_increase_threshold = TAP_DEFAULT_LOAD_INCREASE_THRESHOLD;
    s->tapping.load_increase_reverse_ms = TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS;
    s->tapping.load_slip_cv_percent = TAP_DEFAULT_LOAD_SLIP_CV_PERCENT;
    s->tapping.clutch_plateau_ms = TAP_DEFAULT_CLUTCH_PLATEAU_MS;
    s->tapping.clutch_action = CLUTCH_ACTION_REVERSE;
    s->tapping.peck_fwd_ms = TAP_DEFAULT_PECK_FWD_MS;
    s->tapping.peck_rev_ms = TAP_DEFAULT_PECK_REV_MS;
    s->tapping.peck_cycles = TAP_DEFAULT_PECK_CYCLES;
    s->tapping.peck_depth_stop = true;
    s->tapping.pedal_action = PEDAL_ACTION_HOLD;
    s->tapping.pedal_chip_break_ms = TAP_DEFAULT_PEDAL_CHIP_BREAK_MS;
    s->tapping.brake_delay_ms = TAP_DEFAULT_BRAKE_DELAY;

    uart_puts("Settings: Migrated tapping to trigger-based system (all disabled)\r\n");
}

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

void settings_init(void) {
    // Try to detect EEPROM
    use_eeprom = eeprom_init();

    // Try to load settings from storage
    settings_t loaded;
    bool loaded_ok = false;

    if (use_eeprom) {
        loaded_ok = eeprom_read_settings(&loaded);
    } else {
        // Read from flash
        flash_read_settings(&loaded);
        loaded_ok = true;  // Flash read always succeeds (may have garbage)
    }

    // Validate loaded settings
    extern void uart_puts(const char* s);
    if (loaded_ok &&
        loaded.magic == SETTINGS_MAGIC &&
        loaded.version == SETTINGS_VERSION) {
        // Verify checksum
        uint16_t expected_crc = calc_checksum(&loaded);
        if (loaded.checksum == expected_crc) {
            // Valid settings - use them
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
            uart_puts("Settings: Version mismatch - migrating from v");
            print_num(loaded.version);
            uart_puts(" to v");
            print_num(SETTINGS_VERSION);
            uart_puts("\r\n");

            // Attempt migration
            if (loaded.version == 1 && SETTINGS_VERSION == 2) {
                // Copy old settings first
                memcpy(&current_settings, &loaded, sizeof(settings_t));
                // Migrate tapping settings
                migrate_tapping_settings(&current_settings);
                // Update version
                current_settings.version = SETTINGS_VERSION;
                current_settings.checksum = calc_checksum(&current_settings);
                dirty = true;
                uart_puts("Settings: Migration successful\r\n");
            } else {
                uart_puts("Settings: Unknown version, using defaults\r\n");
            }
        }
    }

    // Invalid or no settings - use defaults and save immediately
    uart_puts("Settings: Initializing with defaults\r\n");
    set_defaults(&current_settings);
    dirty = true;

    // Auto-save defaults on first boot (prevents "Bad magic" on every boot)
    uart_puts("Settings: Saving defaults to flash...\r\n");
    if (use_eeprom) {
        eeprom_write_settings(&current_settings);
    } else {
        flash_unlock();
        flash_write_settings(&current_settings);
        flash_lock();
    }
    dirty = false;
    uart_puts("Settings: Defaults saved\r\n");

    // Refresh watchdog after flash write (can take 20-50ms)
    extern void IWDG_refresh(void);  // Defined in main.c
    IWDG->KR = 0xAAAA;
}

const settings_t* settings_get(void) {
    return &current_settings;
}

bool settings_save(void) {
    if (!dirty) {
        return true;  // Nothing to save
    }

    // C3 fix: Refuse to save while motor running (flash write blocks CPU ~20ms)
    STATE_LOCK();
    bool motor_running = g_state.motor_running;
    STATE_UNLOCK();

    if (motor_running) {
        return false;  // Unsafe to write flash during motor operation
    }

    // Update checksum
    current_settings.checksum = calc_checksum(&current_settings);

    bool ok = false;

    if (use_eeprom) {
        // Write to EEPROM
        ok = eeprom_write_settings(&current_settings);
    } else {
        // Write to flash
        flash_unlock();
        ok = flash_write_settings(&current_settings);
        flash_lock();
    }

    if (ok) {
        dirty = false;
    }

    return ok;
}

bool settings_using_eeprom(void) {
    return use_eeprom;
}

void settings_reset_defaults(void) {
    set_defaults(&current_settings);
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

/*===========================================================================*/
/* Speed Settings                                                            */
/*===========================================================================*/

void settings_set_speed(uint16_t rpm) {
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > current_settings.speed.max_limit) rpm = current_settings.speed.max_limit;

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

SETTINGS_SETTER_RANGE(brightness, display.brightness, uint8_t, 0, 63)

SETTINGS_SETTER_RANGE(contrast, display.contrast, uint8_t, 0, 63)

SETTINGS_SETTER(units, display.units, units_mode_t)

SETTINGS_SETTER(load_display, display.load_display, bool)

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

SETTINGS_SETTER_RANGE(power_limit, power.power_limit, uint8_t, 0, 100)

SETTINGS_SETTER_RANGE(power_output, power.power_output, uint8_t, 0, 3)

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

SETTINGS_SETTER_RANGE(current_limit, motor.current_limit, uint16_t, 0, 500)

SETTINGS_SETTER_RANGE(spike_thresh, sensor.spike_thresh, uint16_t, 0, 100)

/*===========================================================================*/
/* Additional Speed Settings                                                 */
/*===========================================================================*/

SETTINGS_SETTER_RANGE(slow_start, speed.slow_start, uint16_t, 100, 1000)

SETTINGS_SETTER_RANGE(anti_tearout, speed.anti_tearout, uint16_t, 100, 500)

SETTINGS_SETTER_RANGE(material, speed.material, uint8_t, 0, 11)

SETTINGS_SETTER_RANGE(bit_type, speed.bit_type, uint8_t, 0, 8)

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
SETTINGS_SETTER_RANGE(depth_action, tapping.depth_action, uint8_t, 0, TAP_DEPTH_ACTION_REVERSE)

SETTINGS_SETTER_RANGE(quill_pedal_mode, tapping.quill_pedal_mode, uint8_t, 0, QUILL_PEDAL_TOGGLE)

SETTINGS_SETTER_RANGE(load_increase_threshold, tapping.load_increase_threshold, uint8_t, 0, 100)

SETTINGS_SETTER_RANGE(load_increase_reverse_ms, tapping.load_increase_reverse_ms, uint16_t, 0, 2000)

SETTINGS_SETTER_RANGE(load_slip_cv_percent, tapping.load_slip_cv_percent, uint16_t, 110, 200)

SETTINGS_SETTER_RANGE(clutch_plateau_ms, tapping.clutch_plateau_ms, uint16_t, 50, 500)

SETTINGS_SETTER_RANGE(clutch_action, tapping.clutch_action, uint8_t, 0, 1)

SETTINGS_SETTER_RANGE(pedal_action, tapping.pedal_action, uint8_t, 0, 2)

SETTINGS_SETTER_RANGE(pedal_chip_break_ms, tapping.pedal_chip_break_ms, uint16_t, 50, 500)

void settings_set_tap_depth_action(uint8_t action) {
    settings_set_depth_action(action);
}

void settings_set_smart_pedal_mode(uint8_t mode) {
    settings_set_quill_pedal_mode(mode);
}

void settings_set_load_threshold(uint8_t threshold) {
    settings_set_load_increase_threshold(threshold);
}

void settings_set_reverse_time(uint16_t time_ms) {
    settings_set_load_increase_reverse_ms(time_ms);
}

void settings_set_through_detect(bool enabled) {
    settings_set_load_slip_enabled(enabled);
}

SETTINGS_SETTER_RANGE(peck_fwd_ms, tapping.peck_fwd_ms, uint16_t, TAP_PECK_FWD_MS_MIN, TAP_PECK_FWD_MS_MAX)

SETTINGS_SETTER_RANGE(peck_rev_ms, tapping.peck_rev_ms, uint16_t, TAP_PECK_REV_MS_MIN, TAP_PECK_REV_MS_MAX)

SETTINGS_SETTER_RANGE(peck_cycles, tapping.peck_cycles, uint8_t, 1, 99)

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

void settings_set_speed_round_display(bool enabled) {
    if (current_settings.display.speed_round != enabled) {
        current_settings.display.speed_round = enabled;
        dirty = true;
    }
}

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
