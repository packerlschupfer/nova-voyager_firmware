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
//   (a) absolute ceiling, from the operator's Spike Thresh setting — immediate
//       (defense in depth; the OEM has no equivalent)
//   (b) the same ceiling less a fixed margin, held for JAM_LOAD_TIMEOUT_MS
//   (c) OEM-style step delta on raw, qualified by spindle droop — instant,
//       catches the bit grabbing
//
// And one no-load detector (OEM-parity):
//   (d) low-load = KR<floor AND CV<25 RPM for 18 samples → belt break / detach
#define JAM_LOAD_THRESHOLD          90      // (a) fallback ceiling when the setting is unusable
#define JAM_LOAD_TIMEOUT_MS         5000    // (b) sustained timer
#define JAM_SUSTAINED_MARGIN_PCT    15      // (b) sustained tier sits this far below (a)
/* Smallest threshold that can mean anything as a trip point. */
#define JAM_SPIKE_MIN_THRESH        20
/* JAM_LOW_LOAD_CV_THRESHOLD and JAM_CV_VALID_RPM now live in jam.h -
 * motor_load.c's at-target gate needs the same number. Why the gate exists:
 *
 * CV must prove itself once per run before any detector may read "CV is low"
 * as "the spindle is not turning".
 *
 * Measured 2026-10-13, and the cause is NOT yet known. A start at SV=500 on
 * this machine leaves CV reading 0 for the whole run while the spindle turns;
 * starts at 50, 600, 610 and 2000 all report CV correctly, and once running,
 * the speed can be wound anywhere down to 50 with CV tracking. So it is not a
 * low-speed limit, not the spin-up grace, and not a regression of ours - the
 * pre-2026-10-13 build behaves identically. Only the value 500 reproduces it.
 *
 * This gate does not depend on knowing why. Two detectors treat a low CV as
 * physical evidence: low-load reads it as a snapped belt, and the grab path's
 * cv + tol < sv is trivially true at CV=0, so every cut above JAM_GRAB_MIN_LOAD
 * would read as a grab. Neither can tell "the spindle stopped" from "this run
 * never produced a speed reading", and the second is not a reason to
 * emergency-stop a machine that is cutting. A belt that breaks after the
 * spindle is up still trips: CV will have been valid by then. */
#define JAM_LOW_LOAD_DEBOUNCE       18      // (d) consecutive low-load samples (~900 ms at 20 Hz)

/* (c) qualifiers for the step detector.
 *
 * A step up in raw KR on its own is not a jam — it is what an interrupted cut
 * does on every tooth. A 17 mm bi-metal hole saw in aluminium tripped the bare
 * step test twice on 2026-10-13 at a step threshold of 27, with the spindle
 * holding its commanded speed throughout and CL at 100%: the detector was
 * reporting the tool working, not the tool grabbing.
 *
 * What distinguishes a grab is that the spindle starts LOSING: the controller
 * can no longer hold the commanded speed. So a step now has to come with
 * either a speed droop or a load already near the controller's ceiling, and it
 * has to still be true on the next sample.
 *
 * The regulation band itself (JAM_DROOP_PCT / JAM_DROOP_MIN_RPM and
 * jam_spindle_out_of_regulation()) lives in jam.h: motor_load.c's at-target
 * gate has to arm on exactly the band this trips on. It is deliberately looser
 * than motor_load's ±5% BASELINE window, which answers a different question -
 * when the load reading is trustworthy, not when the spindle is in trouble. */
#define JAM_GRAB_MIN_LOAD           25      // (c) droop below this load is not a grab
#define JAM_STEP_ABS_FLOOR          80      // (c) a step landing here trips on its own
#define JAM_STEP_CONFIRM             2      // (c) consecutive qualified samples (~100 ms)

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
static bool    cv_seen_valid = false;       // CV has read >= JAM_CV_VALID_RPM this run
static uint8_t step_confirm = 0;            // (c) consecutive qualified samples
static bool    step_pending = false;        // (c) a step was seen, awaiting confirmation
static uint8_t step_from = 0;               // (c) raw level the step jumped from

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
    cv_seen_valid = false;
    step_confirm = 0;
    step_pending = false;
    step_from = 0;
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
    cv_seen_valid = false;
    step_confirm = 0;
    step_pending = false;
    step_from = 0;
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
    cv_seen_valid = false;
    /* Robustness: the in-grace branch of jam_load_update() clears these on the
     * first poll of any new run, so they cannot actually latch across a stop.
     * Clearing them here anyway means that invariant does not have to hold for
     * this to be correct. */
    step_pending = false;
    step_confirm = 0;
    step_from = 0;

    // Only clear jam if acknowledged or no jam
    if (jam_status.acknowledged || jam_status.type == JAM_NONE) {
        jam_status.type = JAM_NONE;
    }
}

