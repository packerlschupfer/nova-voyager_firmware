/**
 * @file settings.h
 * @brief Persistent Settings Storage
 *
 * Stores user settings in the last page of flash memory.
 * Settings survive power cycles and firmware updates (if flash page preserved).
 *
 * Based on analysis of original Nova Voyager firmware settings.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "stm32f1xx_hal.h"
#include "config.h"
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Constants                                                                 */
/*===========================================================================*/

#define NUM_FAVORITE_SPEEDS     8
#define SETTINGS_VERSION        2

/*===========================================================================*/
/* Enumerations                                                              */
/*===========================================================================*/

typedef enum {
    UNITS_METRIC = 0,
    UNITS_IMPERIAL_DECIMAL,
    UNITS_IMPERIAL_FRACTION,
} units_mode_t;

typedef enum {
    DIVISION_OFF = 0,
    DIVISION_1_8,
    DIVISION_1_16,
    DIVISION_1_32,
    DIVISION_1_64,
} division_t;

typedef enum {
    DEPTH_MODE_OFF = 0,
    DEPTH_MODE_STANDARD,
    DEPTH_MODE_PRECISION,
} depth_mode_t;

typedef enum {
    DEPTH_ACTION_NOTHING = 0,
    DEPTH_ACTION_STOP,
    DEPTH_ACTION_STOP_REV_2S,
    DEPTH_ACTION_STOP_REV_6S,
    DEPTH_ACTION_STOP_REV_TOP,
} depth_action_t;

typedef enum {
    MOTOR_PROFILE_SOFT = 0,     // Gentle acceleration, lower gains
    MOTOR_PROFILE_NORMAL = 1,   // Factory default
    MOTOR_PROFILE_HARD = 2,     // Aggressive, higher gains
} motor_profile_t;

typedef enum {
    SELF_START_OFF = 0,
    SELF_START_ON = 1,
} self_start_t;

typedef enum {
    PILOT_HOLE_OFF = 0,
    PILOT_HOLE_ON = 1,
} pilot_hole_t;

/*===========================================================================*/
/* Settings Structures                                                       */
/*===========================================================================*/

// Motor/PID control parameters
typedef struct {
    int16_t speed_kprop;        // Speed proportional gain (x100)
    int16_t speed_kint;         // Speed integral gain (x100)
    int16_t voltage_kp;         // Voltage Kp (x100)
    int16_t voltage_ki;         // Voltage Ki (x100)
    int16_t ir_gain;            // IR gain (x100)
    int16_t ir_offset;          // IR offset
    int16_t advance_max;        // Speed advance max
    int16_t pulse_max;          // Pulse max
    uint16_t current_limit;     // Current limit (%)
    uint8_t  profile;           // motor_profile_t: Soft/Normal/Hard
    uint16_t speed_ramp;        // Speed ramp rate (RPM/s, 50-2000)
    uint16_t torque_ramp;       // Torque ramp rate (50-2000)
} motor_params_t;

// Speed settings
typedef struct {
    uint16_t default_rpm;       // Default/last used speed
    uint16_t favorite[NUM_FAVORITE_SPEEDS];  // 8 favorite speed presets
    uint16_t max_limit;         // Maximum speed limit
    uint16_t slow_start;        // Slow start speed
    uint16_t anti_tearout;      // Anti-tear out speed
    uint8_t  step_size;         // Fine (10) or coarse (50)
    bool     rounding;          // Speed rounding ON/OFF

    // Material-based RPM calculation
    uint8_t  material;          // material_type_t: Selected material
    uint8_t  bit_type;          // bit_type_t: Bit type (for speed factor)
    uint8_t  bit_diameter;      // Current bit diameter in mm (1-50)
    bool     auto_rpm;          // Auto-calculate RPM from material+diameter+type
} speed_settings_t;

