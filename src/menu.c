/**
 * @file menu.c
 * @brief Menu System for 16x4 LCD Configuration UI
 *
 * Hierarchical menu system with submenus for:
 *   - Speed settings
 *   - Tapping modes and parameters
 *   - Depth settings
 *   - Motor configuration
 *   - Sensor settings
 *   - Power settings
 *   - System actions
 */

#include "menu.h"
#include "lcd.h"
#include "shared.h"
#include "settings.h"
#include "materials.h"
#include "config.h"
#include "tapping.h"
#include "motor.h"
#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

// External debug output
extern void uart_puts(const char* s);

/*===========================================================================*/
/* Menu Types                                                                 */
/*===========================================================================*/

typedef enum {
    MENU_SUBMENU,    // Opens submenu
    MENU_INT,        // Integer value
    MENU_ENUM,       // Enum (cycle through options)
    MENU_ACTION,     // Execute action
    MENU_BACK        // Go back
} menu_type_t;

typedef struct {
    const char* label;        // 8-char max for display
    menu_type_t type;
    int16_t* value;           // Pointer to value (for INT/ENUM)
    int16_t min, max, step;   // For INT type
    uint8_t enum_count;       // For ENUM type
    const char** enum_opts;   // ENUM option strings
    uint8_t submenu_id;       // For SUBMENU type / action ID
} menu_item_t;

/*===========================================================================*/
/* Menu State                                                                 */
/*===========================================================================*/

static uint8_t menu_level = 0;          // 0=main, 1=submenu
static uint8_t menu_index = 0;          // Current item in menu
static uint8_t menu_scroll = 0;         // Scroll position
static uint8_t menu_submenu = 0;        // Which submenu we're in
static bool menu_editing = false;       // Currently editing a value
static int16_t menu_edit_value = 0;     // Temporary edit value

// Submenu IDs
#define SUBMENU_MAIN      0
#define SUBMENU_SPEED     1
#define SUBMENU_TAP       2
#define SUBMENU_DEPTH     3
#define SUBMENU_SENSOR    4
#define SUBMENU_SYSTEM    5
#define SUBMENU_MOTOR     6
#define SUBMENU_POWER     7
#define SUBMENU_TAP_DEPTH 8   // Tapping depth mode settings
#define SUBMENU_TAP_QUILL 9   // Tapping smart mode settings
#define SUBMENU_TAP_LOAD  10  // Tapping load mode settings
#define SUBMENU_TAP_PECK  11  // Tapping peck mode settings
#define SUBMENU_DRILL     12  // Step drill mode settings

// Parent submenu for nested navigation
static uint8_t menu_parent = SUBMENU_MAIN;

/*===========================================================================*/
/* Settings Cache                                                             */
/*===========================================================================*/

// Speed settings
static int16_t s_target_rpm;
static int16_t s_max_speed;
static int16_t s_slow_start;
static int16_t s_material;      // Menu reads as 16-bit
static int16_t s_bit_type;      // Menu reads as 16-bit
static int16_t s_bit_dia;       // Menu reads as 16-bit
static int16_t s_auto_rpm;      // Menu reads as 16-bit

// Tapping settings
static int16_t s_tap_speed;
// Trigger enables
static int16_t s_depth_trigger;
static int16_t s_load_inc_trigger;
static int16_t s_load_slip_trigger;
static int16_t s_clutch_trigger;
static int16_t s_quill_trigger;
static int16_t s_peck_trigger;
static int16_t s_pedal_trigger;
// Per-trigger settings
static int16_t s_tap_depth_action;
static int16_t s_depth_completion;
static int16_t s_tap_quill_pedal;
static int16_t s_quill_completion;
static int16_t s_tap_load_thresh;
static int16_t s_tap_reverse_time;
static int16_t s_load_completion;
static int16_t s_tap_through_det;
static int16_t s_load_slip_completion;
static int16_t s_tap_fwd_ms;
static int16_t s_tap_rev_ms;
static int16_t s_tap_peck_cycles;
static int16_t s_peck_completion;
static int16_t s_peck_reverse_out_ms;

// Depth settings
static int16_t s_depth_mode;    // Menu reads as 16-bit
static int16_t s_depth_target;
static int16_t s_units;         // Menu reads as 16-bit

// Step drill settings
static int16_t s_step_enabled;    // Menu reads as 16-bit
static int16_t s_step_start_dia;  // Menu reads as 16-bit
static int16_t s_step_dia_inc;    // Menu reads as 16-bit
static int16_t s_step_depth;      // Menu reads as 16-bit
static int16_t s_step_base_rpm;
static int16_t s_step_target_dia; // Menu reads as 16-bit

