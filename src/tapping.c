/**
 * @file tapping.c
 * @brief Tapping Mode Settings and Utilities
 *
 * NOTE: The actual tapping state machine is in task_tapping.c (FreeRTOS task).
 * This file provides settings management and utility functions only.
 */

#include "tapping.h"
#include "config.h"
#include <string.h>

/*===========================================================================*/
/* Private Variables                                                          */
/*===========================================================================*/

static tapping_state_t tap_state = {
    .state = TAP_STATE_IDLE,
    .speed_rpm = SPEED_TAP_DEFAULT
};

// Tapping trigger settings (runtime copy)
static tapping_settings_t tap_settings = {
    .depth_trigger_enabled = 0,
    .load_increase_enabled = 0,
    .load_slip_enabled = 0,
    .clutch_slip_enabled = 0,
    .quill_trigger_enabled = 0,
    .peck_trigger_enabled = 0,
    .pedal_enabled = 0,
    .speed_rpm = 200,
    .depth_action = TAP_DEPTH_ACTION_STOP,
    .quill_pedal_mode = QUILL_PEDAL_OFF,
    .load_increase_threshold = TAP_DEFAULT_LOAD_INCREASE_THRESHOLD,
    .load_increase_reverse_ms = TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS,
    .load_slip_cv_percent = TAP_DEFAULT_LOAD_SLIP_CV_PERCENT,
    .clutch_plateau_ms = TAP_DEFAULT_CLUTCH_PLATEAU_MS,
    .clutch_action = CLUTCH_ACTION_REVERSE,
    .peck_fwd_ms = TAP_DEFAULT_PECK_FWD_MS,
    .peck_rev_ms = TAP_DEFAULT_PECK_REV_MS,
    .peck_cycles = TAP_DEFAULT_PECK_CYCLES,
    .peck_depth_stop = 1,
    .pedal_action = PEDAL_ACTION_HOLD,
    .pedal_chip_break_ms = TAP_DEFAULT_PEDAL_CHIP_BREAK_MS
};

/*===========================================================================*/
/* Speed Functions                                                            */
/*===========================================================================*/

void tapping_set_speed(uint16_t rpm) {
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > SPEED_MAX_RPM) rpm = SPEED_MAX_RPM;
    tap_state.speed_rpm = rpm;
    tap_settings.speed_rpm = rpm;
}

/*===========================================================================*/
/* Settings Getters/Setters                                                   */
/*===========================================================================*/

const tapping_settings_t* tapping_get_settings(void) {
    return &tap_settings;
}

// Trigger enables
void tapping_set_depth_trigger_enabled(bool enabled) {
    tap_settings.depth_trigger_enabled = enabled ? 1 : 0;
}

void tapping_set_load_increase_enabled(bool enabled) {
    tap_settings.load_increase_enabled = enabled ? 1 : 0;
}

void tapping_set_load_slip_enabled(bool enabled) {
    tap_settings.load_slip_enabled = enabled ? 1 : 0;
}

void tapping_set_clutch_slip_enabled(bool enabled) {
    tap_settings.clutch_slip_enabled = enabled ? 1 : 0;
}

void tapping_set_quill_trigger_enabled(bool enabled) {
    tap_settings.quill_trigger_enabled = enabled ? 1 : 0;
}

void tapping_set_peck_trigger_enabled(bool enabled) {
    tap_settings.peck_trigger_enabled = enabled ? 1 : 0;
}

void tapping_set_pedal_enabled(bool enabled) {
    tap_settings.pedal_enabled = enabled ? 1 : 0;
}

// Depth trigger settings
void tapping_set_depth_action(tap_depth_action_t action) {
    tap_settings.depth_action = action;
}

// Quill trigger settings (renamed from SMART)
void tapping_set_quill_pedal_mode(quill_pedal_mode_t mode) {
    tap_settings.quill_pedal_mode = mode;
}

