#include "logging.h"
#include <string.h>
/**
 * @file task_tapping.c
 * @brief Tapping Trigger System - Combinable Triggers
 *
 * NEW ARCHITECTURE: Combinable triggers (can enable multiple simultaneously)
 *
 * AUTOMATIC TRIGGERS (combinable):
 * - Depth: Stop/reverse at target depth
 * - Load Increase: KR spike detection (blind holes, excessive resistance)
 * - Load Slip: CV overshoot detection (through-hole exit)
 * - Clutch Slip: Load plateau detection (torque limiter engaged)
 * - Quill: Auto-reverse based on quill direction (manual feed sensing)
 * - Peck: Timed forward/reverse cycles
 *
 * MANUAL TRIGGERS:
 * - Pedal: Manual chip break or hold reversal
 *
 * LEGACY: Still uses mode-based switch for compatibility during transition.
// Trigger-based logic IMPLEMENTED - parallel monitoring, priority resolution
 */

#include "shared.h"
#include "config.h"
#include "tapping.h"
#include "settings.h"
#include "motor.h"
#include "serial_console.h"
#include <stdio.h>

/*===========================================================================*/
/* Constants                                                                  */
/*===========================================================================*/

// Use config.h constant: TAP_DEPTH_DEADBAND_MM (2.0mm = 20 in 0.1mm units)
#define TAP_DEADBAND_0_1MM      TAP_DEPTH_DEADBAND_MM
#define TAP_POLL_MS             50      // State machine update interval
#define TAP_LOAD_SAMPLE_MS      50      // Load sampling interval
#define THROUGH_HOLE_DEBOUNCE   3       // N consecutive low-load readings
#define TAP_TRANSITION_TIMEOUT_MS  1000 // SAFETY: Max time in TRANSITION state (1s)

/*===========================================================================*/
/* Private Variables                                                          */
/*===========================================================================*/

static int16_t tap_prev_depth = 0;
static uint8_t tap_internal_state = TAP_STATE_IDLE;
static TickType_t tap_transition_start = 0;
static TickType_t tap_phase_start = 0;          // For peck timing
static uint8_t tap_peck_cycle = 0;              // Current peck cycle
static uint8_t tap_load_baseline = 0;           // Learned baseline load (KR)
static uint8_t tap_low_load_count = 0;          // Through-hole debounce counter
static uint32_t tap_fwd_time_ms = 0;            // Peck forward time
static uint32_t tap_rev_time_ms = 0;            // Peck reverse time
static uint16_t tap_brake_delay_ms = 100;       // Cached brake delay from settings

// CV overshoot detection (discovered 2026-01-25 via logic analyzer)
// Through-hole detection uses CV overshoot, not just KR threshold
static uint16_t tap_cv_baseline = 0;            // Learned baseline CV (target speed)
static TickType_t tap_baseline_start = 0;       // When baseline learning started
static bool tap_baseline_learned = false;       // True after ~4s stable baseline
static uint8_t tap_cv_overshoot_count = 0;      // Debounce counter for CV overshoot
static bool tap_was_forward = true;             // Local direction tracker (motor task clobbers g_state.motor_forward on stop)

// AUDIT FIX (HIGH, task_tapping.c:285): tracking for bounded completion reverse.
// REVERSE_OUT and REVERSE_TIMED used to have no termination — REVERSE_OUT
// looped because complete_transition kept re-issuing TAP_REVERSE, and
// REVERSE_TIMED re-fired every 50 ms poll because it had no timer.
// Both now record their start tick and are terminated by the REVERSING state.
#define TAP_REVERSE_OUT_MAX_MS   60000   // safety cap in case at-top is never reached
static bool tap_in_completion_reverse = false;
static uint8_t tap_completion_kind = 0;         // completion_action_t
static TickType_t tap_completion_start = 0;
/* REVERSE_TIMED duration for the completion in progress. Was hardcoded to
 * peck_reverse_out_ms, which was correct only while peck was the one trigger
 * with a completion action. */
static uint32_t tap_completion_timed_ms = 0;

// Clutch slip detection (torque limiter)
static uint8_t tap_clutch_prev_load = 0;        // Previous load reading
/* How long the CURRENT trigger reverse should run before resuming forward.
 * 0 = open-ended, i.e. the historical behaviour where some other trigger has to
 * end it.
 *
 * This is the whole fix for three settings that did nothing. There are two ways
 * out of CUTTING: execute_completion(), which records a kind and a start so
 * REVERSING knows exactly how to finish, and execute_trigger_reverse(), which
 * recorded NOTHING but a reason string. So a reverse triggered by load-increase,
 * load-slip or clutch had no exit at all unless peck, quill or pedal happened to
 * be enabled — it ran to the 30 s TAP_MAX_CYCLE_TIME_MS backstop. That is why
 * load_increase_reverse_ms and pedal_chip_break_ms could not be honoured: the
 * reverse did not know which trigger caused it. Give it a duration and all three
 * settings become implementable. */
static uint32_t tap_reverse_duration_ms = 0;

static TickType_t tap_clutch_plateau_start = 0; // When plateau detected
/* Latch so a plateau lasting seconds does not repaint the ALERT banner on
 * every 50 ms poll. File scope, not a function static, so it is cleared
 * alongside the other clutch state on abort and disarm. */
static bool tap_clutch_alert_shown = false;
/* Previous pedal level, for CHIP_BREAK's press edge. */
static bool tap_pedal_prev_pressed = false;
/* Consecutive RESUME re-fires of one automatic trigger — see
 * TAP_RESUME_ESCALATE_COUNT. tap_resume_trigger is compared by pointer: every
 * active_trigger is a string literal from this file, so identical triggers
 * share an address. A false mismatch would only reset the counter, which is
 * the fail-safe direction. */
static const char* tap_resume_trigger = NULL;
static uint8_t tap_resume_count = 0;
static TickType_t tap_resume_last = 0;
static bool tap_clutch_plateau_active = false;  // True when load plateaued

#define TAP_BASELINE_LEARN_MS        4000       // 4 seconds to learn baseline (matches original)
#define CLUTCH_LOAD_MIN              30         // Minimum load to consider plateau (30%)
#define CLUTCH_LOAD_DELTA_MAX        5          // Max load change during plateau (5%)
#define DEPTH_AT_TOP_MM              10         // Depth threshold for "at top" position (1.0mm)
#define PECK_INTER_CYCLE_DELAY_MS    200        // Delay between PECK cycles (motor recovery)
#define LOAD_BASELINE_EMA_ALPHA      8          // EMA filter: new_baseline = (old*7 + new)/8

/*===========================================================================*/
/* Helper Functions                                                           */
/*===========================================================================*/

/**
 * @brief Stop motor for tapping (fast stop, no post-sync overhead)
 */