// Tapping settings (EEPROM stored)
typedef struct {
    // Trigger enables (combinable)
    bool     depth_trigger_enabled;     // Enable depth-based trigger
    bool     load_increase_enabled;     // Enable KR spike detection (blind holes)
    bool     load_slip_enabled;         // Enable CV overshoot detection (through holes)
    bool     clutch_slip_enabled;       // Enable load plateau detection (torque limiter)
    bool     quill_trigger_enabled;     // Enable quill direction auto-reverse (was SMART)
    bool     peck_trigger_enabled;      // Enable timed peck cycles
    bool     pedal_enabled;             // Enable pedal override

    // General settings
    uint16_t speed_rpm;                 // Tapping speed

    // Depth trigger settings
    uint8_t  depth_action;              // TAP_DEPTH_ACTION_STOP or REVERSE (legacy)
    uint8_t  depth_completion_action;   // completion_action_t

    // Quill trigger settings (renamed from SMART)
    uint8_t  quill_pedal_mode;          // QUILL_PEDAL_OFF, REVERSE, or TOGGLE
    uint8_t  quill_completion_action;   // completion_action_t

    // Load increase settings (KR spike - blind holes)
    uint8_t  load_increase_threshold;   // % above baseline to trigger (0-100)
    uint16_t load_increase_reverse_ms;  // Duration of reversal (ms)
    uint8_t  load_completion_action;    // completion_action_t

    // Load slip settings (CV overshoot - through holes)
    uint16_t load_slip_cv_percent;      // CV overshoot threshold % (default 130)
    uint8_t  load_slip_completion_action; // completion_action_t

    // Clutch slip settings (load plateau - torque limiter)
    uint16_t clutch_plateau_ms;         // Time at plateau to trigger (ms)
    uint8_t  clutch_action;             // clutch_action_t: action when detected

    // Peck trigger settings (time-based pulses)
    uint16_t peck_fwd_ms;               // Forward pulse duration (ms)
    uint16_t peck_rev_ms;               // Reverse pulse duration (ms)
    uint8_t  peck_cycles;               // Number of cycles (0=infinite until depth)
    bool     peck_depth_stop;           // Stop at target depth (vs complete all cycles)
    uint8_t  peck_completion_action;    // completion_action_t: STOP or REVERSE_OUT
    uint16_t peck_reverse_out_ms;       // Reverse duration if REVERSE_TIMED

    // Pedal settings
    uint8_t  pedal_action;              // pedal_action_t: HOLD or CHIP_BREAK
    uint16_t pedal_chip_break_ms;       // Chip break duration if CHIP_BREAK mode (ms)

    // Common settings
    uint16_t brake_delay_ms;            // Delay between stop and direction change (50-500ms)
} tap_settings_t;

// Depth settings
typedef struct {
    depth_mode_t   mode;        // OFF, Standard, Precision
    depth_action_t action;      // What to do at target depth
    int16_t        target;      // Target depth (0.1mm units)
    int16_t        offset;      // Depth offset/calibration
    bool           enabled;     // Depth stop enabled
} depth_settings_t;

// Step drill settings (auto-speed reduction for step drills)
typedef struct {
    bool     enabled;           // Step drill mode ON/OFF
    uint8_t  start_diameter;    // Starting diameter in mm (5-50)
    uint8_t  diameter_increment; // Diameter increase per step in mm (1-10)
    uint8_t  step_depth_x2;     // Step depth in 0.5mm units (10-40 = 5-20mm)
    uint16_t base_rpm;          // RPM at smallest diameter (250-3000)
    uint8_t  target_diameter;   // Stop when this diameter reached (0=disabled, 5-50mm)
} step_drill_settings_t;

// Display settings
typedef struct {
    units_mode_t units;         // Metric, Imperial decimal/fraction
    division_t   main_div;      // Main division (1/8, 1/16, etc)
    division_t   sub_div;       // Sub division
    bool         main_round;    // Main rounding
    bool         sub_round;     // Sub rounding
    bool         load_display;  // Show load on screen
    bool         speed_round;   // Speed rounding display
    uint8_t      brightness;    // LCD brightness (0-100)
    uint8_t      contrast;      // LCD contrast (0-63)
} display_settings_t;

