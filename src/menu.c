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
#include "dfu.h"
#include "utilities.h"   /* ARRAY_COUNT */
#include "menu_format.h"  /* menu_format_enum, MENU_FIELD_WIDTH */
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
#define SUBMENU_ADVANCED  13  // Advanced motor PID settings
#define SUBMENU_TAP_PEDAL 14  // Tapping pedal action settings

/*===========================================================================*/
/* Settings Cache                                                             */
/*===========================================================================*/

// Speed settings
static int16_t s_target_rpm;
/* What speed.default_rpm was when this menu visit started, so the apply path
 * can tell "the operator edited the Target row" from "the operator never
 * touched it" — see menu_apply_settings(). */
static int16_t s_target_rpm_entry;
static int16_t s_max_speed_entry;
static int16_t s_max_speed;
static int16_t s_slow_start;
static int16_t s_material;      // Menu reads as 16-bit
static int16_t s_bit_type;      // Menu reads as 16-bit
static int16_t s_bit_dia;       // Menu reads as 16-bit

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
static int16_t s_tap_depth_completion;
static int16_t s_tap_quill_pedal;
static int16_t s_clutch_action;
static int16_t s_quill_completion;
static int16_t s_load_completion;
static int16_t s_load_slip_completion;
static int16_t s_tap_pedal_action;
static int16_t s_tap_pedal_chip_ms;
static int16_t s_tap_load_thresh;
static int16_t s_tap_reverse_time;
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
static int16_t s_overload_thresh;
static int16_t s_step_thresh;      // Raw-KR step jam threshold (% delta, 0 disables)
static int16_t s_low_load_detect;  // No-load (belt break / tool detach) detector
static int16_t s_low_load_thresh;  // KR floor for low-load detector (%)

// Motor settings
static int16_t s_motor_profile;   // Menu reads as 16-bit
static int16_t s_speed_ramp;
static int16_t s_torque_ramp;
static int16_t s_current_limit;
static int16_t s_mcb_temp;  // MCB heatsink temperature (read-only)

// Advanced motor PID (read from MCB, editable by advanced users)
static int16_t s_speed_kp;
static int16_t s_speed_ki;
static int16_t s_voltage_kp;
static int16_t s_voltage_ki;
static int16_t s_ir_gain;
static int16_t s_ir_offset;

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
/* Same list plus "Resume" (back off, keep cutting). Not offered for depth —
 * see COMPLETION_RESUME in config.h. */
static const char* completion_resume_opts[] = {"Stop", "RevOut", "RevTime", "Resume"};
static const char* depth_mode_opts[] = {"Off", "Std", "Prec"};
static const char* onoff_opts[] = {"Off", "On"};
static const char* clutch_action_opts[] = {"Rev", "Alert"};
static const char* pedal_action_opts[] = {"Hold", "Chip"};
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
static const char* motor_profile_opts[] = {"Soft", "Norm", "Hard"};
static const char* power_output_opts[] = {"Low", "Med", "High"};
static const char* vibration_opts[] = {"Off", "Low", "Med", "High"};

/*===========================================================================*/
/* Menu Definitions                                                           */
/*===========================================================================*/
/* AUDIT FIX (HIGH, menu.c:239): none of these tables carries a hand-written
 * length any more. Row counts come from ARRAY_COUNT via MENU_CASE in
 * get_current_menu() below — add, delete or #ifdef a row and the count follows
 * on its own.
 *
 * v0.1.0 shipped `#define TAP_MENU_COUNT 13` against a 12-entry tap_menu[]: a
 * "[]Clutch" row had been deleted without touching the count. menu_rotate()
 * wraps at count and menu_draw()'s `idx >= count` guard passed index 12, which
 * in .data is tap_peck_menu[0] — one step past "< Back" showed a phantom
 * "FwdMs" row that edited s_tap_fwd_ms through a table the operator never
 * opened. Reordering these tables would have turned the same read into
 * `*item->value = menu_edit_value` through an arbitrary pointer.
 *
 * SYSTEM_MENU_COUNT was worse: three literals selected by #if on build flags,
 * and the fourth combination (BUILD_READONLY without BUILD_GAMES) was already
 * wrong by one.
 */

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
    {"Pedal>",  MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_TAP_PEDAL},
    {"< Back",  MENU_BACK,    NULL, 0, 0, 0, 0, NULL, 0},
};
/* The four "AtEnd"/"SlipEnd" completion-action rows for the quill, depth, load
 * and load-slip triggers are BACK, with task_tapping honouring them. The note
 * below is kept because it records why they were pulled and the bar for
 * returning them — which has now been met.
 *
 * HISTORICAL: the four rows were gone.
 *
 * They were editable MENU_ENUM rows bound to cache variables that
 * menu_apply_settings() never wrote back — so the row redrew with the new value
 * and looked applied, nothing reached current_settings, SAVE stored the old
 * value, and reopening the menu showed the original. But the deeper problem is
 * that tapping.{depth,quill,load,load_slip}_completion_action are read by
 * NOTHING: task_tapping consumes only peck_completion_action (and depth_action,
 * which is the "AtDep" row and does work). They were a second, parallel notion
 * of completion that was never implemented.
 *
 * A menu row the operator can change, that redraws as if it took effect, and
 * that no code consults, is worse than a missing feature. Removed rather than
 * guessed at — if per-trigger completion actions are wanted, task_tapping has
 * to honour them first and the rows come back with the behaviour behind them.
 * The settings fields are left in place (see settings.h) and simply have no
 * reader.
 *
 * That last condition is what changed: the trigger chain in task_tapping.c now
 * selects a completion action per trigger, so each row drives real behaviour.
 * "AtDep" is the same row it always was, but bound to depth_completion_action
 * (three options) rather than the deleted two-value depth_action. */