/* A threshold of 0 — or anything else unusable — would collapse the test to
 * `filtered >= 0`, an emergency stop seconds into every drill. The setters
 * clamp, but a stale EEPROM blob can still reach us: refuse it and fall back
 * to the absolute ceiling rather than act on it. */
static uint8_t effective_spike_thresh(uint8_t setting) {
    return (setting >= JAM_SPIKE_MIN_THRESH) ? setting : JAM_LOAD_THRESHOLD;
}

/* The sustained tier sits a fixed margin below the operator's ceiling - but
 * never below a load the machine reaches on its own, or it stops an idle
 * spindle.
 *
 * REVIEW FINDING 2026-10-14: spike_thresh is settable down to 20, and
 * `spike - 15` put the three lowest menu positions at 5/10/14%. Free-run KR on
 * this machine is 15-16% at 750 rpm and 24-25% at 1250, so Spike Thresh 20
 * meant JAM_LOAD_SUSTAINED emergency-stopping a spindle that was touching
 * nothing, 5 s after start.
 *
 * No fixed floor fixes that, because free-run load SCALES WITH SPEED - the two
 * figures above are the same machine, same tool, 500 rpm apart. The number
 * that does track it is the one the machine already measures: the learned idle
 * baseline. So the baseline becomes a FLOOR on this threshold.
 *
 * What keeps this from reintroducing the problem c1e624e removed: that one was
 * `min(baseline + delta, setting)`, which let the baseline drag the trip point
 * DOWN and made Spike Thresh unable to relax anything - no setting existed
 * that let a hole saw finish a cut. This is a max(): the baseline can only ever
 * push the threshold UP, away from a trip. It cannot cause a stop, only
 * prevent one.
 *
 * The result may legitimately end up ABOVE the spike ceiling, and it is not
 * clamped to it. Clamping was the first thing tried and it was wrong: the
 * sustained tier is gated on jam_detect alone, so with Spike Detect off it is
 * the only tier left, and clamping put its threshold back below free-run -
 * the idle spindle stopped again. An "inverted" pair is not a problem: each
 * tier is independently meaningful, and where spike is the lower of the two
 * and enabled, it simply fires first.
 *
 * While the baseline is still learning there is no measurement, so fall back
 * to the factory relationship (JAM_LOAD_THRESHOLD - margin = 75) rather than
 * to the operator's possibly-nonsensical low setting. Conservative here means
 * HIGHER. */
#define JAM_SUSTAINED_BASELINE_MARGIN 20

static uint8_t effective_sustained_thresh(uint8_t eff_spike, bool *from_baseline) {
    uint16_t from_setting = (eff_spike > JAM_SUSTAINED_MARGIN_PCT)
                              ? (uint16_t)(eff_spike - JAM_SUSTAINED_MARGIN_PCT)
                              : eff_spike;

    uint8_t baseline = 0;
    uint16_t floor_pct = (uint16_t)(JAM_LOAD_THRESHOLD - JAM_SUSTAINED_MARGIN_PCT);
    if (motor_load_get_baseline(&baseline)) {
        floor_pct = (uint16_t)baseline + JAM_SUSTAINED_BASELINE_MARGIN;
    }

    const bool floored = (floor_pct > from_setting);
    uint16_t eff = floored ? floor_pct : from_setting;
    if (eff > 100) eff = 100;
    if (from_baseline) *from_baseline = floored;
    return (uint8_t)eff;
}

bool jam_cv_feedback_seen(void) {
    return cv_seen_valid;
}

void jam_get_load_config(uint8_t spike_threshold, jam_load_config_t *out) {
    if (!out) return;
    out->spike_thresh     = effective_spike_thresh(spike_threshold);
    out->sustained_thresh = effective_sustained_thresh(out->spike_thresh,
                                                       &out->sustained_from_baseline);
    out->sustained_ms     = JAM_LOAD_TIMEOUT_MS;
    out->droop_pct        = JAM_DROOP_PCT;
    out->droop_min_rpm    = JAM_DROOP_MIN_RPM;
    out->grab_min_load    = JAM_GRAB_MIN_LOAD;
    out->step_abs_floor   = JAM_STEP_ABS_FLOOR;
    out->step_confirm     = JAM_STEP_CONFIRM;
}

