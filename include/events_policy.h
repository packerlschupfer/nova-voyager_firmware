/**
 * @file events_policy.h
 * @brief The guard / E-Stop event decisions, as pure predicates.
 *
 * Header-only so test/test_events_policy exercises the real conditions rather
 * than a paraphrase. test/test_events re-implements its handlers ("logic
 * extracted from events.c") and therefore could not have caught the bug these
 * predicates exist for — a copy of the logic only proves the copy right.
 *
 * THE BUG
 * -------
 * handle_btn_guard() and handle_btn_estop() branched on the CURRENT pin level
 * at the moment the queued event was dequeued. task_ui.c:202 runs at priority 2
 * and refreshes g_state.guard_closed from the live pin every 2 ms, while
 * task_main runs at priority 1. A guard bumped open and shut before its event
 * was processed therefore matched NEITHER branch: no motor-queue purge, no
 * CMD_MOTOR_STOP, no spindle hold, and no motor_hardware_enable() to undo the
 * PD4 drop the ISR had already done. The spindle coasted unbraked while
 * g_state.state still read APP_STATE_DRILLING.
 *
 * An edge is not a level. encoder.c latches the edge; these predicates consume
 * the latch.
 */

#ifndef EVENTS_POLICY_H
#define EVENTS_POLICY_H

#include <stdbool.h>

/**
 * @brief Should a guard event run the stop + spindle-hold path?
 *
 * @param guard_opened_since  Edge latch: the guard was open at some point
 *                            since the last check (encoder_guard_opened_since).
 * @param guard_closed_now    Live level at dequeue time.
 * @param motor_active        The machine was drilling or tapping.
 *
 * True if the guard was open at any point, however briefly, while the machine
 * was cutting.
 */
static inline bool guard_requires_abort(bool guard_opened_since,
                                        bool guard_closed_now,
                                        bool motor_active) {
    const bool was_open = guard_opened_since || !guard_closed_now;
    return was_open && motor_active;
}

/**
 * @brief Should a guard event release an active spindle hold?
 *
 * Only once the guard is shut again, and only if a hold is actually running.
 * Deliberately evaluated after guard_requires_abort() and not in the same
 * pass as it: a bounce should brake the spindle, not brake it and instantly
 * let go.
 */
static inline bool guard_permits_release(bool aborting,
                                         bool guard_closed_now,
                                         bool hold_active) {
    return !aborting && guard_closed_now && hold_active;
}

/**
 * @brief Should an E-Stop event run the stop + hold + ERROR path?
 *
 * True while the button is held, and also for a press that was already
 * released by the time the event was processed. An E-Stop the operator felt
 * as a press must always stop the machine.
 */
static inline bool estop_requires_stop(bool estop_engaged_since,
                                       bool estop_active_now) {
    return estop_active_now || estop_engaged_since;
}

/**
 * @brief Should an E-Stop event run the recovery path?
 *
 * Whenever the button is not currently held — including immediately after
 * estop_requires_stop() has run for the same event, which is the bounce case.
 * Both firing in order lands on the state a press-then-release physically
 * produced: commands purged, motor stopped, then recovery to IDLE. Running
 * only the stop half would leave a persistent E-Stop screen with no further
 * edge coming to clear it.
 */
static inline bool estop_requires_recovery(bool estop_active_now) {
    return !estop_active_now;
}

#endif /* EVENTS_POLICY_H */