// Sensor/detection settings
typedef struct {
    bool     jam_detect;        // Jam detection ON/OFF
    bool     spike_detect;      // Load spike detection ON/OFF
    uint8_t  vibration_sensitivity; // 0=OFF, 1=LOW, 2=MED, 3=HIGH
    uint16_t vibration_thresh;  // Vibration threshold (legacy)
    uint16_t spike_thresh;      // Load spike threshold (%)
    uint8_t  stall_sensitivity; // Stall detection sensitivity (0-100)
    uint16_t stall_time_ms;     // Stall detection time threshold (ms)
    bool     guard_check_enabled;  // Chuck guard safety check ON/OFF (default: ON)
    bool     pedal_enabled;     // Foot pedal input enabled (default: ON)
} sensor_settings_t;

// Interface settings
typedef struct {
    bool     key_sound;         // Key press sound
    bool     show_shortcuts;    // Show menu shortcuts
    uint8_t  f1_function;       // F1 shortcut function
    uint8_t  f2_function;       // F2 shortcut function
    uint8_t  f3_function;       // F3 shortcut function
    uint8_t  f4_function;       // F4 shortcut function
    bool     menu_locked;       // Menu lock enabled
    uint16_t password;          // Menu password (0 = disabled)
} interface_settings_t;

// Braking/power settings
typedef struct {
    bool     braking_enabled;   // Power braking ON/OFF
    bool     spindle_hold;      // Powered spindle hold
    uint8_t  power_limit;       // Output power limit (%) - legacy, use power_output instead
    uint8_t  power_output;      // Power output level: 0=Low(20%), 1=Med(50%), 2=High(70%)
    uint16_t low_voltage_thresh; // Low voltage threshold (0.1V units)
    uint16_t dc_bus_voltage;    // DC bus voltage (0.1V units, default 3600 = 360V)
    uint8_t  temp_threshold;    // Temperature shutdown threshold (C, default 60)
    bool     self_start;        // Self-start mode (auto-restart after power loss)
    bool     pilot_hole;        // Pilot hole mode (slow start for centering)
} power_settings_t;

// Main settings structure
typedef struct {
    uint32_t magic;             // Magic number for validation

    uint16_t version;           // Settings version for migration

    motor_params_t      motor;
    speed_settings_t    speed;
    tap_settings_t      tapping;
    depth_settings_t    depth;
    step_drill_settings_t step_drill;
    display_settings_t  display;
    sensor_settings_t   sensor;
    interface_settings_t interface;
    power_settings_t    power;

    // Checksum (CRC16 of all above fields)
    uint16_t checksum;
} settings_t;

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

/**
 * @brief Initialize settings module
 * Loads settings from EEPROM or flash, sets defaults if invalid
 */
void settings_init(void);

/**
 * @brief Get pointer to current settings (read-only)
 */
/** @brief Get read-only pointer to current settings */
const settings_t* settings_get(void);

/**
 * @brief Save current settings to storage (EEPROM or flash)
 */
/** @brief Save settings to EEPROM or flash */
bool settings_save(void);

/**
 * @brief Reset settings to factory defaults
 */
void settings_reset_defaults(void);

/**
 * @brief Check if settings have been modified since last save
 */
bool settings_is_dirty(void);

/**
 * @brief Mark settings as needing save
 */
void settings_mark_dirty(void);

/**
 * @brief Check if EEPROM is being used for storage
 * @return true if using EEPROM, false if using flash
 */
bool settings_using_eeprom(void);

/*===========================================================================*/
/* Convenience Setters (mark dirty automatically)                            */
/*===========================================================================*/

