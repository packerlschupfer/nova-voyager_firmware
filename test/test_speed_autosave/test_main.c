/**
 * @file test_main.c
 * @brief Tests for the REAL last-used-speed debounce.
 *
 * Includes the shipping include/speed_autosave.h.
 *
 * What this guards: the operator sweeps the encoder through dozens of values
 * on the way to the one they want. Committing each one would be thousands of
 * writes per session against a finite-endurance AT24C02. Committing none — the
 * v0.1.0 behaviour — means pressing OFF loses the speed entirely.
 */

#include <unity.h>
#include <string.h>
#include "speed_autosave.h"

static speed_autosave_t st;

void setUp(void)   { memset(&st, 0, sizeof(st)); }
void tearDown(void) {}

/* The operator turning the knob, once. */
static void note(uint16_t rpm, uint32_t t) { speed_autosave_note(&st, rpm, t); }

/* Poll; returns the value to write, or 0 if nothing is due. */
static uint16_t poll(uint16_t persisted, uint32_t t) {
    uint16_t out = 0;
    return speed_autosave_due(&st, persisted, t, &out) ? out : 0;
}

/*--- the basic contract ---------------------------------------------------*/

/* Nothing was chosen, so nothing is ever written — however long we wait.
 * This is the property the polling design could not have: it sampled a field
 * that update_sv_state() and step-drill mode also write. */
static void test_nothing_is_written_without_an_operator_action(void) {
    for (uint32_t t = 0; t < 60000; t += 100) {
        TEST_ASSERT_EQUAL_UINT16(0, poll(1200, t));
    }
}

static void test_settled_choice_commits_once_after_the_debounce(void) {
    note(1500, 1000);
    TEST_ASSERT_EQUAL_UINT16(0, poll(1200, 1000 + SPEED_AUTOSAVE_DEBOUNCE_MS - 1));
    TEST_ASSERT_EQUAL_UINT16(1500, poll(1200, 1000 + SPEED_AUTOSAVE_DEBOUNCE_MS));

    /* Written now, so it must not fire again on every later poll. */
    for (uint32_t t = 20000; t < 40000; t += 100) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, poll(1500, t), "committed value re-committed");
    }
}

/*--- the debounce ---------------------------------------------------------*/

/* A sweep through 40 detents must produce ONE write, not 40. */
static void test_a_long_sweep_commits_only_the_final_value(void) {
    uint32_t t = 0;
    uint16_t rpm = 1000;
    int commits = 0;
    for (int i = 0; i < 40; i++) {
        rpm += 50;
        t += 120;               /* a brisk turn, ~8 detents/second */
        note(rpm, t);
        if (poll(1000, t)) commits++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, commits, "committed mid-sweep");

    t += SPEED_AUTOSAVE_DEBOUNCE_MS;
    TEST_ASSERT_EQUAL_UINT16(3000, poll(1000, t));
    TEST_ASSERT_EQUAL_UINT16(3000, rpm);
}

static void test_a_pause_short_of_the_threshold_restarts_the_debounce(void) {
    note(1100, 1000);
    TEST_ASSERT_EQUAL_UINT16(0, poll(1000, 1000 + SPEED_AUTOSAVE_DEBOUNCE_MS - 100));

    note(1200, 1000 + SPEED_AUTOSAVE_DEBOUNCE_MS - 50);   /* moves again */
    /* The old deadline must not carry over. */
    TEST_ASSERT_EQUAL_UINT16(0, poll(1000, 1000 + SPEED_AUTOSAVE_DEBOUNCE_MS + 50));
    TEST_ASSERT_EQUAL_UINT16(1200, poll(1000, 1000 + SPEED_AUTOSAVE_DEBOUNCE_MS - 50
                                              + SPEED_AUTOSAVE_DEBOUNCE_MS));
}

/* Turned away and back again: the choice equals what is stored, so no write. */
static void test_returning_to_the_stored_value_commits_nothing(void) {
    note(1500, 1000);
    note(1200, 2000);
    TEST_ASSERT_EQUAL_UINT16(0, poll(1200, 2000 + SPEED_AUTOSAVE_DEBOUNCE_MS + 1));
}

/* ...and having disarmed on that no-op, a later genuine change still works. */
static void test_a_no_op_choice_does_not_disable_later_ones(void) {
    note(1200, 1000);
    TEST_ASSERT_EQUAL_UINT16(0, poll(1200, 1000 + SPEED_AUTOSAVE_DEBOUNCE_MS + 1));
    note(1800, 20000);
    TEST_ASSERT_EQUAL_UINT16(1800, poll(1200, 20000 + SPEED_AUTOSAVE_DEBOUNCE_MS));
}

/*--- wrap -----------------------------------------------------------------*/

/* HAL_GetTick() is a 32-bit ms counter and wraps after ~49 days. A machine
 * left powered that long must still save the speed. */
static void test_debounce_survives_the_tick_wrap(void) {
    const uint32_t before_wrap = 0xFFFFFF00u;
    note(1500, before_wrap);
    const uint32_t after = before_wrap + SPEED_AUTOSAVE_DEBOUNCE_MS;
    TEST_ASSERT_TRUE(after < before_wrap);          /* really did wrap */
    TEST_ASSERT_EQUAL_UINT16(0, poll(1200, before_wrap + 10));
    TEST_ASSERT_EQUAL_UINT16(1500, poll(1200, after));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_is_written_without_an_operator_action);
    RUN_TEST(test_settled_choice_commits_once_after_the_debounce);
    RUN_TEST(test_a_long_sweep_commits_only_the_final_value);
    RUN_TEST(test_a_pause_short_of_the_threshold_restarts_the_debounce);
    RUN_TEST(test_returning_to_the_stored_value_commits_nothing);
    RUN_TEST(test_a_no_op_choice_does_not_disable_later_ones);
    RUN_TEST(test_debounce_survives_the_tick_wrap);
    return UNITY_END();
}
