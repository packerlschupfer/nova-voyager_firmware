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