// Speed settings
/** @brief Set default motor speed with clamping to valid range */
void settings_set_speed(uint16_t rpm);
/** @brief Set encoder speed adjustment step size */
void settings_set_speed_step(uint8_t step);
/** @brief Set favorite speed for quick recall (slot 0-7) */
void settings_set_favorite_speed(uint8_t slot, uint16_t rpm);
/** @brief Set maximum speed limit */
void settings_set_max_speed(uint16_t rpm);
/** @brief Enable/disable speed display rounding */
void settings_set_speed_rounding(bool enabled);
/** @brief Set slow start speed for pilot holes */
void settings_set_slow_start(uint16_t rpm);
/** @brief Set anti-tearout speed threshold */
void settings_set_anti_tearout(uint16_t rpm);
/** @brief Set material type for auto-RPM calculation */
void settings_set_material(uint8_t material);
/** @brief Set drill bit type for auto-RPM calculation */
void settings_set_bit_type(uint8_t bit_type);
/** @brief Set drill bit diameter in mm */
void settings_set_bit_diameter(uint8_t diameter);
/** @brief Enable/disable automatic RPM calculation */
void settings_set_auto_rpm(bool enabled);

// Tapping trigger enables
/** @brief Enable/disable depth-based tapping trigger */
void settings_set_depth_trigger_enabled(bool enabled);
/** @brief Enable/disable load increase tapping trigger */
void settings_set_load_increase_enabled(bool enabled);
/** @brief Enable/disable load slip (through-hole) trigger */
void settings_set_load_slip_enabled(bool enabled);
/** @brief Enable/disable clutch slip detection trigger */
void settings_set_clutch_slip_enabled(bool enabled);
/** @brief Enable/disable quill movement tapping trigger */
void settings_set_quill_trigger_enabled(bool enabled);
/** @brief Enable/disable peck drilling trigger */
void settings_set_peck_trigger_enabled(bool enabled);
/** @brief Enable/disable foot pedal trigger */
void settings_set_pedal_enabled(bool enabled);

// Tapping general settings
/** @brief Set tapping mode default speed */
void settings_set_tap_speed(uint16_t rpm);

// Depth trigger settings
/** @brief Set action at target depth (stop or reverse) */
void settings_set_depth_action(uint8_t action);

// Quill trigger settings (renamed from SMART)
/** @brief Set quill mode pedal override behavior */
void settings_set_quill_pedal_mode(uint8_t mode);

// Load increase settings
/** @brief Set load increase detection threshold percentage */
void settings_set_load_increase_threshold(uint8_t threshold);
/** @brief Set load trigger reversal duration */
void settings_set_load_increase_reverse_ms(uint16_t time_ms);

// Load slip settings
/** @brief Set CV overshoot threshold for through-hole detection */
void settings_set_load_slip_cv_percent(uint16_t percent);

// Clutch slip settings
/** @brief Set clutch slip plateau detection time */
void settings_set_clutch_plateau_ms(uint16_t ms);
/** @brief Set clutch slip trigger action */
void settings_set_clutch_action(uint8_t action);

// Peck trigger settings
/** @brief Set peck drilling forward pulse duration */
void settings_set_peck_fwd_ms(uint16_t ms);
/** @brief Set peck drilling reverse pulse duration */
void settings_set_peck_rev_ms(uint16_t ms);
/** @brief Set number of peck cycles (0=infinite) */
void settings_set_peck_cycles(uint8_t cycles);
/** @brief Stop at target depth in peck mode */
void settings_set_peck_depth_stop(bool enabled);

// Pedal settings
/** @brief Set pedal trigger action mode */
void settings_set_pedal_action(uint8_t action);
/** @brief Set pedal chip break hold duration */
void settings_set_pedal_chip_break_ms(uint16_t ms);

// Common settings
void settings_set_brake_delay(uint16_t delay_ms);

