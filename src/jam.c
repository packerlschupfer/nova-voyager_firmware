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
#include "motor_load.h"
#include "config.h"
#include "shared.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>

// Load-jam thresholds. Three-tier coverage:
//   (a) absolute safety ceiling — always applies (defense in depth, no OEM)
//   (b) baseline-relative delta — once motor_load has armed the baseline
//   (c) OEM-style step delta on raw — instant, catches the bit biting
//
// And one no-load detector (OEM-parity):
//   (d) low-load = KR<floor AND CV<25 RPM for 18 samples → belt break / detach
#define JAM_LOAD_THRESHOLD          90      // (a) absolute sustained ceiling (%)
#define JAM_LOAD_TIMEOUT_MS         5000    // (a)+(b) sustained timer
#define JAM_SPIKE_DELTA_PCT         25      // (b) filtered > baseline + delta
/* Smallest spike cap that can mean anything. Below the sustained-jam delta a
 * "spike" cap would trip before the sustained detector ever could. */
#define JAM_SPIKE_MIN_THRESH        20
#define JAM_SUSTAINED_DELTA_PCT     30      // (b) sustained timer trigger
#define JAM_LOW_LOAD_CV_THRESHOLD   25      // (d) CV below this counts as "stopped"
#define JAM_LOW_LOAD_DEBOUNCE       18      // (d) consecutive low-load samples (~900 ms at 20 Hz)

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

// Load-based detection state (timer for sustained-load trigger)
static TickType_t load_jam_start_time = 0;
static bool load_jam_condition_active = false;
static uint8_t low_load_count = 0;          // (d) consecutive-samples debounce

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

    load_jam_start_time = 0;
    load_jam_condition_active = false;
    low_load_count = 0;
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
    low_load_count = 0;
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
    low_load_count = 0;

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

    // AUDIT FIX (MEDIUM, task_motor.c:700 + jam.c:173): the documented contract
    // is "sends EVT_JAM_DETECTED" but the only place that actually sent one was
    // the load-jam path — startup/stall/comm/vibration all trip the emergency
    // stop and leave the UI showing DRILLING with no error message. Sending
    // the event here fixes all four detectors at once. handle_jam_detected in
    // events.c owns the state transition to APP_STATE_ERROR + the "! DRILL
    // JAM !" screen.
    /* AUDIT FIX (LOW, jam.c:417): the four load paths used to send their own
     * event on top of this one. Two were plain duplicates of EVT_JAM_DETECTED;
     * the spike and step paths sent EVT_LOAD_SPIKE as well, so ONE trip queued
     * TWO events. They were then handled in order — handle_jam_detected() set
     * APP_STATE_ERROR and the persistent "DRILL BIT JAM" screen, then
     * handle_load_spike() overwrote it with a 2-second "LOAD SPIKE" message
     * and returned. Two seconds later the message expired and the operator was
     * left looking at an apparently normal screen on a machine that had
     * latched ERROR and would refuse to start, with nothing to say why.
     *
     * One trip, one event. handle_jam_detected() picks its message from
     * jam_get_status()->type, so each detector still names itself. */
    SEND_EVENT(EVT_JAM_DETECTED);
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
    /* AUDIT FIX (LOW, jam.c:246): this used to overwrite last_response_time —
     * which jam_notify_response() sets from the motor task's hot path, the only
     * place that actually knows a reply arrived — with
     * motor_get_status()->last_update_ms. That field is written solely by
     * motor_read_response(), which has zero callers, so it stays 0 and the
     * `> 0` guard made the overwrite dead code. Harmless today, and a
     * guaranteed false JAM_COMM_TIMEOUT on every motor start the moment
     * anyone gives motor_read_response() a caller. jam_notify_response() is
     * the single authority; the shadow copy is gone.
     */
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
void jam_notify_response(void) {
    // BUGFIX (2026-07-09): the audit's Phase 2 fix added jam_init() at boot,
    // but the hot path never wrote motor_status.last_update_ms, so
    // last_response_time stayed frozen at boot tick. On first motor start
    // (minutes/hours later) the comm-timeout check saw (now - boot) >> 1 s
    // and false-fired JAM_COMM_TIMEOUT — worse now, because Phase 2 also
    // routed JAM_COMM_TIMEOUT through EVT_JAM_DETECTED → "DRILL BIT JAM"
    // screen. Motor task now calls this from every successful response.
    last_response_time = HAL_GetTick();
}

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
        case JAM_LOAD_STEP:
            return "Load Step!";
        case JAM_LOW_LOAD:
            return "No Load!";
        default:
            return "Unknown Jam";
    }
}

/*===========================================================================*/
/* Load-Based Jam Detection (Phase 2.3: Extracted from task_motor.c)        */
/*===========================================================================*/

