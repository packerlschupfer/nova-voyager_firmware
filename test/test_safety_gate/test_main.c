/**
 * @file test_main.c
 * @brief Tests for the REAL safety_can_start_motor() gate.
 *
 * Unlike test_safety, which models the state flags with a local struct, this
 * includes the shipping include/safety.h and exercises the actual inline
 * functions every motor-start path funnels through. A copy of the logic would
 * only prove the copy right.
 *
 * Each refusal condition is checked in isolation from an otherwise-permitting
 * state, so a condition that stopped being consulted fails exactly one test
 * rather than hiding behind another.
 */

#include <unity.h>
#include <string.h>
#include "safety.h"

/* Symbols safety.h expects from the firmware. */
shared_state_t g_state;
volatile bool g_clock_fault;
volatile bool g_brownout_latched;

/* The gate's scan check is ownership-aware (safety.h): a claim held by THIS
 * task does not refuse it a start, one held by another task does. The stub
 * lets the suite exercise both sides. */
static bool s_scan_is_ours = false;
bool motor_scan_held_by_caller(void) { return s_scan_is_ours; }
volatile bool motor_scan_mode;

static settings_t s_settings;
static bool s_settings_null;

const settings_t* settings_get(void) {
    return s_settings_null ? NULL : &s_settings;
}

void setUp(void) {
    memset(&g_state, 0, sizeof(g_state));
    memset(&s_settings, 0, sizeof(s_settings));
    g_clock_fault = false;
    g_brownout_latched = false;
    s_scan_is_ours = false;
    motor_scan_mode = false;
    s_settings_null = false;

    /* A state that permits starting: no fault, guard closed, checking on. */
    g_state.state = APP_STATE_IDLE;
    g_state.estop_active = false;
    g_state.flash_in_progress = false;
    g_state.guard_closed = true;
    s_settings.sensor.guard_check_enabled = true;
}

void tearDown(void) {}

/* --- the permitting baseline ------------------------------------------- */

void test_permits_start_when_everything_clear(void) {
    TEST_ASSERT_TRUE(safety_can_start_motor());
}

/* --- each condition must independently refuse --------------------------- */

