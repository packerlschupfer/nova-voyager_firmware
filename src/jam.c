/**
 * @file jam.c
 * @brief Motor Jam/Stall Detection Implementation
 *
 * Phase 2.3: Expanded to include load-based jam detection
 * Implements time-based stall detection by monitoring
 * motor command vs. actual running state, plus load monitoring.
 */

#include "jam.h"
#include "motor.h"
#include "config.h"
#include "shared.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>

// Load-based detection constants (from task_motor.c)
#define JAM_LOAD_THRESHOLD      90      // 90% load = potential jam
#define JAM_LOAD_TIMEOUT_MS     5000    // 5 seconds sustained high load

// External UART function for logging
extern void uart_puts(const char* s);

/*===========================================================================*/
/* Private Variables (Phase 5.2: Thread-safety classified)                   */
/*===========================================================================*/

// [MODULE_LOCAL] Only accessed from motor task via public API
// No mutex needed - all calls from single task context
static jam_status_t jam_status;

// Timing state
static uint32_t motor_start_time = 0;       // When motor was commanded
static uint32_t stall_start_time = 0;       // When stall condition began
static uint32_t last_response_time = 0;     // Last motor controller response
static uint32_t vibration_start_time = 0;   // When high vibration began

// State tracking
static bool motor_was_running = false;
static bool startup_complete = false;

// Load-based detection state (Phase 2.3: from task_motor.c)
static TickType_t load_jam_start_time = 0;
static bool load_jam_condition_active = false;

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

/**
 * @brief Initialize jam detection system
 *
 * Resets all detection timers and clears jam status.
 * Call during system boot or after jam is cleared.
 *
 * Thread safety: Call from motor task during initialization
 */
void jam_init(void) {
    jam_status.type = JAM_NONE;
    jam_status.timestamp = 0;
    jam_status.duration_ms = 0;
    jam_status.acknowledged = false;

    motor_start_time = 0;
    stall_start_time = 0;
    last_response_time = HAL_GetTick();
    vibration_start_time = 0;
    motor_was_running = false;
    startup_complete = false;

    // Phase 2.3: Load-based detection init
    load_jam_start_time = 0;
    load_jam_condition_active = false;
}

/**
 * @brief Reset jam detection (clear current jam without acknowledging)
 *
 * Clears all detection timers but preserves jam status if not acknowledged.
 * Use jam_acknowledge() to fully clear jam state.
 *
 * Thread safety: Call from motor task
 */
void jam_reset(void) {
    jam_status.type = JAM_NONE;
    jam_status.acknowledged = false;
    stall_start_time = 0;
    vibration_start_time = 0;
    startup_complete = false;

    // Phase 2.3: Load-based detection reset
    load_jam_start_time = 0;
    load_jam_condition_active = false;
}

/**
 * @brief Notify jam detector that motor start was commanded
 *
 * Starts startup timeout monitoring. Motor must actually start running
 * within JAM_STARTUP_TIMEOUT_MS or jam will be triggered.
 *
 * Thread safety: Call from motor task when START command sent
 */
void jam_motor_started(void) {
    motor_start_time = HAL_GetTick();
    startup_complete = false;
    stall_start_time = 0;
    motor_was_running = false;
}

/**
 * @brief Notify jam detector that motor stopped normally
 *
 * Clears monitoring timers. Preserves jam state if not acknowledged
 * (allows jam message to persist on LCD after stop).
 *
 * Thread safety: Call from motor task when STOP command sent
 */
void jam_motor_stopped(void) {
    // Normal stop - clear any pending detection
    motor_start_time = 0;
    stall_start_time = 0;
    startup_complete = false;
    motor_was_running = false;

    // Only clear jam if acknowledged or no jam
    if (jam_status.acknowledged || jam_status.type == JAM_NONE) {
        jam_status.type = JAM_NONE;
    }
}

static void trigger_jam(jam_type_t type, uint16_t duration) {
    jam_status.type = type;
    jam_status.timestamp = HAL_GetTick();
    jam_status.duration_ms = duration;
    jam_status.acknowledged = false;

    // Emergency stop the motor
    motor_emergency_stop();
}

/**
 * @brief Update jam detection state machine (call periodically)
 *
 * Monitors multiple jam conditions:
 * - Startup timeout: Motor commanded but doesn't start within 2s
 * - Stall detection: Motor stops while commanded after successful start
 * - Communication timeout: No status updates for 5s
 * - Vibration: Sustained high vibration for 3s
 *
 * @param motor_running true if motor reports running state
 * @param motor_commanded true if motor has been commanded to run
 * @return true if jam triggered this update, false otherwise
 *
 * Thread safety: Call from motor task only (uses motor_get_status())
 * Call frequency: 10-20 Hz recommended
 *
 * @note On jam detection, calls motor_emergency_stop() and sends EVT_JAM_DETECTED
 */
