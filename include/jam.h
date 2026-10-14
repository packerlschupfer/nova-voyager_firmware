/**
 * @file jam.h
 * @brief Motor Jam/Stall Detection
 *
 * Monitors motor operation and detects stall conditions:
 * - Startup timeout (motor doesn't start within threshold)
 * - Runtime stall (motor stops while commanded to run)
 * - Communication timeout (no response from motor controller)
 */

#ifndef JAM_H
#define JAM_H

#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Jam Detection Status                                                      */
/*===========================================================================*/

typedef enum {
    JAM_NONE = 0,           // No jam detected
    JAM_STARTUP_TIMEOUT,    // Motor didn't start in time
    JAM_STALL_DETECTED,     // Motor stalled during operation
    JAM_COMM_TIMEOUT,       // Communication with motor controller lost
    JAM_OVERCURRENT,        // Current exceeded threshold (if sensing available)
    JAM_VIBRATION,          // Excessive vibration detected
    JAM_LOAD_SUSTAINED,     // Sustained high load (Phase 2.3: from task_motor.c)
    JAM_LOAD_SPIKE,         // Immediate load spike (filtered > baseline+delta)
    JAM_LOAD_STEP,          // OEM-style: instant KR delta > step_thresh (bit catches)
    JAM_LOW_LOAD,           // Belt break / tool detach (KR<floor + CV<25 for 18 samples)
} jam_type_t;

typedef struct {
    jam_type_t type;
    uint32_t timestamp;     // When jam was detected
    uint16_t duration_ms;   // How long condition persisted
    bool acknowledged;      // User has acknowledged the jam
} jam_status_t;

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

// Timeout thresholds (milliseconds)
#define JAM_STARTUP_TIMEOUT_MS      3000    // 3 seconds to start
#define JAM_STALL_TIMEOUT_MS        500     // 500ms stall triggers jam
#define JAM_COMM_TIMEOUT_MS         1000    // 1 second without response

// Vibration threshold (0-1000 scale)
#define JAM_VIBRATION_THRESHOLD     800     // Excessive vibration level
#define JAM_VIBRATION_TIMEOUT_MS    200     // Sustained vibration time

/*===========================================================================*/
/* Spindle regulation band — ONE definition, used by two modules             */
/*===========================================================================*/

/* "Is the spindle failing to hold the speed it was commanded?"
 *
 * This lives in the header, and is an inline rather than a pair of constants,
 * because TWO modules have to agree on it exactly:
 *
 *   - jam.c's droop detector trips when the spindle is OUTSIDE this band;
 *   - motor_load.c's at-target gate arms when it has been INSIDE it.
 *
 * Those are the same question asked in opposite directions, and the gate is
 * only sound if the two bands are identical. They were not when the gate was
 * first written on 2026-10-14: arming used motor_load's ±5% baseline window
 * while tripping used ±10%, which left a dead zone - a spindle settling 5-10%
 * under setpoint, which is an ordinary heavy cut, never armed the gate at all
 * and so disarmed the droop detector for the whole run. Two constants in two
 * files drift; one function cannot.
 *
 * One-sided on purpose: CV above SV is an overshoot on a ramp-down, not a jam.
 * The RPM floor keeps it meaningful at low commanded speeds, where 10% is a
 * handful of RPM. */
#define JAM_DROOP_PCT               10      // CV this far below SV = out of regulation
#define JAM_DROOP_MIN_RPM           60      // ...but never less than this many RPM

static inline bool jam_spindle_out_of_regulation(uint16_t cv, uint16_t sv) {
    if (sv == 0) return false;
    uint16_t tol = (uint16_t)(((uint32_t)sv * JAM_DROOP_PCT) / 100);
    if (tol < JAM_DROOP_MIN_RPM) tol = JAM_DROOP_MIN_RPM;
    return (uint32_t)cv + tol < (uint32_t)sv;
}

/* CV below this counts as "the spindle is not turning" (low-load detector),
 * and is also the smallest CV that may arm the at-target gate. Shared for the
 * same reason as the band above: motor_load.c needs it too. */
#define JAM_LOW_LOAD_CV_THRESHOLD   25
#define JAM_CV_VALID_RPM            JAM_LOW_LOAD_CV_THRESHOLD

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

/**
 * @brief Initialize jam detection
 */
void jam_init(void);

/**
 * @brief Reset jam detection state (call when motor stops normally)
 */