void test_clock_fault_refuses(void) {
    g_clock_fault = true;
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

void test_estop_refuses(void) {
    g_state.estop_active = true;
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

void test_error_state_refuses(void) {
    g_state.state = APP_STATE_ERROR;
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

void test_flash_in_progress_refuses(void) {
    g_state.flash_in_progress = true;
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

void test_open_guard_refuses_when_checking_enabled(void) {
    g_state.guard_closed = false;
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

/* --- the guard feature flag -------------------------------------------- */

void test_open_guard_permitted_when_checking_disabled(void) {
    g_state.guard_closed = false;
    s_settings.sensor.guard_check_enabled = false;
    TEST_ASSERT_TRUE(safety_can_start_motor());
}

void test_null_settings_does_not_crash_and_skips_guard_check(void) {
    /* settings_get() can return NULL before settings are loaded. The guard
     * clause must short-circuit rather than dereference it. */
    s_settings_null = true;
    g_state.guard_closed = false;
    TEST_ASSERT_TRUE(safety_can_start_motor());
}

void test_null_settings_still_honours_the_other_conditions(void) {
    s_settings_null = true;
    g_state.estop_active = true;
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

/* --- a clock fault outranks a permitting state -------------------------- */

void test_clock_fault_refuses_even_with_guard_check_disabled(void) {
    s_settings.sensor.guard_check_enabled = false;
    g_clock_fault = true;
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

/* --- refusal reasons match the condition -------------------------------- */

void test_reason_reports_clock_fault_first(void) {
    g_clock_fault = true;
    g_state.estop_active = true;   /* clock fault is checked first */
    TEST_ASSERT_EQUAL_STRING("clock fault - crystal failed", safety_refusal_reason());
}

void test_reason_reports_estop(void) {
    g_state.estop_active = true;
    TEST_ASSERT_EQUAL_STRING("E-Stop engaged", safety_refusal_reason());
}

void test_reason_reports_guard_open(void) {
    g_state.guard_closed = false;
    TEST_ASSERT_EQUAL_STRING("guard open", safety_refusal_reason());
}

void test_reason_reports_flash_write(void) {
    g_state.flash_in_progress = true;
    TEST_ASSERT_EQUAL_STRING("flash write in progress", safety_refusal_reason());
}

/* An MCB parameter write suspends task_motor's whole poll block, and all four
 * jam detectors live inside it. Starting the spindle in that window means
 * cutting with no jam detection at all, so the gate refuses.
 *
 * This closes a TOCTOU hole: the sync paths sampled g_state.motor_running once
 * and then held motor_scan_mode for ~2.2 s, so a start arriving inside the
 * window passed a check that had already been made. */
/* Brown-out interlock (PVD), added 2026-08-30. A supply that sagged through the
 * threshold refuses a start, and unlike motor_scan_mode it LATCHES: clearing it
 * is a reset's job. See include/brownout.h. */
static void test_brownout_refuses_start(void) {
    TEST_ASSERT_TRUE(safety_can_start_motor());
    g_brownout_latched = true;
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

static void test_reason_reports_brownout(void) {
    g_brownout_latched = true;
    TEST_ASSERT_EQUAL_STRING("supply brown-out detected - power cycle",
                             safety_refusal_reason());
}

/* The clock fault is the more fundamental failure and is reported first. */
static void test_clock_fault_outranks_brownout_in_reason(void) {
    g_brownout_latched = true;
    g_clock_fault = true;
    TEST_ASSERT_EQUAL_STRING("clock fault - crystal failed",
                             safety_refusal_reason());
}

/* The LCD rendering must follow the SAME precedence as the console reason —
 * the panel and the console naming different causes would be worse than the
 * panel saying nothing. Added with the LCD refusal display 2026-08-31. */
static void test_lcd_refusal_matches_console_precedence(void) {
    const char* l1;
    const char* l2;

    /* Brown-out outranks a transient MCB write... */
    g_brownout_latched = true;
    motor_scan_mode = true;
    safety_refusal_lcd(&l1, &l2);
    TEST_ASSERT_EQUAL_STRING("! BROWNOUT !    ", l1);
    TEST_ASSERT_EQUAL_STRING("supply brown-out detected - power cycle",
                             safety_refusal_reason());

    /* ...and a clock fault outranks the brown-out, in both renderings. */
    g_clock_fault = true;
    safety_refusal_lcd(&l1, &l2);
    TEST_ASSERT_EQUAL_STRING("! CLOCK FAULT ! ", l1);
    TEST_ASSERT_EQUAL_STRING("clock fault - crystal failed",
                             safety_refusal_reason());
}

/* Every line must be exactly LCD_COLS wide: display.c pads, but a line that is
 * too LONG would be truncated mid-word on the panel. */
static void test_lcd_refusal_lines_are_display_width(void) {
    const char* l1;
    const char* l2;
    struct { bool* flag; } _unused;
    (void)_unused;

    /* Walk every branch of the chain and measure both lines. */
    g_clock_fault = true;  safety_refusal_lcd(&l1, &l2);
    TEST_ASSERT_EQUAL_UINT(16, strlen(l1)); TEST_ASSERT_EQUAL_UINT(16, strlen(l2));
    g_clock_fault = false; g_brownout_latched = true;  safety_refusal_lcd(&l1, &l2);
    TEST_ASSERT_EQUAL_UINT(16, strlen(l1)); TEST_ASSERT_EQUAL_UINT(16, strlen(l2));
    g_brownout_latched = false; g_state.estop_active = true; safety_refusal_lcd(&l1, &l2);
    TEST_ASSERT_EQUAL_UINT(16, strlen(l1)); TEST_ASSERT_EQUAL_UINT(16, strlen(l2));
    g_state.estop_active = false; g_state.state = APP_STATE_ERROR; safety_refusal_lcd(&l1, &l2);
    TEST_ASSERT_EQUAL_UINT(16, strlen(l1)); TEST_ASSERT_EQUAL_UINT(16, strlen(l2));
    g_state.state = APP_STATE_IDLE; g_state.flash_in_progress = true; safety_refusal_lcd(&l1, &l2);
    TEST_ASSERT_EQUAL_UINT(16, strlen(l1)); TEST_ASSERT_EQUAL_UINT(16, strlen(l2));
    g_state.flash_in_progress = false; g_state.guard_closed = false; safety_refusal_lcd(&l1, &l2);
    TEST_ASSERT_EQUAL_UINT(16, strlen(l1)); TEST_ASSERT_EQUAL_UINT(16, strlen(l2));
    g_state.guard_closed = true; motor_scan_mode = true; safety_refusal_lcd(&l1, &l2);
    TEST_ASSERT_EQUAL_UINT(16, strlen(l1)); TEST_ASSERT_EQUAL_UINT(16, strlen(l2));
    motor_scan_mode = false; safety_refusal_lcd(&l1, &l2);   /* the fallback */
    TEST_ASSERT_EQUAL_UINT(16, strlen(l1)); TEST_ASSERT_EQUAL_UINT(16, strlen(l2));
}

static void test_mcb_parameter_write_refuses(void) {
    motor_scan_mode = true;       /* held by SOMEONE ELSE */
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

/* REVIEW FIX (CRITICAL) regression test. align_gate_ok() used to return true on
 * seeing its own claim BEFORE consulting this gate, so a second ALIGN inside an
 * existing session re-energized the windings with the E-Stop engaged — the
 * v0.1.0 ALIGN bypass, reintroduced by the re-entrancy shortcut added for it.
 * Re-entrancy is handled HERE now, so it cannot be skipped: holding the claim
 * yourself excuses you from the SCAN condition and from nothing else. */
static void test_own_scan_claim_does_not_excuse_estop(void) {
    motor_scan_mode = true;
    s_scan_is_ours = true;
    TEST_ASSERT_TRUE(safety_can_start_motor());   /* our own claim: fine */

    g_state.estop_active = true;
    TEST_ASSERT_FALSE(safety_can_start_motor()); /* ...but E-Stop still refuses */
    TEST_ASSERT_EQUAL_STRING("E-Stop engaged", safety_refusal_reason());
}

static void test_own_scan_claim_does_not_excuse_guard_or_brownout(void) {
    motor_scan_mode = true;
    s_scan_is_ours = true;

    g_state.guard_closed = false;
    TEST_ASSERT_FALSE(safety_can_start_motor());
    g_state.guard_closed = true;

    g_brownout_latched = true;
    TEST_ASSERT_FALSE(safety_can_start_motor());
}

/* A claim held by another task must still refuse, and still say so. */
static void test_foreign_scan_claim_still_refuses(void) {
    motor_scan_mode = true;
    s_scan_is_ours = false;
    TEST_ASSERT_FALSE(safety_can_start_motor());
    TEST_ASSERT_EQUAL_STRING("MCB parameter write in progress",
                             safety_refusal_reason());
}

static void test_reason_reports_mcb_parameter_write(void) {
    motor_scan_mode = true;
    TEST_ASSERT_EQUAL_STRING("MCB parameter write in progress",
                             safety_refusal_reason());
}

/* ...and stops refusing once the write finishes — a latched flag here would
 * brick every start until the next reboot. */
static void test_start_permitted_again_after_the_write(void) {
    motor_scan_mode = true;
    TEST_ASSERT_FALSE(safety_can_start_motor());
    motor_scan_mode = false;
    TEST_ASSERT_TRUE(safety_can_start_motor());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lcd_refusal_matches_console_precedence);
    RUN_TEST(test_lcd_refusal_lines_are_display_width);
    RUN_TEST(test_brownout_refuses_start);
    RUN_TEST(test_reason_reports_brownout);
    RUN_TEST(test_clock_fault_outranks_brownout_in_reason);
    RUN_TEST(test_mcb_parameter_write_refuses);
    RUN_TEST(test_own_scan_claim_does_not_excuse_estop);
    RUN_TEST(test_own_scan_claim_does_not_excuse_guard_or_brownout);
    RUN_TEST(test_foreign_scan_claim_still_refuses);
    RUN_TEST(test_reason_reports_mcb_parameter_write);
    RUN_TEST(test_start_permitted_again_after_the_write);
    RUN_TEST(test_permits_start_when_everything_clear);
    RUN_TEST(test_clock_fault_refuses);
    RUN_TEST(test_estop_refuses);
    RUN_TEST(test_error_state_refuses);
    RUN_TEST(test_flash_in_progress_refuses);
    RUN_TEST(test_open_guard_refuses_when_checking_enabled);
    RUN_TEST(test_open_guard_permitted_when_checking_disabled);
    RUN_TEST(test_null_settings_does_not_crash_and_skips_guard_check);
    RUN_TEST(test_null_settings_still_honours_the_other_conditions);
    RUN_TEST(test_clock_fault_refuses_even_with_guard_check_disabled);
    RUN_TEST(test_reason_reports_clock_fault_first);
    RUN_TEST(test_reason_reports_estop);
    RUN_TEST(test_reason_reports_guard_open);
    RUN_TEST(test_reason_reports_flash_write);
    return UNITY_END();
}