static void tap_motor_stop(void) {
    /* REVIEW FIX (MEDIUM): this was the plain MOTOR_CMD, which blocks 100 ms
     * on a full queue, then only bumps a counter and posts EVT_MOTOR_FAULT —
     * the caller gets no return value. task_depth.c:312 documents that exact
     * hazard and converted its four sites; task_tapping was left behind at
     * every stop, including the guard/E-Stop abort, which then set
     * APP_STATE_IDLE and printed a clean abort while only the EXTI PD4 drop
     * had actually stopped the spindle — an unbraked coast reported as a
     * braked stop.
     *
     * The repeated-emergency-stop hazard that made task_depth latch its sites
     * does not apply here: every caller sets tap_internal_state immediately
     * after, so each stop is issued once per cycle. No caller holds
     * MOTOR_CONTROL_LOCK any more: task_tapping does not take it at all (see
     * the long note in the poll loop). That is safe because this is a queue
     * send, and because motor_emergency_stop() on the queue-full fallback
     * takes no mutex either. */
    MOTOR_CMD_SEND_CRITICAL(CMD_MOTOR_STOP_FAST, 0);
}

/**
 * @brief Load peck timing from settings (direct millisecond values)
 */
static void calc_peck_timing(void) {
    const settings_t* s = settings_get();

    tap_fwd_time_ms = s->tapping.peck_fwd_ms;
    tap_rev_time_ms = s->tapping.peck_rev_ms;

    if (tap_fwd_time_ms < 50) tap_fwd_time_ms = 50;
    if (tap_rev_time_ms < 50) tap_rev_time_ms = 50;

    char buf[48];
    uint32_t ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snprintf(buf, sizeof(buf), "[%5lu.%02lu] PECK FWD=%lums REV=%lums\r\n",
             ms / 1000, (ms % 1000) / 10, tap_fwd_time_ms, tap_rev_time_ms);
    uart_puts(buf);
}

/*===========================================================================*/
/* Trigger State Structure                                                   */
/*===========================================================================*/

typedef struct {
    const char* name;           // Trigger name for logging
    bool wants_reverse;         // Trigger wants to reverse (from cutting)
    bool wants_forward;         // Trigger wants to forward (from reversing)
    bool wants_complete;        // Trigger wants to complete cycle
    uint8_t priority;           // Priority level (higher = more important)
} trigger_result_t;

/*===========================================================================*/
/* Trigger Detection Functions (Pure - No Side Effects)                      */
/*===========================================================================*/

/**
 * @brief Check if pedal wants to trigger action
 */
static bool check_pedal_wants_action(bool pedal_pressed, bool pedal_edge, bool in_cutting,
                                     pedal_action_t action) {
    if (action == PEDAL_ACTION_HOLD) {
        return (in_cutting && pedal_pressed);  // Trigger reverse when pressed in cutting
    } else {  // PEDAL_ACTION_CHIP_BREAK
        /* Entry is identical; the two modes differ in how the reverse ENDS.
         * HOLD is open-ended and finishes on release-at-top; CHIP_BREAK gets
         * pedal_chip_break_ms handed to execute_trigger_reverse() and resumes
         * cutting by itself. That distinction lives at the dispatch below.
         *
         * CHIP_BREAK is EDGE triggered, and must be: its reverse ends by
         * itself after pedal_chip_break_ms, with the operator's foot still
         * down. On a level test the very next CUTTING poll would fire another
         * chip break, giving one 50 ms forward blip per reverse for as long as
         * the pedal is held — the opposite of "break the chip and carry on".
         * One press, one chip break; press again for another. */
        return (in_cutting && pedal_edge);
    }
}

/**
 * @brief Check if depth trigger wants reversal
 */
static bool check_depth_wants_reverse(int16_t current_depth, int16_t target_depth, bool in_cutting) {
    return in_cutting && target_depth > 0 && current_depth >= target_depth;
}

/**
 * @brief Check if quill lift wants reversal
 */
static bool check_quill_lift_wants_reverse(int16_t current_depth, bool in_cutting) {
    if (!in_cutting) return false;
    int16_t depth_delta = current_depth - tap_prev_depth;
    const int16_t TRIGGER_UP = -(TAP_DEADBAND_0_1MM + TAP_HYSTERESIS_MM);
    return depth_delta < TRIGGER_UP;
}

/**
 * @brief Check if quill push wants forward (when reversing)
 */
static bool check_quill_push_wants_forward(int16_t current_depth, bool in_reversing) {
    if (!in_reversing) return false;
    int16_t depth_delta = current_depth - tap_prev_depth;
    const int16_t TRIGGER_DOWN = TAP_DEADBAND_0_1MM + TAP_HYSTERESIS_MM;
    return depth_delta > TRIGGER_DOWN;
}

/**
 * @brief Check if load increase wants reversal (KR spike)
 */
static bool check_load_increase_wants_reverse(uint8_t motor_load, uint8_t threshold, bool in_cutting) {
    if (!in_cutting || !tap_baseline_learned) return false;
    return motor_load > tap_load_baseline + threshold;
}

/**
 * @brief Check if load slip wants reversal (CV overshoot - through-hole)
 * PURE: No state modification - returns detection result only
 */
static bool check_load_slip_wants_reverse(uint16_t cv_percent, bool in_cutting, int16_t current_depth) {
    if (!in_cutting || !tap_baseline_learned || current_depth < 50) return false;

    uint16_t actual_cv = motor_get_actual_rpm();
    uint16_t overshoot_threshold = (tap_cv_baseline * cv_percent) / 100;

    // Pure detection - caller handles debounce state update
    return actual_cv > overshoot_threshold;
}

/**
 * @brief Check if clutch slip wants reversal (load plateau)
 * PURE: Returns detection result, caller updates state
 */
static bool check_clutch_wants_reverse(uint8_t motor_load, uint16_t plateau_ms, bool in_cutting) {
    if (!in_cutting) return false;
    if (motor_load < CLUTCH_LOAD_MIN) return false;

    // Check load change (plateau detection)
    uint8_t load_delta = (motor_load > tap_clutch_prev_load) ?
                         (motor_load - tap_clutch_prev_load) :
                         (tap_clutch_prev_load - motor_load);

    // If load changed significantly, not a plateau
    if (load_delta > CLUTCH_LOAD_DELTA_MAX) return false;

    // Check if plateau active and duration exceeded
    if (tap_clutch_plateau_active) {
        uint32_t duration = (xTaskGetTickCount() - tap_clutch_plateau_start) * portTICK_PERIOD_MS;
        return duration >= plateau_ms;
    }

    // Plateau starting (caller will update state)
    return false;
}

/*===========================================================================*/
/* Unified Action Execution Functions                                         */
/*===========================================================================*/

