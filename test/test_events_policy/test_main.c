/**
 * @file test_main.c
 * @brief Tests for the REAL guard / E-Stop event predicates.
 *
 * Includes the shipping include/events_policy.h. test/test_events, by
 * contrast, re-implements its handlers, which is why it passed all through
 * v0.1.0 while the shipped condition was wrong.
 *
 * The bug: both handlers branched on the CURRENT pin level when the queued
 * event was finally dequeued. A guard bumped open and shut in between matched
 * neither branch — the motor command queue was not purged, no stop was sent,
 * no spindle hold was applied, and PD4 was left low with the state still
 * reading DRILLING. The spindle coasted, unbraked.
 */

#include <unity.h>
#include "events_policy.h"

void setUp(void) {}
void tearDown(void) {}

/*--- guard: the regression ------------------------------------------------*/

/* The exact race: the edge latch says it opened, the live level already says
 * closed again. This is the case that used to do nothing at all. */
static void test_guard_bounce_while_drilling_still_aborts(void) {
    TEST_ASSERT_TRUE(guard_requires_abort(true  /* opened since */,
                                          true  /* closed now */,
                                          true  /* was drilling */));
}

static void test_guard_held_open_while_drilling_aborts(void) {
    TEST_ASSERT_TRUE(guard_requires_abort(true, false, true));
}

/* Latch missed (event delivered fast enough that the level is still open). */
static void test_guard_open_level_alone_aborts(void) {
    TEST_ASSERT_TRUE(guard_requires_abort(false, false, true));
}

static void test_guard_closed_and_never_opened_does_not_abort(void) {
    TEST_ASSERT_FALSE(guard_requires_abort(false, true, true));
}

/* A guard opened while the machine is idle is not an abort — there is nothing
 * to stop, and forcing a spindle hold there would be a nuisance, not safety. */
static void test_guard_bounce_while_idle_does_not_abort(void) {
    TEST_ASSERT_FALSE(guard_requires_abort(true, true, false));
}

static void test_guard_held_open_while_idle_does_not_abort(void) {
    TEST_ASSERT_FALSE(guard_requires_abort(true, false, false));
}

/*--- guard: release --------------------------------------------------------*/

static void test_release_when_closed_and_hold_active(void) {
    TEST_ASSERT_TRUE(guard_permits_release(false, true, true));
}

static void test_no_release_without_an_active_hold(void) {
    TEST_ASSERT_FALSE(guard_permits_release(false, true, false));
}

static void test_no_release_while_guard_still_open(void) {
    TEST_ASSERT_FALSE(guard_permits_release(false, false, true));
}

/* A bounce brakes the spindle. It must not then immediately let go in the same
 * pass — that would apply a hold and release it microseconds later. */
static void test_no_release_in_the_same_pass_as_an_abort(void) {
    TEST_ASSERT_FALSE(guard_permits_release(true /* aborting */, true, true));
}

/*--- E-Stop ---------------------------------------------------------------*/

static void test_estop_held_stops(void) {
    TEST_ASSERT_TRUE(estop_requires_stop(true, true));
}

/* The regression: pressed and released before the event was dequeued. This
 * used to take the "RELEASED" branch only — no queue purge, no stop, and the
 * state cleared to IDLE as though nothing had happened. */
static void test_estop_bounce_still_stops(void) {
    TEST_ASSERT_TRUE(estop_requires_stop(true /* engaged since */,
                                         false /* released now */));
}

static void test_estop_never_engaged_does_not_stop(void) {
    TEST_ASSERT_FALSE(estop_requires_stop(false, false));
}

static void test_estop_released_recovers(void) {
    TEST_ASSERT_TRUE(estop_requires_recovery(false));
}

static void test_estop_held_does_not_recover(void) {
    TEST_ASSERT_FALSE(estop_requires_recovery(true));
}

/* A bounce runs both halves, in order: stop, then recover. That lands on the
 * state the press-then-release physically produced. Running only the stop half
 * would leave a persistent E-Stop screen with no further edge to clear it. */
static void test_estop_bounce_runs_stop_then_recovery(void) {
    const bool engaged_since = true, active_now = false;
    TEST_ASSERT_TRUE(estop_requires_stop(engaged_since, active_now));
    TEST_ASSERT_TRUE(estop_requires_recovery(active_now));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_guard_bounce_while_drilling_still_aborts);
    RUN_TEST(test_guard_held_open_while_drilling_aborts);
    RUN_TEST(test_guard_open_level_alone_aborts);
    RUN_TEST(test_guard_closed_and_never_opened_does_not_abort);
    RUN_TEST(test_guard_bounce_while_idle_does_not_abort);
    RUN_TEST(test_guard_held_open_while_idle_does_not_abort);
    RUN_TEST(test_release_when_closed_and_hold_active);
    RUN_TEST(test_no_release_without_an_active_hold);
    RUN_TEST(test_no_release_while_guard_still_open);
    RUN_TEST(test_no_release_in_the_same_pass_as_an_abort);
    RUN_TEST(test_estop_held_stops);
    RUN_TEST(test_estop_bounce_still_stops);
    RUN_TEST(test_estop_never_engaged_does_not_stop);
    RUN_TEST(test_estop_released_recovers);
    RUN_TEST(test_estop_held_does_not_recover);
    RUN_TEST(test_estop_bounce_runs_stop_then_recovery);
    return UNITY_END();
}
