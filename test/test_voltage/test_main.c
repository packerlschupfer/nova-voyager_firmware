/* DC bus low-voltage decision — the SHIPPING logic, not a copy of it.
 *
 * test_events used to carry sim_voltage_sample(), a re-implementation of an
 * EDGE detector that the firmware had replaced with a debounce on 2026-08-30.
 * It was green against behaviour the firmware did not have, and would have
 * stayed green whatever task_motor.c did. include/voltage_monitor.h is
 * header-inline precisely so this suite can include the real thing.
 */
#include <unity.h>
#include "voltage_monitor.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- voltage_is_plausible: the 2026-11-10 bug ---- */

/* The warning fired on every cold boot because the first UD reply after boot
 * is 0 V and the parse range started at 0, so 0 was accepted as a measurement.
 * The MCB is powered FROM the bus it is reporting; if the bus were really at
 * 0 V it could not have answered. */
void test_zero_is_not_a_measurement(void) {
    TEST_ASSERT_FALSE(voltage_is_plausible(0));
}

void test_read_failure_sentinel_is_rejected(void) {
    /* motor_read_param returns -1 on timeout. */
    TEST_ASSERT_FALSE(voltage_is_plausible(-1));
}

/* A genuine sag worth warning about must still get through — the floor rejects
 * the impossible, not the alarming. */
void test_a_real_sag_is_plausible(void) {
    TEST_ASSERT_TRUE(voltage_is_plausible(150));
    TEST_ASSERT_TRUE(voltage_is_plausible(DC_BUS_LOW_VOLTAGE_THRESHOLD - 1));
}

void test_normal_and_unloaded_peak_are_plausible(void) {
    TEST_ASSERT_TRUE(voltage_is_plausible(353));   /* measured steady state */
    TEST_ASSERT_TRUE(voltage_is_plausible(413));   /* measured unloaded peak */
}

void test_absurdly_high_is_rejected(void) {
    TEST_ASSERT_FALSE(voltage_is_plausible(501));
}

/* ---- the debounce: the two 2026-08-30 bugs ---- */

/* An edge detector seeded at the threshold warned on EVERY boot, because the
 * first readings are legitimately below 300 V. A transient must not fire. */
void test_brief_dip_does_not_warn(void) {
    voltage_monitor_t m = {0, false, false};
    for (int i = 0; i < DC_BUS_LOW_DEBOUNCE - 1; i++) {
        TEST_ASSERT_FALSE(voltage_monitor_sample(&m, 250));
    }
    TEST_ASSERT_FALSE(voltage_monitor_sample(&m, 353));   /* recovered */
}

/* An edge detector seeded at 0 treated boot as "already low", so a bus that
 * was genuinely low from power-up never warned at all. It must warn. */
void test_sustained_low_warns_once(void) {
    voltage_monitor_t m = {0, false, false};
    int fired = 0;
    for (int i = 0; i < 20; i++) {
        if (voltage_monitor_sample(&m, 200)) fired++;
    }
    TEST_ASSERT_EQUAL(1, fired);   /* once per sag, not once per poll */
}

void test_warning_fires_exactly_on_the_debounce_poll(void) {
    voltage_monitor_t m = {0, false, false};
    for (int i = 0; i < DC_BUS_LOW_DEBOUNCE - 1; i++) {
        TEST_ASSERT_FALSE(voltage_monitor_sample(&m, 100));
    }
    TEST_ASSERT_TRUE(voltage_monitor_sample(&m, 100));
}

/* Recovery must re-arm, or the first sag of a session silently swallows every
 * later one. */
void test_recovery_rearms_for_the_next_sag(void) {
    voltage_monitor_t m = {0, false, false};
    int fired = 0;
    for (int i = 0; i < DC_BUS_LOW_DEBOUNCE; i++) {
        if (voltage_monitor_sample(&m, 200)) fired++;
    }
    TEST_ASSERT_EQUAL(1, fired);

    voltage_monitor_sample(&m, 353);                 /* bus comes back */

    for (int i = 0; i < DC_BUS_LOW_DEBOUNCE; i++) {
        if (voltage_monitor_sample(&m, 200)) fired++;
    }
    TEST_ASSERT_EQUAL(2, fired);
}

/* One good reading mid-sag resets the count: the requirement is SUSTAINED. */
void test_one_good_reading_resets_the_count(void) {
    voltage_monitor_t m = {0, false, false};
    for (int i = 0; i < DC_BUS_LOW_DEBOUNCE - 1; i++) {
        TEST_ASSERT_FALSE(voltage_monitor_sample(&m, 250));
    }
    TEST_ASSERT_FALSE(voltage_monitor_sample(&m, 353));
    for (int i = 0; i < DC_BUS_LOW_DEBOUNCE - 1; i++) {
        TEST_ASSERT_FALSE(voltage_monitor_sample(&m, 250));
    }
}

