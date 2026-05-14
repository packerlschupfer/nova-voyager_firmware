/**
 * @file tapping.h
 * @brief Tapping Trigger System
 *
 * NOTE: The actual tapping state machine is in task_tapping.c (FreeRTOS task).
 * This file provides settings management and utility functions only.
 *
 * Implements combinable triggers for automatic tapping control:
 *   - Depth: Stop/reverse at target depth
 *   - Load Increase: Detect KR spikes (blind holes)
 *   - Load Slip: Detect CV overshoot (through holes)
 *   - Clutch Slip: Detect load plateau (torque limiter)
 *   - Quill: Auto-reverse on quill direction change
 *   - Peck: Timed forward/reverse cycles
 *   - Pedal: Manual chip break control
 */

#ifndef TAPPING_H
#define TAPPING_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/*===========================================================================*/
/* Tapping State (used by task_tapping.c)                                     */
/*===========================================================================*/

typedef enum {
    TAP_STATE_IDLE = 0,         // Motor stopped, waiting for start
    TAP_STATE_CUTTING,          // Motor forward, monitoring all triggers (inc. PECK timer)
    TAP_STATE_REVERSING,        // Motor reverse, checking completion
    TAP_STATE_TRANSITION        // Brake delay between direction changes
    // REMOVED: CHIP_BREAK (unused), PECK_FWD/REV/PAUSE (unified), COMPLETE (unused)
} tap_state_t;

typedef struct {
    tap_state_t state;          // Current state machine state
    bool pedal_pressed;         // Foot pedal (guard) state
    int16_t depth_current;      // Current depth (0.1mm units)
    int16_t depth_previous;     // Previous depth reading
    int16_t depth_target;       // Target depth (for display)
    uint32_t state_enter_time;  // Timestamp of state entry
    uint16_t speed_rpm;         // Tapping speed
    uint8_t tap_count;          // Number of taps completed

    // Extended state tracking for new modes
    uint8_t load_baseline;      // Learned no-load baseline (%)
    uint8_t current_load;       // Current motor load (%)
    uint8_t peck_cycle;         // Current peck cycle number
    uint8_t peck_total_cycles;  // Total cycles to complete
    uint32_t fwd_time_ms;       // Calculated forward time for peck
    uint32_t rev_time_ms;       // Calculated reverse time for peck
    bool depth_reached;         // Target depth has been reached
    bool through_hole_exit;     // Through-hole exit detected
} tapping_state_t;

/*===========================================================================*/
/* Tapping Parameter Limits                                                  */
/*===========================================================================*/

// Load increase threshold (%)
#define TAP_LOAD_THRESHOLD_MIN      10
#define TAP_LOAD_THRESHOLD_MAX      100
#define TAP_LOAD_THRESHOLD_DEFAULT  30

// Reverse time after load spike (ms)
#define TAP_REVERSE_TIME_MIN        50
#define TAP_REVERSE_TIME_MAX        2000
#define TAP_REVERSE_TIME_DEFAULT    300

// Peck cycle timings (ms)
#define TAP_PECK_FWD_MS_MIN         50
#define TAP_PECK_FWD_MS_MAX         5000
#define TAP_PECK_FWD_MS_DEFAULT     500

#define TAP_PECK_REV_MS_MIN         50
#define TAP_PECK_REV_MS_MAX         2000
#define TAP_PECK_REV_MS_DEFAULT     200

// Chip break delay (ms)
#define TAP_CHIP_BREAK_DELAY_MIN    50
#define TAP_CHIP_BREAK_DELAY_MAX    500
#define TAP_CHIP_BREAK_DELAY_DEFAULT 100

/*===========================================================================*/
/* Speed Functions                                                            */
/*===========================================================================*/

/**
 * @brief Set tapping speed
 * @param rpm Speed in RPM
 */
void tapping_set_speed(uint16_t rpm);

/*===========================================================================*/
/* Trigger Enable Functions                                                   */
/*===========================================================================*/

void tapping_set_depth_trigger_enabled(bool enabled);
void tapping_set_load_increase_enabled(bool enabled);
void tapping_set_load_slip_enabled(bool enabled);
void tapping_set_clutch_slip_enabled(bool enabled);
void tapping_set_quill_trigger_enabled(bool enabled);
void tapping_set_peck_trigger_enabled(bool enabled);
void tapping_set_pedal_enabled(bool enabled);

/*===========================================================================*/
/* Settings Functions                                                         */
/*===========================================================================*/

/**
 * @brief Get tapping settings
 * @return Pointer to current settings
 */
const tapping_settings_t* tapping_get_settings(void);

/**
 * @brief Set depth mode action (stop or reverse at target)
 * @param action TAP_DEPTH_ACTION_STOP or TAP_DEPTH_ACTION_REVERSE
 */
void tapping_set_depth_action(tap_depth_action_t action);

/**
 * @brief Set QUILL mode pedal override behavior
 * @param mode QUILL_PEDAL_OFF, QUILL_PEDAL_REVERSE, or QUILL_PEDAL_TOGGLE
 */
void tapping_set_quill_pedal_mode(quill_pedal_mode_t mode);

/**
 * @brief Get QUILL mode pedal override behavior
 * @return Current quill_pedal_mode_t setting
 */
quill_pedal_mode_t tapping_get_quill_pedal_mode(void);

/**
 * @brief Set load mode threshold
 * @param threshold Load % above baseline to trigger reversal (0-100)
 */
void tapping_set_load_threshold(uint8_t threshold);

/**
 * @brief Set load mode reverse time
 * @param time_ms Duration of reversal in milliseconds
 */
void tapping_set_reverse_time(uint16_t time_ms);

/**
 * @brief Enable/disable through-hole detection in load mode
 * @param enabled true to auto-stop when tap exits material
 */
void tapping_set_through_detect(bool enabled);

/**
 * @brief Set peck drilling parameters (time-based pulses)
 * @param fwd_ms Forward pulse duration in milliseconds
 * @param rev_ms Reverse pulse duration in milliseconds
 * @param cycles Number of peck cycles (0=infinite until depth)
 */
void tapping_set_peck_params(uint16_t fwd_ms, uint16_t rev_ms, uint8_t cycles);

/**
 * @brief Set peck mode depth stop behavior
 * @param stop_at_depth true to stop at target depth, false to complete all cycles
 */
void tapping_set_peck_depth_stop(bool stop_at_depth);

/**
 * @brief Calculate peck timing from RPM and turns
 * Called internally when peck parameters or speed change
 */
void tapping_calc_peck_timing(void);

/*===========================================================================*/
/* Utility Functions                                                          */
/*===========================================================================*/

#endif /* TAPPING_H */