// Sensor settings
static int16_t s_jam_detect;      // Menu reads as 16-bit
static int16_t s_vibration_sens;  // 0=OFF, 1=LOW, 2=MED, 3=HIGH
static int16_t s_stall_sens;      // Menu reads as 16-bit
static int16_t s_guard_check;     // Chuck guard safety check ON/OFF
static int16_t s_pedal_enable;    // Foot pedal enabled ON/OFF
static int16_t s_stall_time;

// Motor settings
static int16_t s_motor_profile;   // Menu reads as 16-bit
static int16_t s_speed_ramp;
static int16_t s_torque_ramp;
static int16_t s_current_limit;
static int16_t s_mcb_temp;  // MCB heatsink temperature (read-only)

// Power settings
static int16_t s_power_output;    // Power output level (0=Low 20%, 1=Med 50%, 2=High 70%)
static int16_t s_dc_bus;
static int16_t s_temp_thresh;     // Menu reads as 16-bit
static int16_t s_self_start;      // Menu reads as 16-bit
static int16_t s_pilot_hole;      // Menu reads as 16-bit
static int16_t s_spindle_hold;    // Menu reads as 16-bit

// Interface settings
static int16_t s_key_sound;       // Menu reads as 16-bit

/*===========================================================================*/
/* Enum Option Strings                                                        */
/*===========================================================================*/

static const char* quill_pedal_opts[] = {"Off", "Rev", "Toggle"};  // Renamed from smart_pedal_opts
static const char* completion_opts[] = {"Stop", "RevOut", "RevTime"};  // Universal completion actions
static const char* depth_mode_opts[] = {"Off", "Std", "Prec"};
static const char* onoff_opts[] = {"Off", "On"};
static const char* material_opts[] = {
    "SftWd", "HrdWd", "Plywood", "MDF",
    "Alum", "Brass", "Steel", "StlSS",
    "Acrylc", "ABS", "PVC", "Plastic"
};
static const char* bit_type_opts[] = {
    "Twist", "Brad", "Forstner", "Spade",
    "SpdSpr", "HlSaw", "Glass", "Auger", "Step"
};
static const char* units_opts[] = {"mm", "inch"};
static const char* tap_action_opts[] = {"Stop", "Rev"};
static const char* motor_profile_opts[] = {"Soft", "Norm", "Hard"};
static const char* power_output_opts[] = {"Low", "Med", "High"};
static const char* vibration_opts[] = {"Off", "Low", "Med", "High"};

/*===========================================================================*/
/* Menu Definitions                                                           */
/*===========================================================================*/

static const menu_item_t main_menu[] = {
    {"Speed",   MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_SPEED},
    {"Tapping", MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_TAP},
    {"Depth",   MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_DEPTH},
    {"Drill",   MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_DRILL},
    {"Motor",   MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_MOTOR},
    {"Sensors", MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_SENSOR},
    {"Power",   MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_POWER},
    {"System",  MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_SYSTEM},
    {"< Exit",  MENU_BACK,    NULL, 0, 0, 0, 0, NULL, 0},
};
#define MAIN_MENU_COUNT 9