/**
 * @brief Emit one whole tapping-transition line.
 *
 * Every one of these five sites used to be written as a conditional
 * LOG_TAP_DEBUG() for the fixed part followed by BARE uart_puts() calls for the
 * variable part. The macro compiles to nothing unless LOG_LEVEL >= LOG_DEBUG;
 * the uart_puts() calls after it are unconditional. So on any release build
 * (LOG_LEVEL=1) the prefix vanished and the remainder still printed, giving
 * half-lines on the console: a bare "PEDAL", or "PECK): REVERSE_OUT to top"
 * with no idea what was complete. One line, one rule, in one place.
 *
 * Deliberately NOT gated behind LOG_TAP_DEBUG. These fire a few times per hole,
 * never in a hot path, and "why did the spindle just reverse" is exactly the
 * question a release build has to be able to answer at the machine. Gating them
 * would have made release silent, which is a loss of observability, not a fix.
 */
/* Instrumentation for the ~1.4 s that a "200 ms" chip break actually took.
 * Prints a measured millisecond value so the overhead can be attributed to a
 * phase instead of guessed at. Kept behind BUILD_DEBUG: it is diagnostic
 * scaffolding, not something a release console should be emitting. */
#ifdef BUILD_DEBUG
static void tap_log_ms(const char* what, uint32_t ms) {
    extern void uart_puts(const char* s);
    extern void print_num(int32_t n);
    uart_puts("[TAP] ");
    uart_puts(what);
    print_num((int32_t)ms);
    uart_puts(" ms\r\n");
}
#define TAP_LOG_MS(what, ms) tap_log_ms((what), (ms))
#else
#define TAP_LOG_MS(what, ms) ((void)0)
#endif

static void tap_log(const char* what, const char* detail, const char* tail) {
    extern void uart_puts(const char* s);
    uart_puts("[TAP] ");
    uart_puts(what);
    if (detail) uart_puts(detail);
    if (tail)   uart_puts(tail);
    uart_puts("\r\n");
}

/**
 * @brief Execute transition to reversing state
 */
static void execute_trigger_reverse(const char* reason, uint32_t duration_ms) {
    if (tap_internal_state != TAP_STATE_CUTTING) return;

    /* 0 = open-ended (quill/peck/pedal-HOLD end it); non-zero = resume forward
     * after this long. Recorded here because REVERSING cannot otherwise know
     * which trigger it is serving. */
    tap_reverse_duration_ms = duration_ms;

    tap_log("Reversing: ", reason, NULL);

    tap_was_forward = true;
    tap_motor_stop();
    tap_internal_state = TAP_STATE_TRANSITION;
    tap_transition_start = xTaskGetTickCount();
    STATE_LOCK();
    g_state.tap_state = TAP_STATE_TRANSITION;
    STATE_UNLOCK();
}

/**
 * @brief Execute transition to forward state
 */
static void execute_trigger_forward(const char* reason) {
    if (tap_internal_state != TAP_STATE_REVERSING) return;

    tap_log("Forward: ", reason, NULL);

    tap_was_forward = false;
    tap_reverse_duration_ms = 0;   /* this reverse is over; do not leak it into the next */
    tap_motor_stop();
    tap_internal_state = TAP_STATE_TRANSITION;
    tap_transition_start = xTaskGetTickCount();
}

/**
 * @brief Execute completion action (stop or reverse out)
 */
static void execute_completion(completion_action_t action, const char* trigger_name,
                               uint32_t timed_ms) {
    extern void uart_puts(const char* s);

    /* A completion reverse is terminated by its own kind (at-top or
     * peck_reverse_out_ms), never by a trigger duration left over from an
     * earlier load-increase or chip-break reverse in the same run. */
    tap_reverse_duration_ms = 0;
    tap_completion_timed_ms = timed_ms;
    extern void print_num(int32_t n);

    switch (action) {
        case COMPLETION_STOP:
            tap_log("Complete (", trigger_name, "): STOP");
            tap_motor_stop();
            tap_internal_state = TAP_STATE_IDLE;
            STATE_LOCK();
            g_state.tap_state = TAP_STATE_IDLE;
            g_state.motor_running = false;
            g_state.state = APP_STATE_IDLE;
            STATE_UNLOCK();
            break;

        /* Unreachable by construction: the trigger chain routes RESUME to
         * execute_trigger_reverse() and never gets here. Handled anyway, and
         * deliberately as REVERSE_OUT rather than as a no-op — if a future
         * caller does reach it, backing out of the hole is the safe way to be
         * wrong, whereas returning without acting would leave the trigger true
         * and re-enter this on every 50 ms poll while the cut continued. */
        case COMPLETION_RESUME:
        case COMPLETION_REVERSE_OUT:
            /* Own the phase timer as well as the completion timer — the peck
             * block above measures against tap_phase_start. */
            tap_phase_start = xTaskGetTickCount();
            tap_log("Complete (", trigger_name, "): REVERSE_OUT to top");
            // AUDIT FIX (HIGH, task_tapping.c:285): mark this as a completion
            // reverse so the REVERSING state terminates on at-top rather than
            // looping back through complete_transition. Also actually start
            // the reverse — the old code sent STOP and hoped complete_transition
            // would kick off the reverse from tap_was_forward=true, but that
            // created the endless reverse cycle.
            MOTOR_CMD(CMD_MOTOR_TAP_REVERSE, 0);
            tap_internal_state = TAP_STATE_REVERSING;
            tap_in_completion_reverse = true;
            tap_completion_kind = COMPLETION_REVERSE_OUT;
            tap_completion_start = xTaskGetTickCount();
            STATE_LOCK();
            /* Publish the phase too, not just the direction. Both completion
             * branches set tap_internal_state = TAP_STATE_REVERSING but left
             * g_state.tap_state at CUTTING, so every consumer of the published
             * state — the LCD's tapping phase, and the TAP console readout —
             * showed CUTTING while the spindle was reversing out of the hole.
             * Confirmed on target: a quill/depth completion reverse reported
             * "TapState: 1" throughout, where a trigger reverse through
             * complete_transition() correctly reported 2. */
            g_state.tap_state = TAP_STATE_REVERSING;
            g_state.motor_forward = false;
            STATE_UNLOCK();
            break;

        case COMPLETION_REVERSE_TIMED:
            tap_phase_start = xTaskGetTickCount();
            tap_log("Complete (", trigger_name, "): REVERSE_TIMED");
            // Start timed reverse
            MOTOR_CMD(CMD_MOTOR_TAP_REVERSE, 0);
            tap_internal_state = TAP_STATE_REVERSING;
            tap_transition_start = xTaskGetTickCount();
            // AUDIT FIX (HIGH, task_tapping.c:285): record the completion start
            // so the REVERSING state can terminate after peck_reverse_out_ms —
            // otherwise this branch re-fires on every 50 ms poll and spams
            // CMD_MOTOR_TAP_REVERSE into the queue forever.
            tap_in_completion_reverse = true;
            tap_completion_kind = COMPLETION_REVERSE_TIMED;
            tap_completion_start = xTaskGetTickCount();
            STATE_LOCK();
            /* Publish the phase too, not just the direction. Both completion
             * branches set tap_internal_state = TAP_STATE_REVERSING but left
             * g_state.tap_state at CUTTING, so every consumer of the published
             * state — the LCD's tapping phase, and the TAP console readout —
             * showed CUTTING while the spindle was reversing out of the hole.
             * Confirmed on target: a quill/depth completion reverse reported
             * "TapState: 1" throughout, where a trigger reverse through
             * complete_transition() correctly reported 2. */
            g_state.tap_state = TAP_STATE_REVERSING;
            g_state.motor_forward = false;
            STATE_UNLOCK();
            break;
    }
}