static void trigger_jam(jam_type_t type, uint16_t duration) {
    /* Hold the load trace for readout before anything else overwrites it —
     * this is the only record of what the detectors actually saw. */
    motor_load_trace_freeze();

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

    /* Consume the load filter/grace state from motor_load.
     *
     * The learned baseline is deliberately NOT read here any more — see the
     * absolute-threshold comment below. It still drives the display's cutting
     * bar and motor_load's own stability logic. */
    uint8_t filtered = motor_load_get_filtered();
    uint8_t raw = motor_load_get_raw();
    int8_t step_delta = motor_load_get_step_delta();
    bool in_inrush = motor_load_in_spike_grace();

    // (d) Low-load detector (OEM-parity) — independent of jam_detect_enabled
    // because belt break / tool detach is a different failure class. Inrush
    // grace suppresses it so spin-up doesn't trip it (CV briefly 0).
    /* One CV reading for every detector below. motor_load holds the value
     * task_motor published into g_state.current_rpm moments earlier (it is
     * handed to motor_load_update() immediately before jam_update()), so this
     * is the same number the low-load path used to read straight out of
     * g_state - one source now, and the one the tests can drive. */
    uint16_t cv_now = 0, sv_now = 0;
    motor_load_get_speeds(&cv_now, &sv_now);
    if (cv_now >= JAM_CV_VALID_RPM) cv_seen_valid = true;

    if (low_load_detect_enabled && !in_inrush && cv_seen_valid) {
        const uint16_t cv = cv_now;
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

    /* (c) Grab detection. Two ways in, both needing JAM_STEP_CONFIRM
     * consecutive samples (~100 ms at the 50 ms running poll), both gated by
     * inrush grace and both switched off by step_threshold == 0.
     *
     *   1. DROOP — the spindle is below the speed it was commanded while
     *      pulling real load. At CL=100% a controller that cannot hold its
     *      setpoint is a controller losing to the workpiece, and that is what
     *      a jam IS. This does not care how the load got there.
     *
     *   2. SLAM — a raw step up that LANDS at or above JAM_STEP_ABS_FLOOR.
     *      Faster than waiting for the speed to sag, for the violent case.
     *
     * What this replaces: a bare `step_delta >= step_threshold` with no
     * qualification at all, which stopped this machine twice on 2026-10-13
     * drilling aluminium with a 17 mm bi-metal hole saw — the teeth engaging
     * stepped raw KR past the threshold while the spindle held its commanded
     * speed and the MCB sat at CL=100%. An interrupted cut steps the load by
     * design; that is not news about a jam.
     *
     * Note the two conditions are NOT required in the same sample. Load rises
     * first and the speed sags after it — spindle inertia — so an AND of step
     * and droop within one 50 ms poll would almost never be true, and the
     * detector would quietly never fire.
     *
     * The dropout guard (prev > 0 && raw > 0) is the OEM's, from
     * MOTOR_PROTOCOL.md § Jam detector branch B: a single KR=0 from the MCB
     * mid-cut otherwise reads as a full-load step on the sample after it. */
    if (spike_detect_enabled && step_threshold >= JAM_STEP_MIN_THRESH && !in_inrush) {
        /* Same gate, and here it matters more: with CV still at 0 during
         * spin-up, cv + tol < sv is trivially true, so every cut that reached
         * JAM_GRAB_MIN_LOAD would read as a grab. The raw-step path below does
         * not use CV and still covers this window.
         *
         * motor_load_at_target_seen() is the second, stronger gate: the
         * spindle must have REACHED the commanded speed at least once since
         * the last start or speed change. "CV is below SV" only means a grab
         * if the spindle had the speed and lost it; during any ramp it is just
         * a ramp.
         *
         * This cost a real emergency stop on 2026-10-14 (flight recorder,
         * tick 83791). The operator swept the dial 1250 -> 1050 -> 1450 while
         * drilling. Inrush grace after the step-up is max(delta, 500) = 500 ms,
         * the grace flag went clear 5 polls later, and the newest CV reading
         * available was 1225 - taken before the step, and CV only refreshes
         * every ~6 polls. 1225 + 145 < 1450 with KR at 29%: two samples, trip.
         * The spindle was accelerating perfectly normally the whole time.
         *
         * The same hole was one point of load away from stopping the spin-up
         * that started that very run: at 1250 rpm commanded, grace expires
         * after 1250 ms, and CV was still 137 rpm short of target at 1113 with
         * KR at 24%. JAM_GRAB_MIN_LOAD is 25. That is the "it tripped as I
         * started drilling" case, and no threshold tuning fixes it - the
         * comparison itself was being made at a time when it has no meaning.
         *
         * Why a gate and not a longer grace: grace is a guess at how long the
         * ramp takes. Measured here, a 1250 rpm start needs ~2.6 s to settle
         * while grace allows 1250 ms, and the right number depends on the
         * MCB's ramp setting, the load and the inertia of whatever is in the
         * chuck. Reaching the setpoint is not a guess. */
        const bool drooping = cv_seen_valid &&
                              motor_load_at_target_seen() &&
                              jam_spindle_out_of_regulation(cv_now, sv_now) &&
                              (raw >= JAM_GRAB_MIN_LOAD);

        const bool step_seen = (raw > 0) && (motor_load_get_raw_prev() > 0) &&
                               (step_delta > 0) &&
                               ((uint8_t)step_delta >= step_threshold) &&
                               (raw >= JAM_STEP_ABS_FLOOR);

        /* A slam has to STAY slammed. A load that collapses on the next sample
         * is the bit breaking through, or a dropped reading — cancel, don't
         * confirm.
         *
         * "Still slammed" has to mean the same thing the slam itself meant, so
         * it tests BOTH halves of step_seen's bar: the step height AND the
         * absolute floor. Testing only `step_from + step_threshold` (review
         * finding, 2026-10-14) let a low Step Thresh confirm on a wobble -
         * at StepTh=5, a 50 -> 82 slam was confirmed by any sample >= 55,
         * which is most of the way back down to free-run and nowhere near the
         * loaded state that justified the trip. */
        const bool still_up = step_pending &&
                              (raw >= (uint16_t)step_from + step_threshold) &&
                              (raw >= JAM_STEP_ABS_FLOOR);

        if (step_seen) {
            step_pending = true;
            step_from = motor_load_get_raw_prev();
            step_confirm = 1;
        } else if (drooping || still_up) {
            if (++step_confirm >= JAM_STEP_CONFIRM) {
                trigger_jam(JAM_LOAD_STEP, 0);
                step_pending = false;
                step_confirm = 0;
                load_jam_condition_active = false;
                return true;
            }
        } else {
            step_pending = false;
            step_confirm = 0;
        }
    } else {
        step_pending = false;
        step_confirm = 0;
    }

    /* (a)+(b) Absolute tiers. The operator's threshold is the authority.
     *
     * These used to be min(baseline + fixed delta, user setting), gated on the
     * baseline having armed. Three things were wrong with that:
     *
     *  - the min() made Spike Thresh incapable of relaxing anything. With a
     *    free-run baseline of ~11% the spike tier tripped at ~36% filtered
     *    load whatever the setting said, so the 90% this machine was
     *    configured with had no effect at all, and NO setting existed that
     *    would let a hole saw or a large twist drill finish a cut;
     *  - a fixed delta above a free-run baseline cannot tell tools apart. A
     *    17 mm bi-metal hole saw legitimately runs tens of points above
     *    free-run; a 3 mm twist drill barely moves. The delta was really
     *    asking "is it cutting?", not "is it jammed". Note that the OEM has no
     *    equivalent of this tier at all — its only load-jam detector is the
     *    raw step delta (c), so dropping it does not drop below OEM
     *    protection;
     *  - gating the ABSOLUTE ceiling on baseline_armed was a hole. The
     *    baseline only arms after CV sits within ±5% of SV for the whole
     *    stability window, so feeding the bit during spin-up — or any run
     *    where the speed never settles — left the ceiling disabled for the
     *    entire cut.
     *
     * So: the ceiling is the operator's number, it applies whether or not the
     * baseline has armed, and inrush grace is what keeps the EMA's spin-up
     * climb (observed at 91% filtered on a 2000->5000 step) from tripping it.
     *
     * The sustained tier sits a fixed margin below the ceiling: a brief
     * excursion to just under the limit is normal cutting, the same load held
     * for JAM_LOAD_TIMEOUT_MS is not. */
    uint8_t spike_thresh_eff = effective_spike_thresh(spike_threshold);
    uint8_t sustained_thresh = effective_sustained_thresh(spike_thresh_eff, NULL);

    if (spike_detect_enabled && !in_inrush && filtered >= spike_thresh_eff) {
        trigger_jam(JAM_LOAD_SPIKE, 0);
        load_jam_condition_active = false;
        return true;
    }

    TickType_t now = xTaskGetTickCount();
    if (!in_inrush && filtered >= sustained_thresh) {
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