static menu_item_t speed_menu[] = {
    {"Target",  MENU_INT,  &s_target_rpm, SPEED_MIN_RPM, SPEED_MAX_RPM, 50, 0, NULL, 0},
    {"Max",     MENU_INT,  &s_max_speed,  SPEED_MIN_RPM, SPEED_MAX_RPM, 50, 0, NULL, 0},
    {"Slow St", MENU_INT,  &s_slow_start, 100, 1000, 50, 0, NULL, 0},
    {"Materl",  MENU_ENUM, &s_material, 0, 11, 1, 12, material_opts, 0},
    {"BitTyp",  MENU_ENUM, &s_bit_type, 0, 8, 1, 9, bit_type_opts, 0},
    {"BitDia",  MENU_INT,  &s_bit_dia, 1, 50, 1, 0, NULL, 0},
    {"CalcRPM", MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 4},  // Calculate and apply RPM
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define SPEED_MENU_COUNT 8

// Main tapping menu - trigger enables and settings
static menu_item_t tap_menu[] = {
    {"Speed",   MENU_INT,     &s_tap_speed, SPEED_MIN_RPM, 500, 10, 0, NULL, 0},
    {"[]Depth", MENU_ENUM,    &s_depth_trigger, 0, 1, 1, 2, onoff_opts, 0},
    {"[]LdInc", MENU_ENUM,    &s_load_inc_trigger, 0, 1, 1, 2, onoff_opts, 0},
    {"[]LdSlp", MENU_ENUM,    &s_load_slip_trigger, 0, 1, 1, 2, onoff_opts, 0},
    {"[]Quill", MENU_ENUM,    &s_quill_trigger, 0, 1, 1, 2, onoff_opts, 0},
    {"[]Peck",  MENU_ENUM,    &s_peck_trigger, 0, 1, 1, 2, onoff_opts, 0},
    {"[]Pedal", MENU_ENUM,    &s_pedal_trigger, 0, 1, 1, 2, onoff_opts, 0},
    {"Depth>",  MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_TAP_DEPTH},
    {"Quill>",  MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_TAP_QUILL},
    {"Load >",  MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_TAP_LOAD},
    {"Peck >",  MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_TAP_PECK},
    {"< Back",  MENU_BACK,    NULL, 0, 0, 0, 0, NULL, 0},
};
#define TAP_MENU_COUNT 13

// Quill trigger settings
static menu_item_t tap_quill_menu[] = {
    {"Pedal",   MENU_ENUM, &s_tap_quill_pedal, 0, 2, 1, 3, quill_pedal_opts, 0},
    {"AtEnd",   MENU_ENUM, &s_quill_completion, 0, 2, 1, 3, completion_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define TAP_QUILL_MENU_COUNT 3

// Depth trigger settings
static menu_item_t tap_depth_menu[] = {
    {"AtDep",   MENU_ENUM, &s_tap_depth_action, 0, 1, 1, 2, tap_action_opts, 0},
    {"AtEnd",   MENU_ENUM, &s_depth_completion, 0, 2, 1, 3, completion_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define TAP_DEPTH_MENU_COUNT 3

// Load trigger settings
static menu_item_t tap_load_menu[] = {
    {"Thresh",  MENU_INT,  &s_tap_load_thresh, 10, 100, 5, 0, NULL, 0},
    {"RevTim",  MENU_INT,  &s_tap_reverse_time, 50, 2000, 50, 0, NULL, 0},
    {"AtEnd",   MENU_ENUM, &s_load_completion, 0, 2, 1, 3, completion_opts, 0},
    {"SlipEn",  MENU_ENUM, &s_tap_through_det, 0, 1, 1, 2, onoff_opts, 0},
    {"SlipEnd", MENU_ENUM, &s_load_slip_completion, 0, 2, 1, 3, completion_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define TAP_LOAD_MENU_COUNT 6

// Peck trigger settings
static menu_item_t tap_peck_menu[] = {
    {"FwdMs",   MENU_INT,  &s_tap_fwd_ms, 50, 5000, 50, 0, NULL, 0},
    {"RevMs",   MENU_INT,  &s_tap_rev_ms, 50, 2000, 50, 0, NULL, 0},
    {"Cycles",  MENU_INT,  &s_tap_peck_cycles, 0, 99, 1, 0, NULL, 0},
    {"AtEnd",   MENU_ENUM, &s_peck_completion, 0, 2, 1, 3, completion_opts, 0},
    {"RevTime", MENU_INT,  &s_peck_reverse_out_ms, 200, 5000, 100, 0, NULL, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define TAP_PECK_MENU_COUNT 6

static menu_item_t depth_menu[] = {
    {"Mode",    MENU_ENUM, &s_depth_mode, 0, 2, 1, 3, depth_mode_opts, 0},
    {"Target",  MENU_INT,  &s_depth_target, 0, 1500, 10, 0, NULL, 0},
    {"Units",   MENU_ENUM, &s_units, 0, 1, 1, 2, units_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define DEPTH_MENU_COUNT 4

static menu_item_t drill_menu[] = {
    {"Enable",  MENU_ENUM, &s_step_enabled,  0, 1, 1, 2, onoff_opts, 0},
    {"StrtDia", MENU_INT,  &s_step_start_dia, 5, 50, 1, 0, NULL, 0},
    {"TrgtDia", MENU_INT,  &s_step_target_dia, 0, 50, 1, 0, NULL, 0},
    {"DiaInc",  MENU_INT,  &s_step_dia_inc,  1, 10, 1, 0, NULL, 0},
    {"StpDep",  MENU_INT,  &s_step_depth,    10, 40, 1, 0, NULL, 0},
    {"BaseRPM", MENU_INT,  &s_step_base_rpm, 250, 3000, 50, 0, NULL, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define DRILL_MENU_COUNT 7

static menu_item_t sensor_menu[] = {
    {"JamDet",  MENU_ENUM, &s_jam_detect, 0, 1, 1, 2, onoff_opts, 0},
    {"VibSen",  MENU_ENUM, &s_vibration_sens, 0, 3, 1, 4, vibration_opts, 0},
    {"StlSen",  MENU_INT,  &s_stall_sens, 0, 100, 5, 0, NULL, 0},
    {"StlTim",  MENU_INT,  &s_stall_time, 100, 5000, 100, 0, NULL, 0},
    {"Guard",   MENU_ENUM, &s_guard_check, 0, 1, 1, 2, onoff_opts, 0},
    {"Pedal",   MENU_ENUM, &s_pedal_enable, 0, 1, 1, 2, onoff_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define SENSOR_MENU_COUNT 7

static menu_item_t motor_menu[] = {
    {"Profl",   MENU_ENUM, &s_motor_profile, 0, 2, 1, 3, motor_profile_opts, 0},
    {"SpdRmp",  MENU_INT,  &s_speed_ramp, 50, 2000, 50, 0, NULL, 0},
    {"TrqRmp",  MENU_INT,  &s_torque_ramp, 50, 2000, 50, 0, NULL, 0},
    {"CurLim",  MENU_INT,  &s_current_limit, 10, 100, 5, 0, NULL, 0},
    {"TempC",   MENU_INT,  &s_mcb_temp, 0, 150, 0, 0, NULL, 0},  // Read-only (step=0)
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define MOTOR_MENU_COUNT 6

static menu_item_t power_menu[] = {
    {"Output",  MENU_ENUM, &s_power_output, 0, 2, 1, 3, power_output_opts, 0},
    {"DCBus",   MENU_INT,  &s_dc_bus, 1000, 5000, 100, 0, NULL, 0},
    {"Temp",    MENU_INT,  &s_temp_thresh, 40, 100, 5, 0, NULL, 0},
    {"SlfSrt",  MENU_ENUM, &s_self_start, 0, 1, 1, 2, onoff_opts, 0},
    {"Pilot",   MENU_ENUM, &s_pilot_hole, 0, 1, 1, 2, onoff_opts, 0},
    {"SpdHld",  MENU_ENUM, &s_spindle_hold, 0, 1, 1, 2, onoff_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
#define POWER_MENU_COUNT 7

static menu_item_t system_menu[] = {
    {"Beeps",   MENU_ENUM, &s_key_sound, 0, 1, 1, 2, onoff_opts, 0},
    {"Save",    MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 1},
    {"Reset",   MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 2},
    {"DFU",     MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 3},
    {"< Back",  MENU_BACK,   NULL, 0, 0, 0, 0, NULL, 0},
};
#define SYSTEM_MENU_COUNT 5

/*===========================================================================*/
/* Private Functions                                                          */
/*===========================================================================*/

static const menu_item_t* get_current_menu(uint8_t* count) {
    switch (menu_submenu) {
        case SUBMENU_SPEED:
            *count = SPEED_MENU_COUNT;
            return (const menu_item_t*)speed_menu;
        case SUBMENU_TAP:
            *count = TAP_MENU_COUNT;
            return (const menu_item_t*)tap_menu;
        case SUBMENU_TAP_QUILL:
            *count = TAP_QUILL_MENU_COUNT;
            return (const menu_item_t*)tap_quill_menu;
        case SUBMENU_TAP_DEPTH:
            *count = TAP_DEPTH_MENU_COUNT;
            return (const menu_item_t*)tap_depth_menu;
        case SUBMENU_TAP_LOAD:
            *count = TAP_LOAD_MENU_COUNT;
            return (const menu_item_t*)tap_load_menu;
        case SUBMENU_TAP_PECK:
            *count = TAP_PECK_MENU_COUNT;
            return (const menu_item_t*)tap_peck_menu;
        case SUBMENU_DEPTH:
            *count = DEPTH_MENU_COUNT;
            return (const menu_item_t*)depth_menu;
        case SUBMENU_DRILL:
            *count = DRILL_MENU_COUNT;
            return (const menu_item_t*)drill_menu;
        case SUBMENU_MOTOR:
            *count = MOTOR_MENU_COUNT;
            return (const menu_item_t*)motor_menu;
        case SUBMENU_SENSOR:
            *count = SENSOR_MENU_COUNT;
            return (const menu_item_t*)sensor_menu;
        case SUBMENU_POWER:
            *count = POWER_MENU_COUNT;
            return (const menu_item_t*)power_menu;
        case SUBMENU_SYSTEM:
            *count = SYSTEM_MENU_COUNT;
            return system_menu;
        default:
            *count = MAIN_MENU_COUNT;
            return main_menu;
    }
}

static void menu_load_settings(void) {
    const settings_t* s = settings_get();

    // Speed settings
    s_target_rpm = s->speed.default_rpm;
    s_max_speed = s->speed.max_limit;
    s_slow_start = s->speed.slow_start;
    s_material = s->speed.material;
    s_bit_type = s->speed.bit_type;
    s_bit_dia = s->speed.bit_diameter;

    // Tapping trigger enables
    s_depth_trigger = s->tapping.depth_trigger_enabled ? 1 : 0;
    s_load_inc_trigger = s->tapping.load_increase_enabled ? 1 : 0;
    s_load_slip_trigger = s->tapping.load_slip_enabled ? 1 : 0;
    s_clutch_trigger = s->tapping.clutch_slip_enabled ? 1 : 0;
    s_quill_trigger = s->tapping.quill_trigger_enabled ? 1 : 0;
    s_peck_trigger = s->tapping.peck_trigger_enabled ? 1 : 0;
    s_pedal_trigger = s->tapping.pedal_enabled ? 1 : 0;

    // Tapping settings
    s_tap_speed = s->tapping.speed_rpm;
    s_tap_depth_action = s->tapping.depth_action;
    s_depth_completion = s->tapping.depth_completion_action;
    s_tap_quill_pedal = s->tapping.quill_pedal_mode;  // Fixed variable name
    s_quill_completion = s->tapping.quill_completion_action;
    s_tap_load_thresh = s->tapping.load_increase_threshold;
    s_load_completion = s->tapping.load_completion_action;
    s_tap_reverse_time = s->tapping.load_increase_reverse_ms;
    s_tap_through_det = s->tapping.load_slip_enabled ? 1 : 0;
    s_load_slip_completion = s->tapping.load_slip_completion_action;
    s_tap_fwd_ms = s->tapping.peck_fwd_ms;
    s_tap_rev_ms = s->tapping.peck_rev_ms;
    s_tap_peck_cycles = s->tapping.peck_cycles;
    s_peck_completion = s->tapping.peck_completion_action;    s_peck_reverse_out_ms = s->tapping.peck_reverse_out_ms;

    // Depth settings
    s_depth_mode = s->depth.mode;
    s_depth_target = s->depth.target;
    s_units = s->display.units;

    // Step drill settings
    s_step_enabled = s->step_drill.enabled ? 1 : 0;
    s_step_start_dia = s->step_drill.start_diameter;
    s_step_dia_inc = s->step_drill.diameter_increment;
    s_step_depth = s->step_drill.step_depth_x2;
    s_step_base_rpm = s->step_drill.base_rpm;
    s_step_target_dia = s->step_drill.target_diameter;

    // Sensor settings
    s_jam_detect = s->sensor.jam_detect ? 1 : 0;
    s_vibration_sens = s->sensor.vibration_sensitivity;
    s_stall_sens = s->sensor.stall_sensitivity;
    s_stall_time = s->sensor.stall_time_ms;
    s_guard_check = s->sensor.guard_check_enabled ? 1 : 0;
    s_pedal_enable = s->sensor.pedal_enabled ? 1 : 0;

    // Motor settings
    s_motor_profile = s->motor.profile;
    s_speed_ramp = s->motor.speed_ramp;
    s_torque_ramp = s->motor.torque_ramp;
    s_current_limit = s->motor.current_limit;
    s_mcb_temp = motor_get_temperature();  // Read-only: MCB heatsink temperature

    // Power settings
    s_power_output = s->power.power_output;
    s_dc_bus = s->power.dc_bus_voltage;
    s_temp_thresh = s->power.temp_threshold;
    s_self_start = s->power.self_start ? 1 : 0;
    s_pilot_hole = s->power.pilot_hole ? 1 : 0;
    s_spindle_hold = s->power.spindle_hold ? 1 : 0;

    // Interface settings
    s_key_sound = s->interface.key_sound ? 1 : 0;
}

static void menu_apply_settings(void) {
    // Speed settings
    settings_set_speed(s_target_rpm);
    settings_set_max_speed(s_max_speed);
    settings_set_slow_start(s_slow_start);
    settings_set_material(s_material);
    settings_set_bit_type(s_bit_type);
    settings_set_bit_diameter(s_bit_dia);

    // Tapping trigger enables (both global settings AND tapping module)
    settings_set_depth_trigger_enabled(s_depth_trigger);
    tapping_set_depth_trigger_enabled(s_depth_trigger);
    settings_set_load_increase_enabled(s_load_inc_trigger);
    tapping_set_load_increase_enabled(s_load_inc_trigger);
    settings_set_load_slip_enabled(s_load_slip_trigger);
    tapping_set_load_slip_enabled(s_load_slip_trigger);
    settings_set_clutch_slip_enabled(s_clutch_trigger);
    tapping_set_clutch_slip_enabled(s_clutch_trigger);
    settings_set_quill_trigger_enabled(s_quill_trigger);
    tapping_set_quill_trigger_enabled(s_quill_trigger);
    settings_set_peck_trigger_enabled(s_peck_trigger);
    tapping_set_peck_trigger_enabled(s_peck_trigger);
    settings_set_pedal_enabled(s_pedal_trigger);
    tapping_set_pedal_enabled(s_pedal_trigger);

    // Tapping settings
    settings_set_tap_speed(s_tap_speed);
    settings_set_depth_action(s_tap_depth_action);  // Fixed function name
    settings_set_quill_pedal_mode(s_tap_quill_pedal);  // Fixed function + var name
    settings_set_load_increase_threshold(s_tap_load_thresh);  // Fixed function name
    settings_set_load_increase_reverse_ms(s_tap_reverse_time);  // Fixed function name
    settings_set_load_slip_enabled(s_tap_through_det);  // Maps to load slip
    settings_set_peck_fwd_ms(s_tap_fwd_ms);
    settings_set_peck_rev_ms(s_tap_rev_ms);
    settings_set_peck_cycles(s_tap_peck_cycles);

    // Depth settings
    settings_set_depth_mode(s_depth_mode);
    settings_set_depth_target(s_depth_target);
    settings_set_units(s_units);

    // Step drill settings
    settings_set_step_drill_enabled(s_step_enabled);
    settings_set_step_drill_start_dia(s_step_start_dia);
    settings_set_step_drill_dia_inc(s_step_dia_inc);
    settings_set_step_drill_step_depth(s_step_depth);
    settings_set_step_drill_base_rpm(s_step_base_rpm);
    settings_set_step_drill_target_dia(s_step_target_dia);

    // Sensor settings
    settings_set_jam_detect(s_jam_detect);
    settings_set_vibration_sensitivity(s_vibration_sens);
    settings_set_stall_sensitivity(s_stall_sens);
    settings_set_stall_time(s_stall_time);
    settings_set_guard_check(s_guard_check);
    settings_set_pedal_enable(s_pedal_enable);

    // Motor settings
    settings_set_motor_profile(s_motor_profile);
    settings_set_speed_ramp(s_speed_ramp);
    settings_set_torque_ramp(s_torque_ramp);
    settings_set_current_limit(s_current_limit);

    // Power settings
    settings_set_power_output(s_power_output);
    settings_set_dc_bus_voltage(s_dc_bus);
    settings_set_temp_threshold(s_temp_thresh);
    settings_set_self_start(s_self_start);
    settings_set_pilot_hole(s_pilot_hole);
    settings_set_spindle_hold(s_spindle_hold);

    // Interface settings
    settings_set_key_sound(s_key_sound);

    // Update shared state
    STATE_LOCK();
    g_state.target_rpm = s_target_rpm;
    g_state.target_depth = s_depth_target;
    g_state.depth_mode = s_depth_mode;
    STATE_UNLOCK();

    // Update tapping module - trigger enables (both global settings AND tapping module)
    settings_set_depth_trigger_enabled(s_depth_trigger);
    tapping_set_depth_trigger_enabled(s_depth_trigger);
    settings_set_load_increase_enabled(s_load_inc_trigger);
    tapping_set_load_increase_enabled(s_load_inc_trigger);
    settings_set_load_slip_enabled(s_load_slip_trigger);
    tapping_set_load_slip_enabled(s_load_slip_trigger);
    settings_set_clutch_slip_enabled(s_clutch_trigger);
    tapping_set_clutch_slip_enabled(s_clutch_trigger);
    settings_set_quill_trigger_enabled(s_quill_trigger);
    tapping_set_quill_trigger_enabled(s_quill_trigger);
    settings_set_peck_trigger_enabled(s_peck_trigger);
    tapping_set_peck_trigger_enabled(s_peck_trigger);
    settings_set_pedal_enabled(s_pedal_trigger);
    tapping_set_pedal_enabled(s_pedal_trigger);

    // Update tapping module - settings
    tapping_set_speed(s_tap_speed);
    tapping_set_depth_action(s_tap_depth_action);
    tapping_set_quill_pedal_mode(s_tap_quill_pedal);  // Fixed function name
    settings_set_load_increase_threshold(s_tap_load_thresh);  // Fixed function name
    settings_set_load_increase_reverse_ms(s_tap_reverse_time);  // Fixed function name
    settings_set_load_slip_enabled(s_tap_through_det);
    tapping_set_peck_params(s_tap_fwd_ms, s_tap_rev_ms, s_tap_peck_cycles);
}

/*===========================================================================*/
/* System Actions                                                             */
/*===========================================================================*/

static void action_calc_rpm(void) {
    // Calculate RPM from current material, bit type, and diameter
    uint16_t rpm_min, rpm_max;
    material_calc_rpm_range((material_type_t)s_material,
                           (bit_type_t)s_bit_type,
                           s_bit_dia, &rpm_min, &rpm_max);

    // Use midpoint of range
    s_target_rpm = (rpm_min + rpm_max) / 2;

    // Show calculation on LCD
    lcd_clear();
    lcd_print_at(0, 0, materials_db[s_material].name);
    lcd_print_at(1, 0, bit_types_db[s_bit_type].name);

    // Print bit diameter
    char buf[8];
    buf[0] = ' ';
    buf[1] = (s_bit_dia >= 10) ? ('0' + s_bit_dia / 10) : ' ';
    buf[2] = '0' + (s_bit_dia % 10);
    buf[3] = 'm';
    buf[4] = 'm';
    buf[5] = '\0';
    lcd_print_at(1, 9, buf);

    lcd_print_at(2, 0, "RPM:");
    lcd_print_at(3, 2, "Applied!");

    // Print RPM value
    lcd_set_cursor(2, 5);
    int val = s_target_rpm;
    char rpm_buf[5];
    int pos = 3;
    do {
        rpm_buf[pos--] = '0' + (val % 10);
        val /= 10;
    } while (val > 0 && pos >= 0);
    while (pos >= 0) rpm_buf[pos--] = ' ';
    rpm_buf[4] = '\0';
    lcd_print(rpm_buf);

    delay_ms(1500);  // FreeRTOS-safe delay
}

static void action_save_settings(void) {
    menu_apply_settings();
    settings_save();

    // Sync motor settings to MCB
    lcd_clear();
    lcd_print_at(0, 2, "Syncing");
    lcd_print_at(1, 2, "Motor...");
    motor_sync_and_save();

    lcd_clear();
    lcd_print_at(0, 2, "Settings");
    lcd_print_at(1, 4, "Saved!");
    delay_ms(1000);  // FreeRTOS-safe delay
}

static void action_reset_defaults(void) {
    lcd_clear();
    lcd_print_at(0, 0, "Reset defaults?");
    lcd_print_at(1, 0, "ENC=Yes  F1=No");

    // Wait for confirmation (FreeRTOS-safe)
    for (int i = 0; i < 500; i++) {
        delay_ms(10);
        uint16_t pc = GPIOC->IDR;

        // Encoder button (PC15) = confirm
        if (!(pc & (1 << 15))) {
            uart_puts("Resetting to defaults...\r\n");
            settings_reset_defaults();
            settings_save();
            menu_load_settings();

            // Sync defaults to MCB
            lcd_clear();
            lcd_print_at(0, 2, "Syncing");
            lcd_print_at(1, 2, "Motor...");
            motor_sync_and_save();

            lcd_clear();
            lcd_print_at(0, 2, "Defaults");
            lcd_print_at(1, 3, "Restored!");
            uart_puts("Defaults restored and synced to MCB\r\n");
            delay_ms(1000);
            return;
        }
        // F1 (PC10) = cancel
        if (!(pc & (1 << 10))) {
            return;
        }
    }
}

static void action_enter_dfu(void) {
    lcd_clear();
    lcd_print_at(0, 2, "DFU Mode...");
    lcd_print_at(1, 0, "Connect USB");
    lcd_delay_ms(500);

    // Set DFU flag and reset
    *((volatile uint32_t*)0x20000000) = 0xDEADBEEF;
    NVIC_SystemReset();
}

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

void menu_enter(void) {
    menu_level = 0;
    menu_index = 0;
    menu_scroll = 0;
    menu_submenu = SUBMENU_MAIN;
    menu_editing = false;
    menu_load_settings();
}

void menu_exit(void) {
    menu_apply_settings();
    STATE_LOCK();
    g_state.menu_active = false;
    g_state.state = APP_STATE_IDLE;
    STATE_UNLOCK();
}

void menu_back(void) {
    if (menu_level > 0) {
        // Check if we're in a nested tapping submenu
        if (menu_submenu == SUBMENU_TAP_QUILL ||
            menu_submenu == SUBMENU_TAP_DEPTH ||
            menu_submenu == SUBMENU_TAP_LOAD ||
            menu_submenu == SUBMENU_TAP_PECK) {
            // Go back to tapping menu (stay at level 1)
            menu_submenu = SUBMENU_TAP;
            menu_index = 0;
            menu_scroll = 0;
        } else {
            // Go back to main menu
            menu_level = 0;
            menu_submenu = SUBMENU_MAIN;
            menu_index = 0;
            menu_scroll = 0;
        }
    } else {
        menu_exit();
    }
}

void menu_click(void) {
    uint8_t count;
    const menu_item_t* menu = get_current_menu(&count);
    const menu_item_t* item = &menu[menu_index];

    if (menu_editing) {
        // Confirm edit
        if (item->value) {
            *item->value = menu_edit_value;
        }
        menu_editing = false;
        menu_apply_settings();
        return;
    }

    switch (item->type) {
        case MENU_SUBMENU:
            menu_submenu = item->submenu_id;
            menu_level = 1;
            menu_index = 0;
            menu_scroll = 0;
            break;

        case MENU_INT:
        case MENU_ENUM:
            if (item->value) {
                menu_edit_value = *item->value;
                menu_editing = true;
            }
            break;

        case MENU_ACTION:
            switch (item->submenu_id) {
                case 1: action_save_settings(); break;
                case 2: action_reset_defaults(); break;
                case 3: action_enter_dfu(); break;
                case 4: action_calc_rpm(); break;
            }
            break;

        case MENU_BACK:
            menu_back();
            break;
    }
}

void menu_rotate(int8_t delta) {
    if (menu_editing) {
        // Adjust value
        uint8_t count;
        const menu_item_t* menu = get_current_menu(&count);
        const menu_item_t* item = &menu[menu_index];

        if (item->type == MENU_ENUM) {
            int8_t val = (int8_t)menu_edit_value + delta;
            if (val < 0) val = item->enum_count - 1;
            if (val >= item->enum_count) val = 0;
            menu_edit_value = val;
        } else {
            menu_edit_value += delta * item->step;
            if (menu_edit_value < item->min) menu_edit_value = item->min;
            if (menu_edit_value > item->max) menu_edit_value = item->max;
        }
    } else {
        // Navigate menu
        uint8_t count;
        get_current_menu(&count);

        int8_t new_idx = menu_index + delta;
        if (new_idx < 0) new_idx = 0;
        if (new_idx >= count) new_idx = count - 1;
        menu_index = new_idx;

        // Update scroll
        if (menu_index < menu_scroll) {
            menu_scroll = menu_index;
        } else if (menu_index >= menu_scroll + 4) {
            menu_scroll = menu_index - 3;
        }
    }
}

void menu_draw(void) {
    uint8_t count;
    const menu_item_t* menu = get_current_menu(&count);

    // Don't clear - just overwrite to prevent flicker
    // lcd_clear();

    for (int line = 0; line < 4; line++) {
        uint8_t idx = menu_scroll + line;

        lcd_set_cursor(line, 0);

        if (idx >= count) {
            // Clear unused lines to prevent leftover text
            lcd_print("                ");
            continue;
        }

        const menu_item_t* item = &menu[idx];
        bool selected = (idx == menu_index);

        // Selection indicator
        lcd_data(selected ? '>' : ' ');

        // Label (max 6 chars)
        const char* p = item->label;
        int label_len = 0;
        for (int i = 0; i < 6 && *p; i++) {
            lcd_data(*p++);
            label_len++;
        }
        for (int i = label_len; i < 6; i++) {
            lcd_data(' ');
        }
        lcd_data(' ');

        // Value (8 chars on right side)
        switch (item->type) {
            case MENU_SUBMENU:
                lcd_print("       >");
                break;

            case MENU_INT: {
                int16_t val = (selected && menu_editing) ? menu_edit_value :
                              (item->value ? *item->value : 0);
                char buf[9];
                for (int i = 0; i < 8; i++) buf[i] = ' ';
                buf[8] = '\0';
                int v = val < 0 ? -val : val;
                int pos = selected && menu_editing ? 6 : 7;
                if (selected && menu_editing) buf[7] = ']';
                do {
                    buf[pos--] = '0' + (v % 10);
                    v /= 10;
                } while (v > 0 && pos >= 0);
                if (val < 0 && pos >= 0) buf[pos--] = '-';
                if (selected && menu_editing && pos >= 0) buf[pos] = '[';
                lcd_print(buf);
                break;
            }

            case MENU_ENUM: {
                uint8_t val = (selected && menu_editing) ? menu_edit_value :
                              (item->value ? *((uint8_t*)item->value) : 0);
                char buf[9];
                for (int i = 0; i < 8; i++) buf[i] = ' ';
                buf[8] = '\0';
                if (val < item->enum_count && item->enum_opts) {
                    const char* opt = item->enum_opts[val];
                    int len = 0;
                    while (opt[len] && len < 6) len++;
                    int start = selected && menu_editing ? 7 - len - 1 : 8 - len;
                    if (selected && menu_editing) { buf[start-1] = '['; buf[7] = ']'; }
                    for (int i = 0; i < len; i++) buf[start + i] = opt[i];
                }
                lcd_print(buf);
                break;
            }

            case MENU_ACTION:
            case MENU_BACK:
                lcd_print("        ");
                break;
        }
    }
}
