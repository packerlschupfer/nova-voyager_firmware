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
    JAM_LOAD_SPIKE,         // Immediate load spike (Phase 2.3: from task_motor.c)
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
 * @brief Notify jam detector that motor was commanded to start
 */
void jam_motor_started(void);

/**
 * @brief Notify jam detector that motor was commanded to stop
 */
void jam_motor_stopped(void);

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

/**
 * @brief Update load-based jam detection (Phase 2.3: Extracted from task_motor.c)
 * @param load_pct Motor load percentage (0-100%)
 * @param is_running True if motor is running
 * @param jam_detect_enabled True if jam detection is enabled in settings
 * @param spike_detect_enabled True if spike detection is enabled in settings
 * @param spike_threshold Load percentage threshold for immediate spike stop
 * @return True if jam or spike detected and motor stopped
 *
 * Monitors load over time and triggers:
 * - JAM_LOAD_SUSTAINED: Load >= 90% for 5 seconds
 * - JAM_LOAD_SPIKE: Load >= spike_threshold immediately
 */
bool jam_load_update(uint8_t load_pct, bool is_running, bool jam_detect_enabled,
                     bool spike_detect_enabled, uint8_t spike_threshold);

#endif /* JAM_H */