bool jam_load_update(bool is_running, bool jam_detect_enabled,
                     bool spike_detect_enabled, uint8_t spike_threshold,
                     uint8_t step_threshold,
                     bool low_load_detect_enabled, uint8_t low_load_threshold) {
    /* REVIEW FIX: jam_update() takes this early-out and this function did not,
     * so a latched load jam re-entered the detectors on every poll and
     * trigger_jam() re-issued motor_emergency_stop() and re-posted the event
     * until task_main was next scheduled to handle it. One jam, one stop. */
    if (jam_status.type != JAM_NONE && !jam_status.acknowledged) {
        return true;
    }
    if (!is_running) {
        load_jam_condition_active = false;
        load_jam_start_time = 0;
        low_load_count = 0;
        return false;
    }

    // Consume the load filter/baseline/grace state from motor_load.
    uint8_t filtered = motor_load_get_filtered();
    uint8_t raw = motor_load_get_raw();
    int8_t step_delta = motor_load_get_step_delta();
    uint8_t baseline = 0;
    bool baseline_armed = motor_load_get_baseline(&baseline);
    bool in_inrush = motor_load_in_spike_grace();

    // (d) Low-load detector (OEM-parity) — independent of jam_detect_enabled
    // because belt break / tool detach is a different failure class. Inrush
    // grace suppresses it so spin-up doesn't trip it (CV briefly 0).
    if (low_load_detect_enabled && !in_inrush) {
        uint16_t cv = g_state.current_rpm;
        if (raw < low_load_threshold && cv < JAM_LOW_LOAD_CV_THRESHOLD) {
            if (++low_load_count >= JAM_LOW_LOAD_DEBOUNCE) {
                trigger_jam(JAM_LOW_LOAD, 0);
                low_load_count = 0;
                return true;
            }
        } else {
            low_load_count = 0;
        }
    } else {
        low_load_count = 0;
    }

    if (!jam_detect_enabled) {
        load_jam_condition_active = false;
        load_jam_start_time = 0;
        return false;
    }

    // (c) OEM-style step delta on raw (instant) — fastest detector, catches
    // the moment the bit catches before the EMA has a chance to react.
    // Active during the learn phase too; inrush-grace gates it during
    // motor spin-up. step_threshold == 0 disables this path.
    if (spike_detect_enabled && step_threshold >= JAM_STEP_MIN_THRESH && !in_inrush
        && step_delta > 0 && (uint8_t)step_delta >= step_threshold) {
        trigger_jam(JAM_LOAD_STEP, 0);
        load_jam_condition_active = false;
        return true;
    }

    // (a)+(b) Filtered spike and sustained checks only run once the baseline
    // has armed (motor at target speed for the full stability window). Before
    // that, the EMA can legitimately climb past 90% during inrush on harder
    // ramps (e.g. 2000->5000 observed at 91% filtered). During the learn
    // phase, step (c) and low-load (d) handle safety.
    if (!baseline_armed) {
        load_jam_condition_active = false;
        load_jam_start_time = 0;
        return false;
    }

    // Effective thresholds — baseline-relative; user setting caps both.
    uint16_t s_rel = (uint16_t)baseline + JAM_SUSTAINED_DELTA_PCT;
    if (s_rel > JAM_LOAD_THRESHOLD) s_rel = JAM_LOAD_THRESHOLD;
    uint8_t sustained_thresh = (uint8_t)s_rel;
    uint16_t k_rel = (uint16_t)baseline + JAM_SPIKE_DELTA_PCT;
    /* AUDIT FIX (MEDIUM, jam.c:412): spike_threshold is a CAP on the trip
     * point, so a value of 0 — which the console setter used to accept —
     * collapsed the test to `filtered >= 0` and tripped an emergency stop
     * seconds into every drill. The setter now clamps to >= 20, but a stale
     * EEPROM blob or a future caller could still hand us nonsense, and the
     * consequence is an unexpected emergency stop mid-cut. Ignore an
     * unusable cap rather than acting on it. */
    if (spike_threshold >= JAM_SPIKE_MIN_THRESH && k_rel > spike_threshold) {
        k_rel = spike_threshold;
    }
    uint8_t spike_thresh_eff = (uint8_t)k_rel;

    if (spike_detect_enabled && filtered >= spike_thresh_eff && !in_inrush) {
        trigger_jam(JAM_LOAD_SPIKE, 0);
        load_jam_condition_active = false;
        return true;
    }

    TickType_t now = xTaskGetTickCount();
    if (filtered >= sustained_thresh) {
        if (!load_jam_condition_active) {
            load_jam_condition_active = true;
            load_jam_start_time = now;
        } else {
            uint32_t elapsed_ms = (now - load_jam_start_time) * portTICK_PERIOD_MS;
            if (elapsed_ms >= JAM_LOAD_TIMEOUT_MS) {
                trigger_jam(JAM_LOAD_SUSTAINED, elapsed_ms);
                load_jam_condition_active = false;
                return true;
            }
        }
    } else {
        load_jam_condition_active = false;
        load_jam_start_time = 0;
    }

    return false;
}