/**
 * @brief Complete transition after brake delay
 */
static void complete_transition(bool was_forward, int16_t depth) {
    if (tap_internal_state != TAP_STATE_TRANSITION) return;
    if ((xTaskGetTickCount() - tap_transition_start) < pdMS_TO_TICKS(tap_brake_delay_ms)) return;

    if (was_forward) {
        // Was cutting, now reverse
        MOTOR_CMD(CMD_MOTOR_TAP_REVERSE, 0);
        tap_internal_state = TAP_STATE_REVERSING;
        TAP_LOG_MS("TRANSITION->REVERSING took ",
                   (uint32_t)(xTaskGetTickCount() - tap_transition_start) * portTICK_PERIOD_MS);
        tap_phase_start = xTaskGetTickCount();  // Reset timer for reverse duration
        STATE_LOCK();
        g_state.tap_state = TAP_STATE_REVERSING;
        g_state.motor_forward = false;
        STATE_UNLOCK();
    } else {
        // Was reversing, now forward
        MOTOR_CMD(CMD_MOTOR_TAP_FORWARD, 0);
        tap_internal_state = TAP_STATE_CUTTING;
        TAP_LOG_MS("TRANSITION->CUTTING took ",
                   (uint32_t)(xTaskGetTickCount() - tap_transition_start) * portTICK_PERIOD_MS);
        tap_phase_start = xTaskGetTickCount();  // Reset timer for forward duration
        STATE_LOCK();
        g_state.tap_state = TAP_STATE_CUTTING;
        g_state.motor_forward = true;
        STATE_UNLOCK();
    }
    tap_prev_depth = depth;  // Update reference
}

/*===========================================================================*/
/* Task Entry Point                                                           */
/*===========================================================================*/