// Depth settings
/** @brief Set depth monitoring mode */
void settings_set_depth_mode(depth_mode_t mode);
/** @brief Set target depth in 0.1mm units */
void settings_set_depth_target(int16_t depth);
/** @brief Enable/disable depth monitoring */
void settings_set_depth_enabled(bool enabled);
/** @brief Set depth sensor calibration offset */
void settings_set_depth_offset(int16_t offset);

// Display settings
/** @brief Set LCD backlight brightness (0-63) */
void settings_set_brightness(uint8_t level);
/** @brief Set LCD contrast level (0-63) */
void settings_set_contrast(uint8_t level);
/** @brief Set display units mode (metric/imperial) */
void settings_set_units(units_mode_t units);
/** @brief Enable/disable motor load display */
void settings_set_load_display(bool enabled);
void settings_set_speed_round_display(bool enabled);

// Sensor settings
/** @brief Enable/disable jam detection */
void settings_set_jam_detect(bool enabled);
/** @brief Enable/disable load spike detection */
void settings_set_spike_detect(bool enabled);
/** @brief Set load spike detection threshold */
void settings_set_spike_thresh(uint16_t value);
void settings_set_vibration_sensitivity(uint8_t level);  // 0=OFF, 1=LOW, 2=MED, 3=HIGH
void settings_set_vibration_sensor(bool enabled);
void settings_set_vibration_thresh(uint16_t value);

// Power settings
/** @brief Enable/disable power braking */
void settings_set_braking(bool enabled);
/** @brief Enable/disable powered spindle hold */
void settings_set_spindle_hold(bool enabled);
/** @brief Set motor power limit percentage */
void settings_set_power_limit(uint8_t percent);
/** @brief Set power output level (0=Low, 1=Med, 2=High) */
void settings_set_power_output(uint8_t level);  // 0=Low(20%), 1=Med(50%), 2=High(70%)
void settings_set_low_voltage_thresh(uint16_t value);

// Interface settings
/** @brief Enable/disable key press sound */
void settings_set_key_sound(bool enabled);
/** @brief Enable/disable menu shortcut display */
void settings_set_show_shortcuts(bool enabled);
void settings_set_menu_locked(bool enabled);

// Motor parameters (advanced)
/** @brief Set motor speed PID proportional gain */
void settings_set_motor_kprop(int16_t value);
/** @brief Set motor speed PID integral gain */
void settings_set_motor_kint(int16_t value);
/** @brief Set voltage regulation P gain */
void settings_set_voltage_kp(int16_t value);
/** @brief Set voltage regulation I gain */
void settings_set_voltage_ki(int16_t value);
/** @brief Set maximum current limit */
void settings_set_current_limit(uint16_t value);
void settings_set_ir_gain(int16_t value);
void settings_set_ir_offset(int16_t value);
void settings_set_advance_max(int16_t value);
void settings_set_pulse_max(int16_t value);
void settings_set_motor_profile(uint8_t profile);
void settings_set_speed_ramp(uint16_t value);
void settings_set_torque_ramp(uint16_t value);

// Sensor settings (extended)
void settings_set_stall_sensitivity(uint8_t value);
void settings_set_stall_time(uint16_t ms);
void settings_set_guard_check(bool enabled);
void settings_set_pedal_enable(bool enabled);

// Power settings (extended)
void settings_set_dc_bus_voltage(uint16_t voltage);
void settings_set_temp_threshold(uint8_t temp);
void settings_set_self_start(bool enabled);
void settings_set_pilot_hole(bool enabled);

// Step drill settings
void settings_set_step_drill_enabled(bool enabled);
void settings_set_step_drill_start_dia(uint8_t diameter);
void settings_set_step_drill_dia_inc(uint8_t increment);
void settings_set_step_drill_step_depth(uint8_t depth_x2);
void settings_set_step_drill_base_rpm(uint16_t rpm);
void settings_set_step_drill_target_dia(uint8_t diameter);

#endif /* SETTINGS_H */