bool jam_update(bool motor_running, bool motor_commanded) {
    uint32_t now = HAL_GetTick();

    // Skip if jam already active and not acknowledged
    if (jam_status.type != JAM_NONE && !jam_status.acknowledged) {
        return true;
    }

    // Clear previous jam if acknowledged
    if (jam_status.acknowledged) {
        jam_status.type = JAM_NONE;
        jam_status.acknowledged = false;
    }

    // Not monitoring if motor not commanded
    if (!motor_commanded) {
        motor_was_running = false;
        startup_complete = false;
        stall_start_time = 0;
        return false;
    }

    // === Startup Timeout Detection ===
    if (!startup_complete && motor_start_time > 0) {
        if (motor_running) {
            // Motor started successfully
            startup_complete = true;
            motor_was_running = true;
        } else {
            // Check for startup timeout
            uint32_t elapsed = now - motor_start_time;
            if (elapsed > JAM_STARTUP_TIMEOUT_MS) {
                trigger_jam(JAM_STARTUP_TIMEOUT, elapsed);
                return true;
            }
        }
    }

    // === Stall Detection (after successful startup) ===
    if (startup_complete) {
        if (motor_running) {
            // Motor is running normally
            motor_was_running = true;
            stall_start_time = 0;
        } else if (motor_was_running) {
            // Motor was running but now stopped while commanded
            if (stall_start_time == 0) {
                stall_start_time = now;
            } else {
                uint32_t stall_duration = now - stall_start_time;
                if (stall_duration > JAM_STALL_TIMEOUT_MS) {
                    trigger_jam(JAM_STALL_DETECTED, stall_duration);
                    return true;
                }
            }
        }
    }

    // === Communication Timeout Detection ===
    // This would be updated when motor_update() receives a response
    // For now, we track based on the motor module's last_update_ms
    const motor_status_t* status = motor_get_status();
    if (status->last_update_ms > 0) {
        last_response_time = status->last_update_ms;
    }

    if (motor_commanded && (now - last_response_time) > JAM_COMM_TIMEOUT_MS) {
        trigger_jam(JAM_COMM_TIMEOUT, now - last_response_time);
        return true;
    }

    // === Vibration Detection ===
    if (motor_commanded && motor_running) {
        uint16_t vibration = motor_get_vibration();
        if (vibration > JAM_VIBRATION_THRESHOLD) {
            if (vibration_start_time == 0) {
                vibration_start_time = now;
            } else if ((now - vibration_start_time) > JAM_VIBRATION_TIMEOUT_MS) {
                trigger_jam(JAM_VIBRATION, now - vibration_start_time);
                return true;
            }
        } else {
            vibration_start_time = 0;  // Reset if vibration drops
        }
    }

    return false;
}

/**
 * @brief Check if jam is currently active and unacknowledged
 * @return true if jam active, false if no jam or jam acknowledged
 *
 * Thread safety: Safe from any task (read-only)
 */
bool jam_is_active(void) {
    return (jam_status.type != JAM_NONE && !jam_status.acknowledged);
}

const jam_status_t* jam_get_status(void) {
    return &jam_status;
}

/**
 * @brief Acknowledge current jam (allows clearing on next motor_stopped())
 *
 * Sets acknowledged flag. Jam will be fully cleared on next motor_stopped() call.
 *
 * Thread safety: Safe from any task
 */
void jam_acknowledge(void) {
    jam_status.acknowledged = true;
}

const char* jam_get_description(jam_type_t type) {
    switch (type) {
        case JAM_NONE:
            return "No Jam";
        case JAM_STARTUP_TIMEOUT:
            return "Motor Start Fail";
        case JAM_STALL_DETECTED:
            return "Motor Stalled!";
        case JAM_COMM_TIMEOUT:
            return "Comm Timeout";
        case JAM_OVERCURRENT:
            return "Overcurrent!";
        case JAM_VIBRATION:
            return "High Vibration!";
        case JAM_LOAD_SUSTAINED:
            return "Sustained Overload";
        case JAM_LOAD_SPIKE:
            return "Load Spike!";
        default:
            return "Unknown Jam";
    }
}

/*===========================================================================*/
/* Load-Based Jam Detection (Phase 2.3: Extracted from task_motor.c)        */
/*===========================================================================*/

bool jam_load_update(uint8_t load_pct, bool is_running, bool jam_detect_enabled,
                     bool spike_detect_enabled, uint8_t spike_threshold) {
    // Skip if not enabled or motor not running
    if (!jam_detect_enabled || !is_running) {
        load_jam_condition_active = false;
        load_jam_start_time = 0;
        return false;
    }

    TickType_t now = xTaskGetTickCount();

    // Check for high load condition (sustained jam detection)
    if (load_pct >= JAM_LOAD_THRESHOLD) {
        if (!load_jam_condition_active) {
            // Start jam timer
            load_jam_condition_active = true;
            load_jam_start_time = now;
        } else {
            // Check if jam timeout exceeded
            uint32_t elapsed_ms = (now - load_jam_start_time) * portTICK_PERIOD_MS;
            if (elapsed_ms >= JAM_LOAD_TIMEOUT_MS) {
                // JAM DETECTED - trigger and stop motor
                trigger_jam(JAM_LOAD_SUSTAINED, elapsed_ms);
                SEND_EVENT(EVT_JAM_DETECTED);
                load_jam_condition_active = false;
                return true;
            }
        }

        // Immediate stop on load spike (configurable threshold)
        if (spike_detect_enabled && load_pct >= spike_threshold) {
            trigger_jam(JAM_LOAD_SPIKE, 0);
            SEND_EVENT(EVT_LOAD_SPIKE);
            load_jam_condition_active = false;
            return true;
        }
    } else {
        // Load returned to normal - reset jam timer
        load_jam_condition_active = false;
        load_jam_start_time = 0;
    }

    return false;
}