// Quill trigger settings
static menu_item_t tap_quill_menu[] = {
    {"Pedal",   MENU_ENUM, &s_tap_quill_pedal, 0, 2, 1, 3, quill_pedal_opts, 0},
    {"AtEnd",   MENU_ENUM, &s_quill_completion, 0, 3, 1, 4, completion_resume_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
// Depth trigger settings
static menu_item_t tap_depth_menu[] = {
    {"AtDep",   MENU_ENUM, &s_tap_depth_completion, 0, 2, 1, 3, completion_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
// Load trigger settings
static menu_item_t tap_load_menu[] = {
    {"Thresh",  MENU_INT,  &s_tap_load_thresh, 10, 100, 5, 0, NULL, 0},
    {"RevTim",  MENU_INT,  &s_tap_reverse_time, 50, 2000, 50, 0, NULL, 0},
    /* REVIEW FIX (HIGH): this row had its OWN cache variable while editing the
     * same setting as "[]LdSlp" in the tapping menu. menu_apply_settings()
     * wrote tapping.load_slip_enabled from both, so whichever ran last won and
     * the other row's change was silently discarded — and the tapping MODULE
     * was only ever set from the []LdSlp variable, so the stored setting and
     * the running detector could disagree about whether slip detection was
     * armed. One row, one variable, one truth: both rows now edit
     * s_load_slip_trigger. */
    {"AtEnd",   MENU_ENUM, &s_load_completion, 0, 3, 1, 4, completion_resume_opts, 0},
    {"SlipEn",  MENU_ENUM, &s_load_slip_trigger, 0, 1, 1, 2, onoff_opts, 0},
    {"SlipEnd", MENU_ENUM, &s_load_slip_completion, 0, 3, 1, 4, completion_resume_opts, 0},
    /* Clutch slip lives here because it is a load-plateau detector. ClutEn is
     * new in the sense that matters: s_clutch_trigger already had a cache
     * variable, a menu_load_settings() line and a menu_apply_settings() line —
     * but no row bound to it, so the whole clutch detector could only ever be
     * armed from the console. ClutAc is Rev (reverse out, treat as overload) or
     * Alert (warn and keep cutting). */
    {"ClutEn",  MENU_ENUM, &s_clutch_trigger, 0, 1, 1, 2, onoff_opts, 0},
    {"ClutAc",  MENU_ENUM, &s_clutch_action, 0, 1, 1, 2, clutch_action_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};

/* Pedal action settings. Hold = press reverses and keeps reversing until you
 * release at the top; Chip = press reverses for ChipMs and resumes cutting by
 * itself. Until now pedal_action was stored but never consulted, so both modes
 * behaved as Hold. */
static menu_item_t tap_pedal_menu[] = {
    {"PedAct",  MENU_ENUM, &s_tap_pedal_action, 0, 1, 1, 2, pedal_action_opts, 0},
    {"ChipMs",  MENU_INT,  &s_tap_pedal_chip_ms, 50, 500, 10, 0, NULL, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
// Peck trigger settings
static menu_item_t tap_peck_menu[] = {
    {"FwdMs",   MENU_INT,  &s_tap_fwd_ms, 50, 5000, 50, 0, NULL, 0},
    {"RevMs",   MENU_INT,  &s_tap_rev_ms, 50, 2000, 50, 0, NULL, 0},
    {"Cycles",  MENU_INT,  &s_tap_peck_cycles, 0, 99, 1, 0, NULL, 0},
    {"AtEnd",   MENU_ENUM, &s_peck_completion, 0, 2, 1, 3, completion_opts, 0},
    {"RevTime", MENU_INT,  &s_peck_reverse_out_ms, 200, 5000, 100, 0, NULL, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
static menu_item_t depth_menu[] = {
    {"Mode",    MENU_ENUM, &s_depth_mode, 0, 2, 1, 3, depth_mode_opts, 0},
    {"Target",  MENU_INT,  &s_depth_target, 0, 1500, 10, 0, NULL, 0},
    {"Units",   MENU_ENUM, &s_units, 0, 1, 1, 2, units_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
static menu_item_t drill_menu[] = {
    {"Enable",  MENU_ENUM, &s_step_enabled,  0, 1, 1, 2, onoff_opts, 0},
    {"StrtDia", MENU_INT,  &s_step_start_dia, 5, 50, 1, 0, NULL, 0},
    {"TrgtDia", MENU_INT,  &s_step_target_dia, 0, 50, 1, 0, NULL, 0},
    {"DiaInc",  MENU_INT,  &s_step_dia_inc,  1, 10, 1, 0, NULL, 0},
    {"StpDep",  MENU_INT,  &s_step_depth,    10, 40, 1, 0, NULL, 0},
    {"BaseRPM", MENU_INT,  &s_step_base_rpm, 250, 3000, 50, 0, NULL, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
static menu_item_t sensor_menu[] = {
    {"JamDet",  MENU_ENUM, &s_jam_detect, 0, 1, 1, 2, onoff_opts, 0},
    {"VibSen",  MENU_ENUM, &s_vibration_sens, 0, 3, 1, 4, vibration_opts, 0},
    {"StlSen",  MENU_INT,  &s_stall_sens, 0, 100, 5, 0, NULL, 0},
    {"StlTim",  MENU_INT,  &s_stall_time, 100, 5000, 100, 0, NULL, 0},
    {"Guard",   MENU_ENUM, &s_guard_check, 0, 1, 1, 2, onoff_opts, 0},
    {"Pedal",   MENU_ENUM, &s_pedal_enable, 0, 1, 1, 2, onoff_opts, 0},
    {"OvrLd%",  MENU_INT,  &s_overload_thresh, 10, 100, 5, 0, NULL, 0},
    // Raw-KR step jam (OEM-style). 0 disables. Higher = less sensitive.
    {"StepTh",  MENU_INT,  &s_step_thresh, 0, 100, 5, 0, NULL, 0},
    // Low-load detector (belt break / tool detach). Enable + KR floor.
    {"NoLoad",  MENU_ENUM, &s_low_load_detect, 0, 1, 1, 2, onoff_opts, 0},
    {"NoLdTh",  MENU_INT,  &s_low_load_thresh, 0, 50, 1, 0, NULL, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
static menu_item_t advanced_menu[] = {
    {"SpdKp",   MENU_INT,  &s_speed_kp, 0, 9999, 50, 0, NULL, 0},
    {"SpdKi",   MENU_INT,  &s_speed_ki, 0, 9999, 50, 0, NULL, 0},
    {"VltKp",   MENU_INT,  &s_voltage_kp, 0, 9999, 50, 0, NULL, 0},
    {"VltKi",   MENU_INT,  &s_voltage_ki, 0, 9999, 50, 0, NULL, 0},
    /* REVIEW FIX (HIGH): max was 9999 while the factory default is 28835
     * (MOTOR_FACTORY_IR_GAIN). Entering edit seeded 28835, and the first
     * encoder detent applied the step and then clamped to the row max —
     * collapsing IR compensation to ~1/3 in one nudge, persisting it, and
     * pushing it to the MCB, with the row's own bound making it unrecoverable
     * from the menu. 32000 matches settings_clamp_loaded(); every other PID row
     * has a default well inside 9999, so this row was the outlier. */
    {"IRGain",  MENU_INT,  &s_ir_gain, 0, 32000, 10, 0, NULL, 0},
    {"IROffs",  MENU_INT,  &s_ir_offset, 0, 9999, 10, 0, NULL, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
static menu_item_t motor_menu[] = {
    {"Profl",   MENU_ENUM, &s_motor_profile, 0, 2, 1, 3, motor_profile_opts, 0},
    {"SpdRmp",  MENU_INT,  &s_speed_ramp, 50, 2000, 50, 0, NULL, 0},
    {"TrqRmp",  MENU_INT,  &s_torque_ramp, 50, 2000, 50, 0, NULL, 0},
    {"CurLim",  MENU_INT,  &s_current_limit, 10, 100, 5, 0, NULL, 0},
    {"TempC",   MENU_INT,  &s_mcb_temp, 0, 150, 0, 0, NULL, 0},  // Read-only (step=0)
    {"Advanc",  MENU_SUBMENU, NULL, 0, 0, 0, 0, NULL, SUBMENU_ADVANCED},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
static menu_item_t power_menu[] = {
    {"Output",  MENU_ENUM, &s_power_output, 0, 2, 1, 3, power_output_opts, 0},
    {"DCBus",   MENU_INT,  &s_dc_bus, 1000, 5000, 100, 0, NULL, 0},
    {"Temp",    MENU_INT,  &s_temp_thresh, 40, 100, 5, 0, NULL, 0},
    {"SlfSrt",  MENU_ENUM, &s_self_start, 0, 1, 1, 2, onoff_opts, 0},
    {"Pilot",   MENU_ENUM, &s_pilot_hole, 0, 1, 1, 2, onoff_opts, 0},
    {"SpdHld",  MENU_ENUM, &s_spindle_hold, 0, 1, 1, 2, onoff_opts, 0},
    {"< Back",  MENU_BACK, NULL, 0, 0, 0, 0, NULL, 0},
};
static menu_item_t system_menu[] = {
    {"Beeps",   MENU_ENUM, &s_key_sound, 0, 1, 1, 2, onoff_opts, 0},
    {"Save",    MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 1},
    {"Reset",   MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 2},
    {"DFU",     MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 3},
#ifdef BUILD_GAMES
    {"Pong",    MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 5},
    {"Snake",   MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 6},
    {"Pengin",  MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 7},
    {"BeerQL",  MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 8},
#endif
#ifdef BUILD_READONLY
    {"Showcas", MENU_ACTION, NULL, 0, 0, 0, 0, NULL, 9},
#endif
    {"< Back",  MENU_BACK,   NULL, 0, 0, 0, 0, NULL, 0},
};
/* Was three hand-maintained literals keyed on build flags, and the fourth
 * combination was already wrong: BUILD_READONLY without BUILD_GAMES builds a
 * 6-row table and fell through to the `#else 5`, hiding "< Back". No env sets
 * that pair today (demo sets both) — which is exactly why it would have gone
 * unnoticed. ARRAY_COUNT is correct in all four cases by construction. */

/*===========================================================================*/
/* Private Functions                                                          */
/*===========================================================================*/

/* MENU_CASE binds the table to its own length, so the two cannot be paired
 * wrongly and neither can drift from the array. Before this, each arm named a
 * table and a separate hand-written *_MENU_COUNT macro: two edits required to
 * stay correct, and v0.1.0 shipped with one of them (TAP_MENU_COUNT) stale. */
#define MENU_CASE(submenu, table)                        \
    case submenu:                                        \
        *count = (uint8_t)ARRAY_COUNT(table);            \
        return (const menu_item_t*)(table)

static const menu_item_t* get_current_menu(uint8_t* count) {
    switch (menu_submenu) {
        MENU_CASE(SUBMENU_SPEED,      speed_menu);
        MENU_CASE(SUBMENU_TAP,        tap_menu);
        MENU_CASE(SUBMENU_TAP_QUILL,  tap_quill_menu);
        MENU_CASE(SUBMENU_TAP_DEPTH,  tap_depth_menu);
        MENU_CASE(SUBMENU_TAP_LOAD,   tap_load_menu);
        MENU_CASE(SUBMENU_TAP_PECK,   tap_peck_menu);
        MENU_CASE(SUBMENU_TAP_PEDAL,  tap_pedal_menu);
        MENU_CASE(SUBMENU_DEPTH,      depth_menu);
        MENU_CASE(SUBMENU_DRILL,      drill_menu);
        MENU_CASE(SUBMENU_MOTOR,      motor_menu);
        MENU_CASE(SUBMENU_SENSOR,     sensor_menu);
        MENU_CASE(SUBMENU_POWER,      power_menu);
        MENU_CASE(SUBMENU_ADVANCED,   advanced_menu);
        MENU_CASE(SUBMENU_SYSTEM,     system_menu);
        default:
            *count = (uint8_t)ARRAY_COUNT(main_menu);
            return main_menu;
    }
}

static void menu_load_settings(void) {
    const settings_t* s = settings_get();

    // Speed settings
    s_target_rpm = s->speed.default_rpm;
    s_max_speed  = s->speed.max_limit;
    /* Snapshot AFTER loading, not before — these are what "did this visit
     * change Speed?" is measured against in menu_apply_settings(). Taking them
     * first meant the entry value was the PREVIOUS visit's, which on the first
     * menu open after any reset is zero, so the first visit always looked
     * edited and re-applied the stale Target. Caught on the machine, not by
     * reading the code. */
    s_target_rpm_entry = s_target_rpm;
    s_max_speed_entry  = s_max_speed;
    s_slow_start = s->speed.slow_start;
    s_material = s->speed.material;
    s_bit_type = s->speed.bit_type;
    s_bit_dia = s->speed.bit_diameter;

    // Tapping trigger enables
    s_depth_trigger = s->tapping.depth_trigger_enabled ? 1 : 0;
    s_load_inc_trigger = s->tapping.load_increase_enabled ? 1 : 0;
    s_load_slip_trigger = s->tapping.load_slip_enabled ? 1 : 0;
    s_clutch_trigger = s->tapping.clutch_slip_enabled ? 1 : 0;
    s_clutch_action = (int16_t)s->tapping.clutch_action;
    s_tap_pedal_action = (int16_t)s->tapping.pedal_action;
    s_tap_pedal_chip_ms = (int16_t)s->tapping.pedal_chip_break_ms;
    s_quill_trigger = s->tapping.quill_trigger_enabled ? 1 : 0;
    s_peck_trigger = s->tapping.peck_trigger_enabled ? 1 : 0;
    s_pedal_trigger = s->tapping.pedal_enabled ? 1 : 0;

    // Tapping settings
    s_tap_speed = s->tapping.speed_rpm;
    s_tap_depth_completion = (int16_t)s->tapping.depth_completion_action;
    s_quill_completion = (int16_t)s->tapping.quill_completion_action;
    s_load_completion = (int16_t)s->tapping.load_completion_action;
    s_load_slip_completion = (int16_t)s->tapping.load_slip_completion_action;
    s_tap_quill_pedal = s->tapping.quill_pedal_mode;  // Fixed variable name
    s_tap_load_thresh = s->tapping.load_increase_threshold;
    s_tap_reverse_time = s->tapping.load_increase_reverse_ms;
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
    s_overload_thresh = s->sensor.overload_threshold;
    s_step_thresh = s->sensor.step_thresh;
    s_low_load_detect = s->sensor.low_load_detect ? 1 : 0;
    s_low_load_thresh = s->sensor.low_load_thresh;

    // Motor settings
    s_motor_profile = s->motor.profile;
    s_speed_ramp = s->motor.speed_ramp;
    s_torque_ramp = s->motor.torque_ramp;
    s_current_limit = s->motor.current_limit;
    s_mcb_temp = motor_get_temperature();  // Read-only: MCB heatsink temperature

    // Advanced PID settings
    s_speed_kp = s->motor.speed_kprop;
    s_speed_ki = s->motor.speed_kint;
    s_voltage_kp = s->motor.voltage_kp;
    s_voltage_ki = s->motor.voltage_ki;
    s_ir_gain = s->motor.ir_gain;
    s_ir_offset = s->motor.ir_offset;

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
    /* REVIEW FIX (HIGH): max BEFORE target. settings_set_speed() clamps to
     * speed.max_limit, so applying the target first clamped a newly raised
     * Target against the OLD Max — raising both in one menu visit silently
     * discarded part of the change. */
    settings_set_max_speed(s_max_speed);

    /* REVIEW FIX (HIGH): the Target was pushed — and g_state.target_rpm
     * rewritten from settings — unconditionally, on EVERY confirmed edit and
     * on menu exit. The console is deliberately not blocked while the menu is
     * open, so: open the menu at 1800, send SPEED 2200, then touch any
     * unrelated row (or just close the menu) and the spindle snapped back to
     * 1800 from a row the operator never opened.
     *
     * Verified on the machine: gating only settings_set_speed() was NOT
     * enough — the read-back below assigns g_state.target_rpm from
     * settings.speed.default_rpm, which a console SPEED does not update until
     * its 5 s debounced autosave lands, so the revert came from there instead.
     * Both halves have to be skipped when this visit did not touch Speed. Max
     * counts as touching it: settings_set_speed() clamps to max_limit, so
     * lowering Max must still re-clamp and re-display the Target. */
    const bool speed_touched = (s_target_rpm != s_target_rpm_entry) ||
                               (s_max_speed  != s_max_speed_entry);
    if (speed_touched) {
        settings_set_speed(s_target_rpm);
    }
    settings_set_slow_start(s_slow_start);
    settings_set_material(s_material);
    settings_set_bit_type(s_bit_type);
    settings_set_bit_diameter(s_bit_dia);

    // Tapping trigger enables (pushed to the runtime store by the sync below)
    settings_set_depth_trigger_enabled(s_depth_trigger);
    settings_set_load_increase_enabled(s_load_inc_trigger);
    settings_set_load_slip_enabled(s_load_slip_trigger);
    settings_set_clutch_slip_enabled(s_clutch_trigger);
    settings_set_clutch_action((uint8_t)s_clutch_action);
    settings_set_pedal_action((uint8_t)s_tap_pedal_action);
    settings_set_pedal_chip_break_ms((uint16_t)s_tap_pedal_chip_ms);
    settings_set_quill_trigger_enabled(s_quill_trigger);
    settings_set_peck_trigger_enabled(s_peck_trigger);
    settings_set_pedal_enabled(s_pedal_trigger);

    // Tapping settings
    settings_set_tap_speed(s_tap_speed);
    settings_set_depth_completion_action((uint8_t)s_tap_depth_completion);
    settings_set_quill_completion_action((uint8_t)s_quill_completion);
    settings_set_load_completion_action((uint8_t)s_load_completion);
    settings_set_load_slip_completion_action((uint8_t)s_load_slip_completion);
    settings_set_quill_pedal_mode(s_tap_quill_pedal);  // Fixed function + var name
    settings_set_load_increase_threshold(s_tap_load_thresh);  // Fixed function name
    settings_set_load_increase_reverse_ms(s_tap_reverse_time);  // Fixed function name
    settings_set_peck_fwd_ms(s_tap_fwd_ms);
    settings_set_peck_rev_ms(s_tap_rev_ms);
    settings_set_peck_cycles(s_tap_peck_cycles);
    // AUDIT FIX (MEDIUM, menu.c:545): AtEnd / RevTime menu edits used to be
    // silently discarded — no settings_set_* and no tapping_set_* were wired.
    // Now persisted AND pushed to the runtime tap_settings via the sync path.
    extern void settings_set_peck_completion_action(uint8_t action);
    extern void settings_set_peck_reverse_out_ms(uint16_t ms);
    settings_set_peck_completion_action((uint8_t)s_peck_completion);
    settings_set_peck_reverse_out_ms((uint16_t)s_peck_reverse_out_ms);

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
    settings_set_overload_threshold(s_overload_thresh);
    settings_set_step_thresh((uint8_t)s_step_thresh);
    settings_set_low_load_detect(s_low_load_detect != 0);
    settings_set_low_load_thresh((uint8_t)s_low_load_thresh);

    // Motor settings
    settings_set_motor_profile(s_motor_profile);
    settings_set_speed_ramp(s_speed_ramp);
    settings_set_torque_ramp(s_torque_ramp);
    settings_set_current_limit(s_current_limit);

    // Advanced PID settings
    settings_set_motor_kprop(s_speed_kp);
    settings_set_motor_kint(s_speed_ki);
    settings_set_voltage_kp(s_voltage_kp);
    settings_set_voltage_ki(s_voltage_ki);
    settings_set_ir_gain(s_ir_gain);
    settings_set_ir_offset(s_ir_offset);

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
    /* REVIEW FIX (HIGH): this assigned s_target_rpm RAW, bypassing the
     * max_limit clamp settings_set_speed() applies — the Speed>Target row is
     * bounded by the global SPEED_MAX_RPM (5500), not by the operator's own
     * Max, and action_calc_rpm() can push it to 5500 with no clamp at all. The
     * encoder path honours max_limit (events.c), so the two entry points
     * disagreed. Read back what the settings layer actually accepted rather
     * than re-deriving the clamp here. */
    /* Read back what the settings layer accepted, into BOTH the shared state
     * and the menu's own cache — otherwise lowering Max below Target in one
     * visit leaves the Speed>Target row displaying an RPM the machine will
     * never use, until the menu is closed and reopened. */
    if (speed_touched) {
        s_target_rpm       = (int16_t)settings_get()->speed.default_rpm;
        s_target_rpm_entry = s_target_rpm;
        s_max_speed_entry  = s_max_speed;
        g_state.target_rpm = settings_get()->speed.default_rpm;
    }
    g_state.target_depth = s_depth_target;
    g_state.depth_mode = s_depth_mode;
    STATE_UNLOCK();

    /* The trigger enables and the load-trigger tuning are applied with the
     * rest of the tapping fields above — this used to repeat all of them here,
     * a copy of the same list that had already diverged from it once. */
    /* REVIEW FIX (HIGH): the runtime store task_tapping actually reads
     * (tapping.c's tap_settings, via tapping_get_settings()) used to be
     * updated here by a HAND-MAINTAINED list of tapping_set_* calls beside the
     * settings_set_* ones. The two lists drifted, as duplicated lists do:
     * load_increase_threshold, load_slip_cv_percent, clutch_plateau_ms,
     * clutch_action, peck_completion_action, peck_reverse_out_ms,
     * peck_depth_stop, pedal_action and pedal_chip_break_ms were persisted but
     * never pushed, and settings_sync_to_tapping() — which copies ALL of them —
     * had exactly one caller, main.c at boot. Lower the tapping load threshold
     * from 60% to 20%, save, and the protective reversal still fired at 60%
     * until the next power cycle, with the comment above claiming otherwise.
     *
     * One copy function, called once, after every settings_set_* above. */
    settings_sync_to_tapping();
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
    // AUDIT FIX (LOW, menu.c:657): rows 1 and 3 share DDRAM columns >=8 in
    // this LCD's addressing (row 1 base 0xD0, row 3 base 0xD8 overlap), so
    // lcd_print_at(1, 9, ...) actually renders inside row 3. Anchor the
    // diameter and RPM text within the non-overlapping column range (0-7).
    /* AUDIT FIX (MEDIUM, menu.c:668): the comment directly above is right and
     * this line used to contradict it. lcd_set_cursor() adds the column to a
     * per-row base of {0xC0, 0xD0, 0xC8, 0xD8} (lcd.c:93), so row 1 column 8 is
     * 0xD0 + 8 = 0xD8 — byte 0 of ROW 3. " 12mm" landed at the start of the
     * fourth physical line, where the "Applied!" print two lines below then
     * overwrote it from byte 2, and row 1 showed nothing beside the bit type.
     * Columns 8-15 of rows 0 and 1 are unusable for the same reason; this is
     * the only place that used one. */
    /* Word index, not character index: col 4 is character 8. The bit-type name
     * printed at col 0 above can be 8 characters ("Forstner"), i.e. characters
     * 0-7, so anything below col 4 overwrites its tail. Col 4 puts " 12mm" at
     * characters 8-12, clear of the name and clear of col 8 (which would be
     * row 3). */
    lcd_print_at(1, 4, buf);

    lcd_print_at(2, 0, "RPM:");
    lcd_print_at(3, 2, "Applied!");

    // Print RPM value at column 4 (within row 2's non-overlap range)
    lcd_set_cursor(2, 4);
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

    delay_ms_ui(1500);  // FreeRTOS-safe delay
}

/* REVIEW FIX: the failure text used to assume the only way settings_save()
 * could fail was a deferred flash mirror, so a genuine write error told the
 * operator to "stop, SAVE again" a spindle that was already stopped, and a
 * flash-only unit that had written NOTHING claimed a deferred mirror. Say
 * which of the three actually happened. */
static void menu_show_save_result(settings_save_result_t r,
                                  const char* ok_row0, uint8_t ok_col0,
                                  const char* ok_row1, uint8_t ok_col1,
                                  const char* ok_uart) {
    switch (r) {
        case SETTINGS_SAVE_OK:
            /* REVIEW FIX: the first version of this helper hard-coded
             * "Settings" on row 0 and column 2 on both rows, so a factory
             * reset reported "Settings / Restored!" left-shifted, and its
             * console confirmation was dropped entirely — the success path
             * said nothing on the UART while all three failure paths did. */
            lcd_print_at(0, ok_col0, ok_row0);
            lcd_print_at(1, ok_col1, ok_row1);
            if (ok_uart) {
                uart_puts(ok_uart);
            }
            break;
        case SETTINGS_SAVE_DEFERRED:
            lcd_print_at(0, 0, "Partly saved:");
            lcd_print_at(1, 0, "stop, SAVE again");
            uart_puts("Settings: EEPROM written; flash mirror still deferred\r\n");
            break;
        case SETTINGS_SAVE_BLOCKED:
            lcd_print_at(0, 0, "NOT saved -");
            lcd_print_at(1, 0, "stop spindle 1st");
            uart_puts("Settings: nothing written - flash unit needs a stopped motor\r\n");
            break;
        case SETTINGS_SAVE_ERROR:
        default:
            lcd_print_at(0, 0, "SAVE FAILED!");
            lcd_print_at(1, 0, "Storage error");
            uart_puts("Settings: write error - storage did not accept the data\r\n");
            break;
    }
}

static void action_save_settings(void) {
    menu_apply_settings();
    /* REVIEW FIX: the return value was discarded here while the comment on
     * settings_save() claimed this path reported it. settings_save() returns
     * false when the flash mirror had to be skipped because the spindle is
     * turning — the EEPROM half is written but interface.*, display.units and
     * the material/bit/diameter selection are not. The operator is standing in
     * front of this LCD; a UART line they cannot see is not a report. */
    const settings_save_result_t saved = settings_save();

    // Sync motor settings to MCB.
    // AUDIT FIX (HIGH, menu.c:686): motor_sync_and_save from the UI task
    // collided with task_motor's 2 Hz idle poll on USART3 — GF/KR bytes
    // interleaved with PID/CL parameter writes, splicing MCB command lines
    // and letting motor_save_mcb_params commit corrupted values to the MCB
    // EEPROM. The motor_scan_mode envelope pauses task_motor's polling for
    // the duration; task_motor consults the flag at the top of its loop.
    /* REVIEW FIX: this path was left without the running-motor guard added to
     * action_reset_defaults(), cmd_msync() and cmd_msave() — and it is
     * reachable, because console START is not blocked by menu_active, so the
     * spindle can be started while the menu is open. Raising motor_scan_mode
     * suspends task_motor's whole poll block, and the load and all four jam
     * detectors live inside it. */
    /* REVIEW FIX: claim the flag BEFORE sampling, and treat DRILLING/TAPPING as
     * busy — see scan_claim_or_refuse() in commands_motor.c for both reasons.
     * Sampling first left a console START able to slip in behind the check;
     * sampling motor_running alone missed a depth auto-reverse in flight. */
    /* One atomic claim: decides and claims inside a single STATE_LOCK region,
     * and a failed claim has nothing to release — so this can no longer tear
     * down an envelope the console task is holding. */
    /* REVIEW FIX: `!claimed` conflated "machine is mid-job" with "another task
     * holds the envelope", so a menu Save Settings during a console MSAVE told
     * the operator to stop an already-stopped spindle and quietly dropped the
     * sync. Keep the reason and report it. */
    const motor_scan_result_t scan = motor_scan_try_claim();
    const bool sync_skipped = (scan != MOTOR_SCAN_CLAIMED);

    lcd_clear();
    if (sync_skipped) {
        /* REVIEW FIX: this branch used to print "Saved." and return without
         * ever consulting `saved` — reintroducing, in the save path, exactly
         * the bug fixed sixty lines below in action_reset_defaults(). With the
         * spindle turning settings_save() returns DEFERRED on an EEPROM unit
         * (flash-only fields not written, dirty still set) or BLOCKED on a
         * flash unit (nothing written at all), and the operator would have
         * been told "Saved." either way. Report the save outcome first; the
         * skipped MCB sync is a separate line. */
        /* REVIEW FIX: ok_uart was NULL, so a SUCCESSFUL save printed nothing on
         * the console while all three failure paths did — the very regression
         * menu_show_save_result() was written to prevent. */
        menu_show_save_result(saved, "Settings", 2, "Saved!", 4,
                              "Settings saved to EEPROM\r\n");
        lcd_print_at(2, 0, "No MCB sync -");
        lcd_print_at(3, 0, (scan == MOTOR_SCAN_BUSY) ? "stop, use MSYNC"
                                                     : "MCB busy, retry");
        /* REVIEW FIX: folding this into ok_uart printed it only on a SUCCESSFUL
         * save, so after a BLOCKED one a remote console operator was told
         * nothing was written but never that the MCB sync was skipped too. It
         * is true on every outcome of this branch, so it prints unconditionally. */
        uart_puts("MCB sync skipped: ");
        uart_puts(motor_scan_refusal(scan));
        uart_puts("\r\n");
        delay_ms_ui(2000);
        return;
    }

    lcd_print_at(0, 2, "Syncing");
    lcd_print_at(1, 2, "Motor...");
    delay_ms_ui(MOTOR_SCAN_SETTLE_MS);
    motor_sync_and_save();
    motor_scan_release();
    HEARTBEAT_UPDATE_UI();

    lcd_clear();
    /* REVIEW FIX: this, the COMMON branch, still passed NULL — so an ordinary
     * successful Save Settings on an idle machine printed nothing at all on the
     * console while every failure did. Only the rare motor-running branch had
     * been fixed. */
    menu_show_save_result(saved, "Settings", 2, "Saved!", 4,
                          "Settings saved and synced to MCB\r\n");
    delay_ms_ui(1000);  // FreeRTOS-safe delay
}

static void action_reset_defaults(void) {
    lcd_clear();
    lcd_print_at(0, 0, "Reset defaults?");
    lcd_print_at(1, 0, "ENC=Yes  F1=No");

    /* REVIEW FIX: this is the one long UI block delay_ms_ui() was introduced
     * for, and it was the one left un-pumped — up to 5 s waiting for the
     * operator to answer, with no HEARTBEAT_UPDATE_UI(). From ~2 s in, main.c
     * sees heartbeat_ui stale and prints "[WATCHDOG] Task failure detected /
     * UI stuck" on every loop, flooding the console with a false imminent-reset
     * alarm while the operator is simply reading the prompt. */
    // Wait for confirmation (FreeRTOS-safe)
    for (int i = 0; i < 500; i++) {
        HEARTBEAT_UPDATE_UI();
        delay_ms(10);
        uint16_t pc = GPIOC->IDR;

        // Encoder button (PC15) = confirm
        if (!(pc & (1 << 15))) {
            uart_puts("Resetting to defaults...\r\n");
            /* REVIEW FIX: same discarded return as action_save_settings(). */
            settings_reset_defaults();
            const settings_save_result_t restored = settings_save();
            menu_load_settings();

            /* Sync defaults to MCB.
             *
             * REVIEW FIX: this call had no motor_scan_mode envelope while the
             * identical call in action_save_settings() forty lines above has
             * one, marked AUDIT FIX (HIGH, menu.c:686). Without it the UI
             * task's parameter writes interleave with task_motor's 2 Hz GF/KR
             * status poll on USART3 — the flag is the only thing that pauses
             * that poll — splicing MCB command lines so motor_save_mcb_params
             * can commit a garbled PID or current-limit value to the MCB's own
             * EEPROM. Reset defaults is exactly when you least want that. */
            /* REVIEW FIX: motor_scan_mode gates task_motor's whole poll
             * block, and motor_load_update()/jam_load_update()/jam_update()
             * live inside it — so this envelope suspends all four jam
             * detectors for the ~0.7 s the sync takes. On the console that is
             * guarded by scan_refuse_if_running(); this path is reachable from
             * the front panel with no debug flag at all, so it has to guard
             * itself. Settings are already saved at this point; only the MCB
             * sync is skipped, and MSYNC/MSAVE can do it once stopped. */
            /* Claim first, then sample — see action_save_settings(). */
            const motor_scan_result_t scan = motor_scan_try_claim();
            const bool sync_skipped = (scan != MOTOR_SCAN_CLAIMED);
            bool synced = false;

            if (sync_skipped) {
                /* REVIEW FIX: this said "Defaults saved" unconditionally. On a
                 * flash-backed unit with the spindle turning settings_save()
                 * returns SETTINGS_SAVE_BLOCKED having written nothing, so the
                 * console claimed a save while the LCD two lines later painted
                 * "NOT saved". Only the SYNC is reported here; whether the save
                 * itself worked is menu_show_save_result()'s job. */
                lcd_clear();
                lcd_print_at(0, 0, "No MCB sync -");
                lcd_print_at(1, 0, (scan == MOTOR_SCAN_BUSY) ? "stop, use MSYNC"
                                                             : "MCB busy, retry");
                uart_puts("MCB sync skipped: ");
                uart_puts(motor_scan_refusal(scan));
                uart_puts("\r\n");
                delay_ms_ui(1500);
            } else {
                lcd_clear();
                lcd_print_at(0, 2, "Syncing");
                lcd_print_at(1, 2, "Motor...");
                delay_ms_ui(MOTOR_SCAN_SETTLE_MS);
                motor_sync_and_save();
                motor_scan_release();
                HEARTBEAT_UPDATE_UI();
                synced = true;
            }

            lcd_clear();
            /* REVIEW FIX: the success string used to promise "synced to MCB"
             * regardless of whether the sync branch actually ran. */
            menu_show_save_result(restored, "Defaults", 2, "Restored!", 3,
                                  synced ? "Defaults restored and synced to MCB\r\n"
                                         : "Defaults restored; MCB sync skipped\r\n");
            delay_ms_ui(1000);
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

    /* REVIEW FIX (CRITICAL): this wrote 0xDEADBEEF to 0x20000000 — the pair
     * commands.c documented as wrong and replaced on 2026-08-29. The console
     * DFU command was fixed; this, the front-panel path, was not. The operator
     * saw "DFU Mode... / Connect USB", the board reset, the bootloader found no
     * magic, and the application started again with no USB device — the
     * front-panel update path was simply dead. It also scribbled four bytes
     * over the base of SRAM (live .data) on the way out. One authority now:
     * include/dfu.h. */
    dfu_reboot_into_bootloader();
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
    // Menu writes go through lcd_print_at, which doesn't update the dirty-row
    // shadow. Invalidate so the menu paints from scratch — and again on exit
    // so the main UI repaints fully instead of skipping unchanged-looking cells.
    lcd_shadow_invalidate();
}

void menu_exit(void) {
    menu_apply_settings();
    lcd_shadow_invalidate();
    STATE_LOCK();
    g_state.menu_active = false;
    // AUDIT FIX (MEDIUM, menu.c:764): don't clobber ERROR. If a fault fired
    // while the menu was open (motor fault, overheat), keep the state so the
    // "Press ON to clear" screen shows on menu exit — old code silently reset
    // to IDLE and the next START press spun the motor with the fault never
    // acknowledged.
    if (g_state.state != APP_STATE_ERROR) {
        g_state.state = APP_STATE_IDLE;
    }
    STATE_UNLOCK();
}

void menu_back(void) {
    /* REVIEW FIX (HIGH): menu_back() never cleared menu_editing, and F1 in the
     * menu routes straight here (task_ui.c). Abandoning an edit with F1 left
     * the flag set AND menu_edit_value holding the abandoned number, so the
     * next click on ANY item took menu_click()'s editing branch and wrote that
     * value through — with no re-clamp to the new item's own min/max. Edit
     * Speed>Target to 5000, press F1, click Tapping>Speed (range 50-500), and
     * the tapping speed becomes 5000: tapping_set_speed() only clamps to the
     * global 5500. Leaving an item abandons the edit, by definition. */
    menu_editing = false;

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

/* The previous round routed these through a menu_busy_flag so task_ui would
 * skip repainting while task_main was inside a menu action — the modal screens
 * (Save, Reset, the DFU/reset confirmations) were being overpainted 30x/s and
 * the operator never saw them. That flag is gone: every menu action now runs on
 * task_ui itself (see ui_menu_click() in task_ui.c), so the action and the
 * repaint are the same task and cannot interleave by construction. */
static void menu_click_impl(void);
static void menu_rotate_impl(int8_t delta);

void menu_click(void) {
    menu_click_impl();
}

void menu_rotate(int8_t delta) {
    menu_rotate_impl(delta);
}

/* menu_draw() has always guarded `idx >= count`; the click and rotate paths
 * never did. REVIEW FIX (CRITICAL): with an index past the end, the editing
 * branch below executes `*item->value = menu_edit_value` — a write through a
 * pointer read from past the end of the item table. Menu input is now owned by
 * a single task (see ui_menu_click() in task_ui.c), which removes the race that
 * produced the stale index; this closes the hole the race was driving through,
 * and any future one. An empty menu is not indexable at all. */
static bool menu_item_valid(uint8_t count) {
    if (count == 0) {
        return false;
    }
    if (menu_index >= count) {
        menu_index = (uint8_t)(count - 1);
        menu_scroll = 0;
    }
    return true;
}

static void menu_click_impl(void) {
    uint8_t count;
    const menu_item_t* menu = get_current_menu(&count);
    if (!menu_item_valid(count)) {
        return;
    }
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
#ifdef BUILD_GAMES
                case 5: { extern void pong_run(void); extern void game_launch(void(*)(void)); game_launch(pong_run); break; }
                case 6: { extern void snake_run(void); extern void game_launch(void(*)(void)); game_launch(snake_run); break; }
                case 7: { extern void penguin_run(void); extern void game_launch(void(*)(void)); game_launch(penguin_run); break; }
                case 8: { extern void beerquill_run(void); extern void game_launch(void(*)(void)); game_launch(beerquill_run); break; }
#endif
#ifdef BUILD_READONLY
                case 9: { extern void showcase_run(void); extern void game_launch(void(*)(void)); game_launch(showcase_run); break; }
#endif
            }
            break;

        case MENU_BACK:
            menu_back();
            break;
    }
}

static void menu_rotate_impl(int8_t delta) {
    if (menu_editing) {
        // Adjust value
        uint8_t count;
        const menu_item_t* menu = get_current_menu(&count);
        if (!menu_item_valid(count)) {
            return;
        }
        const menu_item_t* item = &menu[menu_index];

        if (item->type == MENU_ENUM) {
            /* REVIEW FIX: the old single-step wrap (val < 0 -> enum_count - 1)
             * assumed |delta| == 1, but task_ui hands menu_rotate() the whole
             * accumulated detent count, so a quick two-click spin off either
             * end landed one option short of where the operator aimed. */
            int16_t val = (int16_t)menu_edit_value + delta;
            const int16_t n = (int16_t)item->enum_count;
            if (n > 0) {
                val %= n;
                if (val < 0) val += n;
            } else {
                val = 0;
            }
            menu_edit_value = (int16_t)val;
        } else {
            menu_edit_value += delta * item->step;
            if (menu_edit_value < item->min) menu_edit_value = item->min;
            if (menu_edit_value > item->max) menu_edit_value = item->max;
        }
    } else {
        // Navigate menu
        uint8_t count;
        get_current_menu(&count);

        /* REVIEW FIX: same single-step assumption — two clicks CCW from the
         * top row used to select the last item instead of the second-to-last,
         * and int8_t overflowed on a big delta. Wrap properly. */
        int16_t new_idx = (int16_t)menu_index + delta;
        if (count > 0) {
            new_idx %= (int16_t)count;
            if (new_idx < 0) new_idx += (int16_t)count;
        } else {
            new_idx = 0;
        }
        menu_index = (uint8_t)new_idx;

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
                /* AUDIT FIX (HIGH, menu.c:982): the bracket arithmetic used to
                 * live here and wrote buf[-1] for any 6-character option. It
                 * now lives in menu_format_enum() (include/menu_format.h),
                 * which is unit tested in test/test_menu_format — including
                 * the exact 6-character case that overflowed. */
                const bool editing = selected && menu_editing;
                const char* opt = (val < item->enum_count && item->enum_opts)
                                      ? item->enum_opts[val] : NULL;
                char buf[MENU_FIELD_WIDTH + 1];
                menu_format_enum(buf, opt, editing);
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