/* Boundary, carried over from the tests this suite replaces: the comparison is
 * strictly-less-than, so the threshold itself is not low. */
void test_exactly_at_threshold_is_not_low(void) {
    voltage_monitor_t m = {0, false, false};
    for (int i = 0; i < DC_BUS_LOW_DEBOUNCE * 2; i++) {
        TEST_ASSERT_FALSE(voltage_monitor_sample(&m, DC_BUS_LOW_VOLTAGE_THRESHOLD));
    }
}

void test_one_below_threshold_warns_after_the_debounce(void) {
    voltage_monitor_t m = {0, false, false};
    const uint16_t v = DC_BUS_LOW_VOLTAGE_THRESHOLD - 1;
    for (int i = 0; i < DC_BUS_LOW_DEBOUNCE - 1; i++) {
        TEST_ASSERT_FALSE(voltage_monitor_sample(&m, v));
    }
    TEST_ASSERT_TRUE(voltage_monitor_sample(&m, v));
}

/* The replaced edge detector fired on the FIRST crossing. The firmware does
 * not, and must not - that behaviour is what warned on every boot. */
void test_first_low_reading_alone_does_not_warn(void) {
    voltage_monitor_t m = {0, false, false};
    TEST_ASSERT_FALSE(voltage_monitor_sample(&m, 250));
}

/* ---- the warm-up gate: 2026-11-10, measured ---- */

/* Per-command attribution on a cold boot showed exactly 6 rejected replies,
 * all UD, none on GF/SV/CV/KR/T0 across 1663 queries. So the MCB answers UD
 * before the bus is readable, and those rejections are the expected
 * pre-charge condition - not protocol errors. */
void test_rejections_before_the_first_valid_reading_are_not_errors(void) {
    voltage_monitor_t m = {0, false, false};
    TEST_ASSERT_FALSE(voltage_monitor_reject_is_error(&m));
}

/* ...but once the bus has proven readable, an implausible reply means
 * something changed, and that IS an error worth counting. */
void test_rejections_after_a_valid_reading_are_errors(void) {
    voltage_monitor_t m = {0, false, false};
    voltage_monitor_sample(&m, 353);
    TEST_ASSERT_TRUE(voltage_monitor_reject_is_error(&m));
}

/* The gate latches on the first plausible reading, and a later sag - which is
 * plausible, just low - must not un-latch it. */
void test_a_sag_does_not_reopen_the_warmup_window(void) {
    voltage_monitor_t m = {0, false, false};
    voltage_monitor_sample(&m, 353);
    for (int i = 0; i < DC_BUS_LOW_DEBOUNCE * 2; i++) {
        voltage_monitor_sample(&m, 200);
    }
    TEST_ASSERT_TRUE(voltage_monitor_reject_is_error(&m));
}

void test_reject_gate_is_null_safe(void) {
    TEST_ASSERT_FALSE(voltage_monitor_reject_is_error(NULL));
}

void test_null_is_safe(void) {
    TEST_ASSERT_FALSE(voltage_monitor_sample(NULL, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_zero_is_not_a_measurement);
    RUN_TEST(test_read_failure_sentinel_is_rejected);
    RUN_TEST(test_a_real_sag_is_plausible);
    RUN_TEST(test_normal_and_unloaded_peak_are_plausible);
    RUN_TEST(test_absurdly_high_is_rejected);
    RUN_TEST(test_brief_dip_does_not_warn);
    RUN_TEST(test_sustained_low_warns_once);
    RUN_TEST(test_warning_fires_exactly_on_the_debounce_poll);
    RUN_TEST(test_recovery_rearms_for_the_next_sag);
    RUN_TEST(test_one_good_reading_resets_the_count);
    RUN_TEST(test_exactly_at_threshold_is_not_low);
    RUN_TEST(test_one_below_threshold_warns_after_the_debounce);
    RUN_TEST(test_first_low_reading_alone_does_not_warn);
    RUN_TEST(test_rejections_before_the_first_valid_reading_are_not_errors);
    RUN_TEST(test_rejections_after_a_valid_reading_are_errors);
    RUN_TEST(test_a_sag_does_not_reopen_the_warmup_window);
    RUN_TEST(test_reject_gate_is_null_safe);
    RUN_TEST(test_null_is_safe);
    return UNITY_END();
}