void task_tapping(void *pvParameters) {
    (void)pvParameters;

    for (;;) {
        // CRITICAL SAFETY: Update task heartbeat for watchdog monitoring
        HEARTBEAT_UPDATE_TAPPING();

        // Get current mode and state
        STATE_LOCK();
        // Mode removed - using triggers;
        app_state_t app_state = g_state.state;
        // Use simulated depth if in sim mode
        int16_t current_depth = g_state.sim_mode ? g_state.sim_depth : g_state.current_depth;
        int16_t target_depth = g_state.target_depth;
        bool pedal_pressed = g_state.pedal_pressed;
        bool motor_forward = g_state.motor_forward;
        bool motor_running = g_state.motor_running;
        uint8_t motor_load = g_state.motor_load;
        bool guard_closed = g_state.guard_closed;  // SAFETY: Check guard status
        // AUDIT FIX (CRITICAL, task_tapping.c:319): also snapshot estop_active
        // so we can abort mid-cycle. Without this the tapping task can enqueue
        // TAP_FORWARD/REVERSE from a stale app_state read after handle_btn_estop
        // has already purged the motor queue, restarting the spindle under
        // engaged E-Stop.
        bool estop_active = g_state.estop_active;
        STATE_UNLOCK();

        // CRITICAL SAFETY: Abort tapping if guard opens or E-Stop engages.
        if ((!guard_closed || estop_active) &&
            (app_state == APP_STATE_TAPPING || tap_internal_state != TAP_STATE_IDLE)) {
            extern void uart_puts(const char* s);
            LOG_TAP_DEBUG(estop_active ? "E-Stop - aborting tapping cycle\r\n"
                                       : "Guard opened - aborting tapping cycle\r\n");
            tap_motor_stop();
            tap_internal_state = TAP_STATE_IDLE;
            /* REVIEW FIX (HIGH): the clutch plateau state is written only
             * inside `if (clutch_slip_enabled && in_cutting)` and was absent
             * from both this abort and the disarm reset below, which clears
             * seven other variables. A cutting phase that ended plateaued left
             * the flag set, so on re-entry check_clutch_wants_reverse() measured
             * a duration spanning the whole previous phase plus the reverse,
             * blew past clutch_plateau_ms and reversed immediately —
             * CUTTING/REVERSING ping-pong with ~50 ms of real cutting per
             * cycle — and the stale flag survived an abort into the next run. */
            tap_clutch_plateau_active = false;
            tap_clutch_plateau_start = 0;
            tap_clutch_alert_shown = false;
            tap_reverse_duration_ms = 0;
            tap_resume_count = 0;
            tap_resume_trigger = NULL;
            /* Adopt the level rather than clearing it: a pedal still held
             * through an abort must be released and pressed again to count as
             * a new chip-break press. */
            tap_pedal_prev_pressed = pedal_pressed;
            STATE_LOCK();
            if (!estop_active) g_state.state = APP_STATE_IDLE;  // E-Stop path owns ERROR
            g_state.tap_state = TAP_STATE_IDLE;
            STATE_UNLOCK();
            delay_ms(TAP_POLL_MS);
            continue;
        }

        // Check if any triggers are enabled
        const tapping_settings_t* tap_cfg = tapping_get_settings();
        bool any_triggers = tap_cfg->depth_trigger_enabled ||
                           tap_cfg->load_increase_enabled ||
                           tap_cfg->load_slip_enabled ||
                           tap_cfg->clutch_slip_enabled ||
                           tap_cfg->quill_trigger_enabled ||
                           tap_cfg->peck_trigger_enabled ||
                           tap_cfg->pedal_enabled;

        // Only process if triggers enabled, armed, and in tapping state
        STATE_LOCK();
        bool armed = g_state.tapping_armed;
        STATE_UNLOCK();
        if (!any_triggers || !armed || app_state != APP_STATE_TAPPING) {
            // AUDIT FIX (CRITICAL, task_tapping.c:319): if we were mid-cycle
            // (any state past IDLE), stop the motor before resetting internal
            // state — otherwise a disarm mid-run silently leaks a running
            // spindle. The estop/guard abort above already handled the
            // safety-critical exits; this catches the ordinary disarm cases.
            if (tap_internal_state != TAP_STATE_IDLE && motor_running) {
                tap_motor_stop();
            }
            tap_internal_state = TAP_STATE_IDLE;
            tap_prev_depth = current_depth;
            tap_peck_cycle = 0;
            tap_load_baseline = 0;
            tap_low_load_count = 0;
            tap_cv_baseline = 0;
            tap_baseline_learned = false;
            tap_cv_overshoot_count = 0;
            tap_clutch_plateau_active = false;   /* see the abort path above */
            tap_clutch_plateau_start = 0;
            tap_clutch_alert_shown = false;
            tap_reverse_duration_ms = 0;
            tap_resume_count = 0;
            tap_resume_trigger = NULL;
            /* Adopt the level rather than clearing it: a pedal still held
             * through an abort must be released and pressed again to count as
             * a new chip-break press. */
            tap_pedal_prev_pressed = pedal_pressed;
            delay_ms(TAP_POLL_MS);
            continue;
        }

        // Get brake delay from settings (cached for performance)
        {
            const settings_t* s = settings_get();
            tap_brake_delay_ms = s->tapping.brake_delay_ms;
            if (tap_brake_delay_ms < 50) tap_brake_delay_ms = 50;
        }

        // ===================================================================
        // COMBINABLE TRIGGER SYSTEM - TRUE SIMULTANEOUS MONITORING
        // ALL enabled triggers check their conditions each cycle
        // ALL can contribute to reversal decision (OR logic)
        // Priority used only when multiple fire simultaneously
        // ===================================================================

        // Check which triggers are enabled
        bool pedal_enabled = tap_cfg->pedal_enabled;
        bool quill_enabled = tap_cfg->quill_trigger_enabled;
        bool depth_enabled = tap_cfg->depth_trigger_enabled;
        bool load_inc_enabled = tap_cfg->load_increase_enabled;
        bool load_slip_enabled = tap_cfg->load_slip_enabled;
        bool peck_enabled = tap_cfg->peck_trigger_enabled;

        // Legacy mode compatibility removed - triggers set directly via menu/settings

        // ===================================================================
        // TRUE PARALLEL TRIGGER MONITORING
        // Check ALL enabled triggers, use priority only when multiple fire
        // ===================================================================

        // Read state once for all trigger checks
        STATE_LOCK();
        motor_running = g_state.motor_running;
        motor_forward = g_state.motor_forward;
        current_depth = g_state.sim_mode ? g_state.sim_depth : g_state.current_depth;
        target_depth = g_state.target_depth;
        motor_load = g_state.motor_load;
        pedal_pressed = g_state.pedal_pressed;
        uint16_t target_rpm_snapshot = g_state.target_rpm;
        STATE_UNLOCK();

        bool in_cutting = (tap_internal_state == TAP_STATE_CUTTING);
        bool in_reversing = (tap_internal_state == TAP_STATE_REVERSING);

        // Check ALL enabled triggers (parallel detection)
        const bool pedal_edge = (pedal_pressed && !tap_pedal_prev_pressed);
        tap_pedal_prev_pressed = pedal_pressed;

        bool pedal_wants = pedal_enabled &&
                          check_pedal_wants_action(pedal_pressed, pedal_edge, in_cutting,
                                                   tap_cfg->pedal_action);
        bool depth_wants = depth_enabled &&
                          check_depth_wants_reverse(current_depth, target_depth, in_cutting);
        bool quill_lift_wants = quill_enabled &&
                               check_quill_lift_wants_reverse(current_depth, in_cutting);
        bool quill_push_wants = quill_enabled &&
                               check_quill_push_wants_forward(current_depth, in_reversing);
        bool load_inc_wants = load_inc_enabled &&
                             check_load_increase_wants_reverse(motor_load,
                                                              tap_cfg->load_increase_threshold,
                                                              in_cutting);
        bool load_slip_wants = load_slip_enabled &&
                              check_load_slip_wants_reverse(tap_cfg->load_slip_cv_percent,
                                                            in_cutting, current_depth);
        bool clutch_wants = tap_cfg->clutch_slip_enabled &&
                           check_clutch_wants_reverse(motor_load, tap_cfg->clutch_plateau_ms,
                                                     in_cutting);

        // Update detection state (debounce counters, plateau tracking)
        // This separates detection (pure) from state updates (side effects)
        if (load_slip_enabled && in_cutting && tap_baseline_learned) {
            uint16_t actual_cv = motor_get_actual_rpm();
            uint16_t overshoot_threshold = (tap_cv_baseline * tap_cfg->load_slip_cv_percent) / 100;
            if (actual_cv > overshoot_threshold) {
                tap_cv_overshoot_count++;
            } else {
                tap_cv_overshoot_count = 0;
            }
        }

        // AUDIT FIX (MEDIUM, task_tapping.c:194): through-hole trigger was
        // firing on a single overshoot sample even though the debounce counter
        // was being maintained. Gate load_slip_wants on the counter now, so a
        // single noisy motor_get_actual_rpm() spike doesn't reverse the tap
        // mid-thread. THROUGH_HOLE_DEBOUNCE=3 consecutive samples.
        if (load_slip_wants && tap_cv_overshoot_count < THROUGH_HOLE_DEBOUNCE) {
            load_slip_wants = false;
        }

        if (tap_cfg->clutch_slip_enabled && in_cutting) {
            if (motor_load < CLUTCH_LOAD_MIN) {
                tap_clutch_plateau_active = false;
            } else {
                uint8_t load_delta = (motor_load > tap_clutch_prev_load) ?
                                     (motor_load - tap_clutch_prev_load) :
                                     (tap_clutch_prev_load - motor_load);
                if (load_delta > CLUTCH_LOAD_DELTA_MAX) {
                    tap_clutch_plateau_active = false;
                    tap_clutch_plateau_start = xTaskGetTickCount();
                } else if (!tap_clutch_plateau_active) {
                    tap_clutch_plateau_active = true;
                    tap_clutch_plateau_start = xTaskGetTickCount();
                }
                tap_clutch_prev_load = motor_load;
            }
        }

        // Determine active trigger (priority resolution)
        // PRIORITY ORDER (highest to lowest): Pedal > Quill > Depth > Load Inc > Load Slip > Clutch
        // Rationale: Manual override (pedal) > Operator intent (quill) > Safety limits (depth/load)
        /* CLUTCH_ACTION_ALERT: warn and keep cutting, per the enum's own
         * definition ("Show warning, keep running"). clutch_action was stored,
         * packed, synced — and never read, so ALERT performed a full reverse
         * exactly like REVERSE did. Latched so a plateau that persists for
         * seconds does not repaint the warning every 50 ms poll. */
        if (clutch_wants && tap_cfg->clutch_action == CLUTCH_ACTION_ALERT) {
            if (!tap_clutch_alert_shown) {
                tap_clutch_alert_shown = true;
                uart_puts("[TAP] Clutch slip detected - alert only, still cutting\r\n");
                STATE_LOCK();
                g_state.error_until = HAL_GetTick() + TAP_ALERT_DISPLAY_MS;
                g_state.error_line1 = "! CLUTCH SLIP ! ";
                g_state.error_line2 = "Still cutting   ";
                STATE_UNLOCK();
            }
            clutch_wants = false;   /* do not reverse on it */
        } else if (!clutch_wants) {
            tap_clutch_alert_shown = false;   /* re-arm once the plateau clears */
        }

        const char* active_trigger = NULL;
        /* Reverse duration for the chosen trigger. 0 = open-ended. */
        uint32_t trigger_reverse_ms = 0;
        /* What ENDS the cycle for whichever trigger wins, and the duration to
         * use if that action is REVERSE_TIMED. COMPLETION_RESUME means "do not
         * end it" — back off and keep cutting, which is what every trigger did
         * unconditionally before these four settings were wired up.
         *
         * Deliberately assigned in the SAME if/else chain that picks
         * active_trigger. A second chain ordered by hand is how the priority
         * and the action drift apart, and this file has already been bitten by
         * exactly that (see the duplicated tapping_set_* list in menu.c). */
        uint8_t trigger_completion = COMPLETION_RESUME;
        uint32_t trigger_timed_ms = tap_cfg->peck_reverse_out_ms;
        /* Is the WINNING trigger one of the automatic load detectors? The
         * escalation rule below must key off this, not off
         * (load_inc_wants || load_slip_wants): pedal outranks load-increase, so
         * a chip break taken while a load spike also happens to be true would
         * otherwise be counted as a load re-fire and escalate the OPERATOR's
         * pedal presses into a completion. Set in the same chain as
         * active_trigger so the two cannot disagree. */
        bool trigger_is_automatic = false;
        if (pedal_wants) {
            active_trigger = "PEDAL";
            /* Both pedal modes resume: HOLD open-ended (ends on
             * release-at-top), CHIP_BREAK after pedal_chip_break_ms. */
            trigger_completion = COMPLETION_RESUME;
            /* PEDAL_ACTION_CHIP_BREAK is documented as "Press=timed reverse,
             * auto-resume forward" — the timed part was never implemented, and
             * check_pedal_wants_action() still notes "same for chip break
             * (duration differs)". This is that duration. HOLD stays
             * open-ended: it ends on release-at-top, as before. */
            if (tap_cfg->pedal_action == PEDAL_ACTION_CHIP_BREAK) {
                trigger_reverse_ms = tap_cfg->pedal_chip_break_ms;
            }
        }
        else if (quill_lift_wants || quill_push_wants) {
            active_trigger = "QUILL";
            /* Default RESUME keeps the interactive lift/push: lift reverses,
             * push drives forward again. Choosing Stop/RevOut/RevTime makes a
             * lift end the hole instead. */
            trigger_completion = tap_cfg->quill_completion_action;
        }
        else if (depth_wants) {
            active_trigger = "DEPTH";
            /* Target depth reached — the hole is done, so this is a genuine
             * completion. Replaces the legacy two-value depth_action, whose
             * REVERSE branch fell through to an open-ended reverse that only
             * the 30 s backstop could end. */
            trigger_completion = tap_cfg->depth_completion_action;
        }
        else if (load_inc_wants) {
            active_trigger = "LOAD_INC";
            /* load_increase_reverse_ms serves as both the back-off duration
             * (RESUME) and the RevTime duration — it is the trigger's own
             * "Duration of reversal" either way. */
            trigger_completion = tap_cfg->load_completion_action;
            trigger_timed_ms = tap_cfg->load_increase_reverse_ms;
            trigger_is_automatic = true;
            /* The menu's Load > RevTim row. Without this the reverse ran to the
             * 30 s backstop instead of the operator's 50-2000 ms. */
            trigger_reverse_ms = tap_cfg->load_increase_reverse_ms;
        }
        else if (load_slip_wants) {
            active_trigger = "LOAD_SLIP";
            /* CV overshoot on a through hole = the tap broke through. */
            trigger_completion = tap_cfg->load_slip_completion_action;
            trigger_is_automatic = true;
        }
        else if (clutch_wants) {
            active_trigger = "CLUTCH";
            /* clutch_action REVERSE means "treat as overload" — back out of
             * the hole. It has no completion setting of its own; REVERSE_OUT
             * is the action that matches the words, and it replaces another
             * open-ended reverse that only the backstop ended. */
            trigger_completion = COMPLETION_REVERSE_OUT;
        }

        /* Escalate a RESUME trigger that keeps re-firing. Only the automatic
         * load triggers: the operator-driven ones (pedal chip break, quill
         * lift/push) are supposed to repeat as often as the operator asks. */
        if (active_trigger && trigger_completion == COMPLETION_RESUME &&
            trigger_is_automatic) {
            const TickType_t now_tick = xTaskGetTickCount();
            const uint32_t since = (uint32_t)(now_tick - tap_resume_last) * portTICK_PERIOD_MS;
            /* ANY automatic trigger counts toward one shared window — not
             * "the same trigger N times". Requiring identity meant two
             * detectors alternating never escalated: LOAD_INC, LOAD_SLIP,
             * LOAD_INC, LOAD_SLIP reset the counter on every event and could
             * cycle forever. That alternation is entirely plausible — a load
             * spike, then a CV overshoot as the spindle unloads during the
             * back-off — and it is precisely the case where giving up matters
             * most. tap_resume_trigger is kept only to name the last one in
             * the log. */
            if (since <= TAP_RESUME_ESCALATE_WINDOW_MS) {
                tap_resume_count++;
            } else {
                tap_resume_count = 1;
            }
            tap_resume_trigger = active_trigger;
            tap_resume_last = now_tick;

            if (tap_resume_count >= TAP_RESUME_ESCALATE_COUNT) {
                tap_log("Escalating (", active_trigger,
                        " keeps re-firing): backing out instead of resuming");
                trigger_completion = COMPLETION_REVERSE_OUT;
                tap_resume_count = 0;
                tap_resume_trigger = NULL;
            }
        }

        /* AUDIT FIX (MEDIUM, task_tapping.c:404): tap_in_completion_reverse
         * was cleared on exactly one of the five paths back to TAP_STATE_IDLE.
         * A guard-open or E-Stop abort, a mid-cycle disarm, a pedal-HOLD
         * completion or a transition timeout all left it set, and the next
         * tapping cycle then entered its first reverse already believing it
         * was in a completion reverse — terminating on at-top instead of
         * running the cycle. Both places that set it also move to
         * TAP_STATE_REVERSING, so "idle implies clear" is an invariant worth
         * asserting in one place instead of patching five exits. */
        if (tap_internal_state == TAP_STATE_IDLE) {
            tap_in_completion_reverse = false;
        }

        // ===================================================================
        // UNIFIED STATE MACHINE (Replaces mode-specific handlers)
        // ===================================================================

        /* NO MOTOR_CONTROL_LOCK here, deliberately.
         *
         * This state machine used to run its entire poll inside
         * MOTOR_CONTROL_LOCK. task_motor holds that same recursive mutex
         * across its whole MCB poll block, and inside it wait_response()
         * blocks up to MOTOR_RESPONSE_TIMEOUT_MS (250 ms) per query while
         * YIELDING (vTaskDelay(1)) with the lock still held —
         * motor_query_status() issues six queries, so the lock can be held for
         * well over a second at a stretch.
         *
         * Measured on the machine 2026-08-31: this task waited 250 ms for the
         * lock on every idle poll, and 813/1055/1178 ms during running polls.
         * A "brake_delay_ms = 100" transition took 876 ms and a
         * "pedal_chip_break_ms = 200" reverse ran 1245 ms. Every fine-grained
         * tapping timing was quantised to the MCB poll — a 9600-baud link that
         * cannot be sped up — which made the 50-500 ms range of three separate
         * settings physically unreachable.
         *
         * The lock was protecting nothing here. What this block actually does
         * is (a) pure state-machine logic, (b) MOTOR_CMD / tap_motor_stop,
         * which are xQueueSend into g_motor_cmd_queue and therefore already
         * thread-safe and already serialised by the queue, and (c)
         * motor_get_actual_rpm(), an aligned 16-bit read. The one path that
         * reaches motor hardware directly, motor_emergency_stop() via
         * MOTOR_CMD_SEND_CRITICAL's queue-full fallback, takes no mutex at
         * all — so dropping the lock cannot make it block either.
         *
         * Ordering between this task and task_motor is owned by the command
         * queue, not by this mutex. */

        // STATE 1: IDLE → CUTTING (motor start)
        if (tap_internal_state == TAP_STATE_IDLE && motor_running && motor_forward) {
            tap_internal_state = TAP_STATE_CUTTING;
            tap_prev_depth = current_depth;

            // Initialize PECK timing
            if (peck_enabled) {
                calc_peck_timing();
                tap_phase_start = xTaskGetTickCount();  // Start PECK forward timer
                tap_peck_cycle = 0;  // Reset cycle counter
            }

            // Initialize baselines for load triggers
            if (load_inc_enabled || load_slip_enabled) {
                tap_load_baseline = motor_load;
                tap_cv_baseline = target_rpm_snapshot;
                tap_baseline_start = xTaskGetTickCount();
                tap_baseline_learned = false;
                tap_cv_overshoot_count = 0;
                tap_low_load_count = 0;
            }

            STATE_LOCK();
            g_state.tap_state = TAP_STATE_CUTTING;
            STATE_UNLOCK();
        }

        // STATE 2: CUTTING → Check triggers → TRANSITION
        else if (tap_internal_state == TAP_STATE_CUTTING) {
            // Update baselines for load triggers
            if (load_inc_enabled || load_slip_enabled) {
                // Learn baseline for ~4 seconds
                if (!tap_baseline_learned) {
                    uint32_t elapsed = (xTaskGetTickCount() - tap_baseline_start) * portTICK_PERIOD_MS;
                    if (elapsed >= TAP_BASELINE_LEARN_MS) {
                        tap_baseline_learned = true;
                        uint16_t actual_cv = motor_get_actual_rpm();
                        tap_cv_baseline = actual_cv > 0 ? actual_cv : target_rpm_snapshot;
                    }
                }

                // Update KR baseline (exponential moving average, alpha=1/8)
                if (motor_load < tap_load_baseline + 10) {
                    tap_load_baseline = (tap_load_baseline * (LOAD_BASELINE_EMA_ALPHA-1) + motor_load) / LOAD_BASELINE_EMA_ALPHA;
                }
            }

            // Update quill reference when drilling
            if (quill_enabled) {
                int16_t depth_delta = current_depth - tap_prev_depth;
                const int16_t RELEASE_THRESHOLD = TAP_DEADBAND_0_1MM;
                if (depth_delta > RELEASE_THRESHOLD) {
                    tap_prev_depth = current_depth;
                }
            }

            // PECK timer-based trigger (integrated with main state machine)
            if (peck_enabled && !active_trigger) {
                uint32_t cutting_duration = (xTaskGetTickCount() - tap_phase_start) * portTICK_PERIOD_MS;
                if (cutting_duration >= tap_fwd_time_ms) {
                    active_trigger = "PECK_TIMER";
                }
            }

            // Execute reversal if any trigger fired.
            /* Every trigger's completion action is honoured here, not just
             * depth's. The chain above already resolved which trigger won and
             * what it wants; this only has to route RESUME to a trigger
             * reverse and everything else to a completion. */
            if (active_trigger) {
                if (trigger_completion == COMPLETION_RESUME) {
                    execute_trigger_reverse(active_trigger, trigger_reverse_ms);
                } else {
                    execute_completion((completion_action_t)trigger_completion,
                                       active_trigger, trigger_timed_ms);
                }
            }
        }

        // STATE 3: TRANSITION → REVERSING/FORWARD (brake delay)
        else if (tap_internal_state == TAP_STATE_TRANSITION) {
            complete_transition(tap_was_forward, current_depth);
        }

        // STATE 4: REVERSING → Check completion
        else if (tap_internal_state == TAP_STATE_REVERSING) {
            // AUDIT FIX (HIGH, task_tapping.c:285): a completion reverse
            // (REVERSE_OUT / REVERSE_TIMED) needs an explicit termination
            // condition — otherwise it loops or spams the motor queue forever.
            if (tap_in_completion_reverse) {
                uint32_t elapsed = (xTaskGetTickCount() - tap_completion_start) * portTICK_PERIOD_MS;
                bool done = false;
                if (tap_completion_kind == COMPLETION_REVERSE_TIMED) {
                    done = elapsed >= tap_completion_timed_ms;
                } else if (tap_completion_kind == COMPLETION_REVERSE_OUT) {
                    done = (current_depth <= DEPTH_AT_TOP_MM) || (elapsed >= TAP_REVERSE_OUT_MAX_MS);
                }
                if (done) {
                    tap_motor_stop();
                    tap_internal_state = TAP_STATE_IDLE;
                    tap_in_completion_reverse = false;
                    STATE_LOCK();
                    g_state.tap_state = TAP_STATE_IDLE;
                    g_state.motor_running = false;
                    g_state.state = APP_STATE_IDLE;
                    STATE_UNLOCK();
                    delay_ms(TAP_POLL_MS);
                    continue;
                }
            }

            // Update quill reference when reversing
            if (quill_enabled) {
                int16_t depth_delta = current_depth - tap_prev_depth;
                if (depth_delta < -TAP_DEADBAND_0_1MM) {
                    tap_prev_depth = current_depth;
                }
            }

            // PECK timer-based forward trigger (integrated)
            bool peck_wants_forward = false;
            /* REVIEW FIX (HIGH): once the last peck cycle called
             * execute_completion(), this block kept re-entering it. Nothing
             * reset tap_phase_start (only :348, :357 and :590 assign it), so
             * `reversing_duration >= tap_rev_time_ms` stayed true on every
             * 50 ms poll, tap_peck_cycle kept exceeding peck_cycles, and
             * execute_completion() fired again — re-arming tap_completion_start
             * each time. For COMPLETION_REVERSE_TIMED the only exit is
             * `elapsed >= peck_reverse_out_ms`, which therefore could never be
             * reached: the spindle reversed indefinitely while
             * CMD_MOTOR_TAP_REVERSE was pushed into the 16-deep motor queue at
             * 20 Hz. tap_in_completion_reverse already records "this reverse is
             * a completion, not a peck" — it just was not being consulted. */
            if (peck_enabled && !tap_in_completion_reverse) {
                uint32_t reversing_duration = (xTaskGetTickCount() - tap_phase_start) * portTICK_PERIOD_MS;
                if (reversing_duration >= tap_rev_time_ms) {
                    tap_peck_cycle++;
                    // Check if more cycles needed
                    if (tap_peck_cycle < tap_cfg->peck_cycles || tap_cfg->peck_cycles == 0) {
                        peck_wants_forward = true;
                    } else {
                        // PECK cycles complete - use completion action
                        execute_completion((completion_action_t)tap_cfg->peck_completion_action,
                                           "PECK", tap_cfg->peck_reverse_out_ms);
                    }
                }
            }

            // Check if should return to forward
            if (quill_push_wants) {
                execute_trigger_forward("QUILL_PUSH");
            }
            else if (peck_wants_forward) {
                /* REVIEW FIX (MEDIUM): this 200 ms sleep was taken while
                 * holding MOTOR_CONTROL_LOCK. task_motor (priority 4) blocks on
                 * the same recursive mutex for its entire poll block, so every
                 * inter-cycle transition of a peck cycle froze the 20 Hz poll
                 * for four cycles — suspending the load filter and all four jam
                 * detectors, repeatedly, for the whole tapping run.
                 *
                 * The explicit unlock/relock around this wait is gone with
                 * the lock itself — task_motor now runs freely throughout. The
                 * re-check below stays and matters more than ever: task_motor
                 * may have tripped a jam or a fault during the delay, and this
                 * must not then command the spindle forward. */
                delay_ms(PECK_INTER_CYCLE_DELAY_MS);  // Inter-cycle recovery

                STATE_LOCK();
                const bool still_ok = (g_state.state == APP_STATE_TAPPING) &&
                                      !g_state.estop_active &&
                                      g_state.guard_closed;
                STATE_UNLOCK();

                if (still_ok && tap_internal_state == TAP_STATE_REVERSING) {
                    execute_trigger_forward("PECK_NEXT_CYCLE");
                }
            }
            /* Trigger reverse with a duration has elapsed -> resume cutting.
             * The one new exit from REVERSING, and the only consumer of
             * tap_reverse_duration_ms. Measured from tap_phase_start, which
             * complete_transition() stamps at the instant the reverse actually
             * begins (after the brake delay), not when the trigger fired. */
            else if (tap_reverse_duration_ms > 0 &&
                     ((xTaskGetTickCount() - tap_phase_start) * portTICK_PERIOD_MS)
                         >= tap_reverse_duration_ms) {
                TAP_LOG_MS("REVERSING ran ",
                           (uint32_t)(xTaskGetTickCount() - tap_phase_start) * portTICK_PERIOD_MS);
                execute_trigger_forward("REVERSE_TIME");
            }
            // Check completion conditions
            else if (pedal_enabled && tap_cfg->pedal_action == PEDAL_ACTION_HOLD &&
                     !pedal_pressed && current_depth <= DEPTH_AT_TOP_MM) {
                // Pedal HOLD mode: complete when pedal released and at top
                tap_motor_stop();
                tap_internal_state = TAP_STATE_IDLE;
                STATE_LOCK();
                g_state.tap_state = TAP_STATE_IDLE;
                g_state.motor_running = false;
                g_state.state = APP_STATE_IDLE;
                STATE_UNLOCK();
            }
        }


        // SAFETY: Transition timeout - if stuck in TRANSITION state for >1s, force stop
        if (tap_internal_state == TAP_STATE_TRANSITION &&
            (xTaskGetTickCount() - tap_transition_start) >= pdMS_TO_TICKS(TAP_TRANSITION_TIMEOUT_MS)) {
            uart_puts("TAPPING: Transition timeout - forcing stop!\r\n");
            tap_motor_stop();
            tap_internal_state = TAP_STATE_IDLE;
            STATE_LOCK();
            g_state.tap_state = TAP_STATE_IDLE;
            g_state.motor_running = false;
            g_state.state = APP_STATE_IDLE;
            STATE_UNLOCK();
        }

        /* SAFETY: no reverse phase may run unbounded.
         *
         * REVIEW FIX (HIGH): TAP_STATE_REVERSING can be entered with NO exit
         * condition. The completion-reverse termination above only applies when
         * tap_in_completion_reverse is set (execute_completion), and the three
         * other ways out each require a specific trigger to be enabled: quill
         * push, peck timer, or pedal-HOLD release. Configure only the
         * load-increase or clutch trigger set to reverse — a
         * perfectly ordinary tapping setup — and execute_trigger_reverse()
         * lands here with none of them armed. The spindle then reverses
         * indefinitely: nothing in the state machine can leave the state, and
         * the only timeout that existed covered TRANSITION.
         *
         * TAP_MAX_CYCLE_TIME_MS has been sitting in config.h with zero
         * references for exactly this job. Legitimate reverses are bounded by
         * peck_reverse_out_ms (max 10 s) and TAP_REVERSE_OUT_MAX_MS, so 30 s is
         * a backstop that cannot fire during normal work. */
        if (tap_internal_state == TAP_STATE_REVERSING &&
            (xTaskGetTickCount() - tap_phase_start) >= pdMS_TO_TICKS(TAP_MAX_CYCLE_TIME_MS)) {
            uart_puts("TAPPING: reverse ran past the cycle limit - forcing stop!\r\n");
            tap_motor_stop();
            tap_internal_state = TAP_STATE_IDLE;
            tap_in_completion_reverse = false;
            STATE_LOCK();
            g_state.tap_state = TAP_STATE_IDLE;
            g_state.motor_running = false;
            g_state.state = APP_STATE_IDLE;
            g_state.error_until = HAL_GetTick() + 5000;
            g_state.error_line1 = "TAP REVERSE     ";
            g_state.error_line2 = "TIMEOUT - check ";
            STATE_UNLOCK();
        }

        delay_ms(TAP_POLL_MS);
    }
}