quill_pedal_mode_t tapping_get_quill_pedal_mode(void) {
    return (quill_pedal_mode_t)tap_settings.quill_pedal_mode;
}

// Load increase settings
void tapping_set_load_increase_threshold(uint8_t threshold) {
    if (threshold > 100) threshold = 100;
    tap_settings.load_increase_threshold = threshold;
}

void tapping_set_load_increase_reverse_ms(uint16_t time_ms) {
    if (time_ms > 2000) time_ms = 2000;
    tap_settings.load_increase_reverse_ms = time_ms;
}

// Load slip settings
void tapping_set_load_slip_cv_percent(uint16_t percent) {
    if (percent < 110) percent = 110;  // Minimum 110%
    if (percent > 200) percent = 200;  // Maximum 200%
    tap_settings.load_slip_cv_percent = percent;
}

// Clutch slip settings
void tapping_set_clutch_plateau_ms(uint16_t ms) {
    if (ms < 100) ms = 100;
    if (ms > 2000) ms = 2000;
    tap_settings.clutch_plateau_ms = ms;
}

void tapping_set_clutch_action(clutch_action_t action) {
    tap_settings.clutch_action = action;
}

// Peck trigger settings
void tapping_set_peck_params(uint16_t fwd_ms, uint16_t rev_ms, uint8_t cycles) {
    if (fwd_ms < 50) fwd_ms = 50;
    if (fwd_ms > 5000) fwd_ms = 5000;
    if (rev_ms < 50) rev_ms = 50;
    if (rev_ms > 2000) rev_ms = 2000;
    if (cycles > 99) cycles = 99;

    tap_settings.peck_fwd_ms = fwd_ms;
    tap_settings.peck_rev_ms = rev_ms;
    tap_settings.peck_cycles = cycles;

    tap_state.fwd_time_ms = fwd_ms;
    tap_state.rev_time_ms = rev_ms;
}

void tapping_set_peck_depth_stop(bool stop_at_depth) {
    tap_settings.peck_depth_stop = stop_at_depth ? 1 : 0;
}

// Pedal settings
void tapping_set_pedal_action(pedal_action_t action) {
    tap_settings.pedal_action = action;
}

void tapping_set_pedal_chip_break_ms(uint16_t ms) {
    if (ms < TAP_MIN_CHIP_BREAK_MS) ms = TAP_MIN_CHIP_BREAK_MS;
    if (ms > TAP_MAX_CHIP_BREAK_MS) ms = TAP_MAX_CHIP_BREAK_MS;
    tap_settings.pedal_chip_break_ms = ms;
}

void tapping_calc_peck_timing(void) {
    // Direct ms values from settings - no calculation needed
    tap_state.fwd_time_ms = tap_settings.peck_fwd_ms;
    tap_state.rev_time_ms = tap_settings.peck_rev_ms;

    // Minimum timing
    if (tap_state.fwd_time_ms < 50) tap_state.fwd_time_ms = 50;
    if (tap_state.rev_time_ms < 50) tap_state.rev_time_ms = 50;
}

/*===========================================================================*/
/* Legacy Compatibility Wrappers                                              */
/*===========================================================================*/

// Wrapper functions for old code - map to new trigger-based functions
void tapping_set_load_threshold(uint8_t threshold) {
    tapping_set_load_increase_threshold(threshold);
}

void tapping_set_reverse_time(uint16_t time_ms) {
    tapping_set_load_increase_reverse_ms(time_ms);
}

void tapping_set_through_detect(bool enabled) {
    tapping_set_load_slip_enabled(enabled);
}

void tapping_set_smart_pedal_mode(quill_pedal_mode_t mode) {
    tapping_set_quill_pedal_mode(mode);
}

quill_pedal_mode_t tapping_get_smart_pedal_mode(void) {
    return tapping_get_quill_pedal_mode();
}

/*===========================================================================*/
/* Utility Functions                                                          */
/*===========================================================================*/