void jam_reset(void);

/**
 * @brief Notify jam detector that motor was commanded to start.
 *        Resets startup-timeout / stall state. Inrush grace + baseline
 *        learning is owned by motor_load — call motor_load_motor_started()
 *        in addition (the motor task calls both).
 */
void jam_motor_started(void);

/**
 * @brief Notify jam detector that motor was commanded to stop
 */
void jam_motor_stopped(void);

/**
 * @brief Notify jam detector that a valid MCB response was just received.
 *        Call from the motor task's hot path after every successful poll
 *        (GF/SV/CV/KR/T0). Without this the comm-timeout check false-fires
 *        on the first drilling run: `last_response_time` is initialized at
 *        boot and never advances since motor_status.last_update_ms is
 *        never written by task_motor's hot path.
 */
void jam_notify_response(void);

/**
 * @brief Update jam detection (call from main loop)
 * @param motor_running True if motor is actually running (feedback)
 * @param motor_commanded True if motor is commanded to run
 * @return True if jam detected
 */
bool jam_update(bool motor_running, bool motor_commanded);

/**
 * @brief Check if jam is currently active
 */
bool jam_is_active(void);

/**
 * @brief Get current jam status
 */
const jam_status_t* jam_get_status(void);

/**
 * @brief Acknowledge jam (clears the fault after user confirmation)
 */
void jam_acknowledge(void);

/**
 * @brief Get human-readable jam description
 */
const char* jam_get_description(jam_type_t type);

/* Smallest step-delta threshold that can mean anything.
 *
 * REVIEW FIX: jam.c's step detector compares a RAW KR delta against the user
 * setting with no debounce, so a threshold of 1 or 2 emergency-stops on
 * ordinary sample jitter. The spike detector has had both a settings floor and
 * an in-detector JAM_SPIKE_MIN_THRESH guard for exactly this reason; step had
 * neither. Lives in the header because settings.c enforces it on the way in and
 * jam.c enforces it again at the point of use — a value that arrived from an
 * EEPROM blob rather than a setter still has to be refused. */
#define JAM_STEP_MIN_THRESH  5

/* The live load-detector trip points, derived from the operator's settings.
 * JAMINFO prints these so "what protection is live before I cut" has one
 * answer computed by the same code the detectors use, rather than a second
 * copy of the arithmetic in the console handler. */
typedef struct {
    uint8_t  spike_thresh;       /* (a) immediate trip, % filtered load */
    uint8_t  sustained_thresh;   /* (b) trip after sustained_ms, % filtered */
    uint16_t sustained_ms;       /* (b) how long (b) must hold */
    uint8_t  droop_pct;          /* (c) CV this far below SV = out of regulation */
    uint16_t droop_min_rpm;      /* (c) ...but never fewer RPM than this */
    uint8_t  grab_min_load;      /* (c) droop below this load is not a grab */
    uint8_t  step_abs_floor;     /* (c) a step landing here trips on its own */
    uint8_t  step_confirm;       /* (c) consecutive qualified samples needed */
} jam_load_config_t;

/**
 * @brief Resolve the operator's spike threshold into the live trip points.
 * @param spike_threshold  settings.sensor.spike_thresh
 * @param[out] out         filled with the effective configuration
 */
/**
 * @brief Has CV reported a usable speed at any point this run?
 *
 * False means the low-load and droop detectors are standing down: they cannot
 * tell a stopped spindle from a run that never produced a speed reading.
 */
bool jam_cv_feedback_seen(void);

void jam_get_load_config(uint8_t spike_threshold, jam_load_config_t *out);

/**
 * @brief Evaluate load-based jam triggers (spike + sustained).
 *
 * Reads filtered load, baseline, and spike-grace state from motor_load —
 * caller is responsible for advancing motor_load via motor_load_update()
 * before invoking this.
 *
 * @param is_running True if motor is commanded and running
 * @param jam_detect_enabled True if jam detection is enabled in settings
 * @param spike_detect_enabled True if spike detection is enabled in settings
 * @param spike_threshold Absolute spike ceiling (%); user setting
 * @return True if a jam was triggered this update
 */
bool jam_load_update(bool is_running, bool jam_detect_enabled,
                     bool spike_detect_enabled, uint8_t spike_threshold,
                     uint8_t step_threshold,
                     bool low_load_detect_enabled, uint8_t low_load_threshold);

#endif /* JAM_H */
