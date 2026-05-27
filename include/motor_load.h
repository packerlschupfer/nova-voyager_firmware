/**
 * @file motor_load.h
 * @brief Motor load (KR) filter + idle-baseline learning + inrush/stability windows.
 *
 * Service module: characterizes the motor's load signal so consumers
 * (jam detection, display split-bar, tapping triggers) can act on smoothed,
 * baseline-relative numbers instead of raw single samples.
 *
 *  raw KR ──► EMA filter ──► filtered_load ──┬─► jam.c (spike + sustained)
 *                                            ├─► display.c (cutting = load - baseline)
 *                                            └─► (future) tapping
 *
 * Lifecycle:
 *   motor_load_motor_started(target)         on motor START
 *   motor_load_motor_speed_change(old, new)  on SET_SPEED while running
 *   motor_load_motor_stopped()               on motor STOP
 *   motor_load_update(raw, cv, sv, running)  every motor poll cycle
 */

#ifndef MOTOR_LOAD_H
#define MOTOR_LOAD_H

#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

void motor_load_init(void);

/**
 * @brief Arm inrush grace + stability window for a fresh motor run.
 *        Resets filter and baseline.
 */
void motor_load_motor_started(uint16_t target_rpm);

/**
 * @brief Step-up extends inrush grace; either direction invalidates the
 *        baseline (new operating point). Filter is kept continuous.
 */
void motor_load_motor_speed_change(uint16_t old_target, uint16_t new_target);

/**
 * @brief Clear all per-run state. Filter and baseline reset.
 */
void motor_load_motor_stopped(void);

/**
 * @brief Feed a KR sample. Advances EMA filter and baseline-learning state.
 *
 * @param raw_load   Raw KR percent (0-100)
 * @param cv         Current motor RPM (used to detect baseline-ready window)
 * @param sv         Target motor RPM (used as the ±tolerance center)
 * @param is_running True while motor is commanded and running
 */
void motor_load_update(uint8_t raw_load, uint16_t cv, uint16_t sv, bool is_running);

/** @return EMA-filtered load percent (0 before first sample / after stop). */
uint8_t motor_load_get_filtered(void);

/** @return Last raw KR sample fed to motor_load_update (0 if none). */
uint8_t motor_load_get_raw(void);

/**
 * @brief Signed step delta between the most recent two raw KR samples.
 *        Positive = load rose this poll cycle. Zero on the very first sample
 *        after motor start (no previous to compare).
 */
int8_t motor_load_get_step_delta(void);

/**
 * @brief Read the learned idle-load baseline.
 * @param[out] out  Baseline percent when armed; untouched otherwise.
 * @return true if armed, false if still learning.
 */
bool motor_load_get_baseline(uint8_t *out);

/** @return true if the inrush-grace deadline has not passed. */
bool motor_load_in_spike_grace(void);

typedef struct {
    uint8_t  filtered_load;
    uint8_t  baseline;
    bool     baseline_armed;
    bool     filter_initialized;
    uint32_t spike_grace_remaining_ms;
    uint32_t stability_elapsed_ms;
    uint32_t stability_required_ms;
} motor_load_debug_t;

/** @brief Diagnostic snapshot — used by LOADINFO / JAMINFO console commands. */
void motor_load_get_debug(motor_load_debug_t *out);

#endif /* MOTOR_LOAD_H */
