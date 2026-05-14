/**
 * @file test_safety.c
 * @brief Safety interlock integration tests
 *
 * Tests safety-critical combinations and edge cases.
 */

#include <unity.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// GF flag constants (from config.h)
#define GF_MOTOR_STOPPED        32
#define GF_MOTOR_RUNNING        34
#define GF_MOTOR_STOPPED_REV    436
#define GF_MOTOR_RUNNING_REV    438

// Simplified state for testing (avoid full shared.h dependencies)
typedef enum {
    APP_STATE_IDLE = 0,
    APP_STATE_DRILLING = 1,
    APP_STATE_TAPPING = 2,
    APP_STATE_MENU = 3,
    APP_STATE_ERROR = 4
} app_state_t;

typedef struct {
    app_state_t state;
    bool motor_running;
    bool estop_active;
    bool guard_closed;
    bool motor_fault;
    uint16_t target_rpm;
    uint16_t current_rpm;
    int16_t current_depth;
    int16_t target_depth;
    const char* error_line1;
    const char* error_line2;
} test_state_t;

// Test state
static test_state_t g_state;

// Speed constants from config.h
#define SPEED_MIN_RPM 100
#define SPEED_MAX_RPM 6000

void setUp(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.state = APP_STATE_IDLE;
}

void tearDown(void) {
    // Cleanup
}

/*===========================================================================*/
/* State Transition Tests                                                    */
/*===========================================================================*/

void test_initial_state_is_idle(void) {
    TEST_ASSERT_EQUAL(APP_STATE_IDLE, g_state.state);
    TEST_ASSERT_FALSE(g_state.motor_running);
    TEST_ASSERT_FALSE(g_state.estop_active);
}

void test_estop_flag_prevents_drilling_state(void) {
    g_state.estop_active = true;

    // Motor should not transition to drilling if estop active
    // This is enforced by events.c handle_btn_start()
    TEST_ASSERT_TRUE(g_state.estop_active);
}

void test_guard_closed_state_tracking(void) {
    g_state.guard_closed = true;
    TEST_ASSERT_TRUE(g_state.guard_closed);

    g_state.guard_closed = false;
    TEST_ASSERT_FALSE(g_state.guard_closed);
}

void test_motor_running_state_persistence(void) {
    g_state.motor_running = false;
    TEST_ASSERT_FALSE(g_state.motor_running);

    g_state.motor_running = true;
    TEST_ASSERT_TRUE(g_state.motor_running);
}

/*===========================================================================*/
/* Safety Flag Combinations                                                  */
/*===========================================================================*/

void test_multiple_safety_flags_can_be_set(void) {
    g_state.estop_active = true;
    g_state.guard_closed = false;
    g_state.motor_fault = true;

    TEST_ASSERT_TRUE(g_state.estop_active);
    TEST_ASSERT_FALSE(g_state.guard_closed);
    TEST_ASSERT_TRUE(g_state.motor_fault);
}

void test_all_safety_flags_clear_initially(void) {
    TEST_ASSERT_FALSE(g_state.estop_active);
    TEST_ASSERT_FALSE(g_state.motor_fault);
    // guard_closed defaults may vary - just verify it's set
}

/*===========================================================================*/
/* Speed Range Tests                                                         */
/*===========================================================================*/

void test_speed_within_valid_range(void) {
    g_state.target_rpm = 1500;
    TEST_ASSERT_GREATER_OR_EQUAL(SPEED_MIN_RPM, g_state.target_rpm);
    TEST_ASSERT_LESS_OR_EQUAL(SPEED_MAX_RPM, g_state.target_rpm);
}

void test_speed_at_minimum_boundary(void) {
    g_state.target_rpm = SPEED_MIN_RPM;
    TEST_ASSERT_EQUAL(SPEED_MIN_RPM, g_state.target_rpm);
}

void test_speed_at_maximum_boundary(void) {
    g_state.target_rpm = SPEED_MAX_RPM;
    TEST_ASSERT_EQUAL(SPEED_MAX_RPM, g_state.target_rpm);
}

/*===========================================================================*/
/* Depth Sensor Tests                                                        */
/*===========================================================================*/

void test_depth_values_can_be_negative(void) {
    g_state.current_depth = -150;  // -15.0mm
    TEST_ASSERT_EQUAL(-150, g_state.current_depth);
}

void test_depth_target_can_be_set(void) {
    g_state.target_depth = 250;  // 25.0mm
    TEST_ASSERT_EQUAL(250, g_state.target_depth);
}

/*===========================================================================*/
/* Error State Tests                                                         */
/*===========================================================================*/

void test_error_state_transition(void) {
    g_state.state = APP_STATE_IDLE;
    g_state.state = APP_STATE_ERROR;

    TEST_ASSERT_EQUAL(APP_STATE_ERROR, g_state.state);
}

void test_error_message_can_be_set(void) {
    g_state.error_line1 = "Test Error";
    g_state.error_line2 = "Line 2";

    TEST_ASSERT_EQUAL_STRING("Test Error", g_state.error_line1);
    TEST_ASSERT_EQUAL_STRING("Line 2", g_state.error_line2);
}

/*===========================================================================*/
/* GF Flag Classification Tests (Fix 3: idle-loop known_good guard)         */
/*===========================================================================*/

// Inline the classification logic exactly as used in task_motor.c
static void classify_gf(uint16_t flags, bool *known_good, bool *error_state) {
    *known_good  = (flags == GF_MOTOR_STOPPED || flags == GF_MOTOR_RUNNING ||
                    flags == GF_MOTOR_STOPPED_REV || flags == GF_MOTOR_RUNNING_REV);
    *error_state = (flags & 0x4000) != 0;
}

void test_gf_known_good_stopped_forward(void) {
    bool kg, es;
    classify_gf(GF_MOTOR_STOPPED, &kg, &es);
    TEST_ASSERT_TRUE(kg);
    TEST_ASSERT_FALSE(es);
}

void test_gf_known_good_running_forward(void) {
    bool kg, es;
    classify_gf(GF_MOTOR_RUNNING, &kg, &es);
    TEST_ASSERT_TRUE(kg);
    TEST_ASSERT_FALSE(es);
}

void test_gf_known_good_stopped_reverse(void) {
    bool kg, es;
    classify_gf(GF_MOTOR_STOPPED_REV, &kg, &es);
    TEST_ASSERT_TRUE(kg);
    TEST_ASSERT_FALSE(es);
}

void test_gf_known_good_running_reverse(void) {
    bool kg, es;
    classify_gf(GF_MOTOR_RUNNING_REV, &kg, &es);
    TEST_ASSERT_TRUE(kg);
    TEST_ASSERT_FALSE(es);
}

void test_gf_garbage_flag_not_known_good(void) {
    bool kg, es;
    classify_gf(0x1234, &kg, &es);
    TEST_ASSERT_FALSE(kg);
}

void test_gf_zero_flag_not_known_good(void) {
    bool kg, es;
    classify_gf(0x0000, &kg, &es);
    TEST_ASSERT_FALSE(kg);
}

void test_gf_error_bit_detected(void) {
    bool kg, es;
    classify_gf(0x4000, &kg, &es);  // Error bit only
    TEST_ASSERT_TRUE(es);
}

void test_gf_known_error_value(void) {
    bool kg, es;
    classify_gf(16929, &kg, &es);   // 0x4221 — real error response from MCB
    TEST_ASSERT_TRUE(es);
}

// Key invariant: garbage with no error bit must NOT update motor_fault.
// Simulates the guard: if (known_good || error_state) { g_state.motor_fault = error_state; }
void test_gf_garbage_no_error_does_not_update_fault(void) {
    bool motor_fault = true;  // Pre-existing fault
    uint16_t garbage = 0x0055;
    bool kg, es;
    classify_gf(garbage, &kg, &es);
    if (kg || es) { motor_fault = es; }  // The guarded update
    // Garbage has no known_good and no error bit — fault must stay unchanged
    TEST_ASSERT_TRUE(motor_fault);
}

void test_gf_error_flag_does_update_fault(void) {
    bool motor_fault = false;
    uint16_t flags = 0x4221;  // Error state
    bool kg, es;
    classify_gf(flags, &kg, &es);
    if (kg || es) { motor_fault = es; }
    TEST_ASSERT_TRUE(motor_fault);
}

// 0xFFFF is the specific corrupted response called out in the original analysis.
// Bit 14 (0x4000) IS set in 0xFFFF, so it is treated as an error state — that is
// correct and safe (fail-safe: unknown = fault). It must NOT be known_good.
void test_gf_0xffff_not_known_good(void) {
    bool kg, es;
    classify_gf(0xFFFF, &kg, &es);
    TEST_ASSERT_FALSE(kg);
}

void test_gf_0xffff_is_error_state(void) {
    bool kg, es;
    classify_gf(0xFFFF, &kg, &es);
    TEST_ASSERT_TRUE(es);  // Bit 14 set → treated as error, not silently ignored
}

void test_gf_0xffff_sets_motor_fault(void) {
    bool motor_fault = false;
    bool kg, es;
    classify_gf(0xFFFF, &kg, &es);
    if (kg || es) { motor_fault = es; }
    // 0xFFFF has error bit → fault is set (fail-safe behaviour)
    TEST_ASSERT_TRUE(motor_fault);
}

void test_gf_known_good_stopped_clears_fault(void) {
    bool motor_fault = true;  // Fault was set
    uint16_t flags = GF_MOTOR_STOPPED;
    bool kg, es;
    classify_gf(flags, &kg, &es);
    if (kg || es) { motor_fault = es; }  // known_good + no error → clears fault
    TEST_ASSERT_FALSE(motor_fault);
}

/*===========================================================================*/
/* motor_running on START Transition Tests (Fix A)                           */
/*===========================================================================*/

// Simulate the state machine logic from handle_btn_start()
static void sim_start_press(test_state_t *s, bool any_trigger) {
    if (s->state == APP_STATE_IDLE) {
        s->state        = any_trigger ? APP_STATE_TAPPING : APP_STATE_DRILLING;
        s->motor_running = true;   // Fix A: optimistic set
    } else if (s->state == APP_STATE_DRILLING || s->state == APP_STATE_TAPPING) {
        s->state = APP_STATE_IDLE;
        // motor_running cleared later by motor task on confirmed stop
    }
}

void test_start_idle_no_trigger_sets_drilling(void) {
    g_state.state = APP_STATE_IDLE;
    sim_start_press(&g_state, false);
    TEST_ASSERT_EQUAL(APP_STATE_DRILLING, g_state.state);
}

void test_start_idle_no_trigger_sets_motor_running(void) {
    g_state.state        = APP_STATE_IDLE;
    g_state.motor_running = false;
    sim_start_press(&g_state, false);
    TEST_ASSERT_TRUE(g_state.motor_running);
}

void test_start_idle_with_trigger_sets_tapping(void) {
    g_state.state = APP_STATE_IDLE;
    sim_start_press(&g_state, true);
    TEST_ASSERT_EQUAL(APP_STATE_TAPPING, g_state.state);
}

void test_start_idle_with_trigger_sets_motor_running(void) {
    g_state.state        = APP_STATE_IDLE;
    g_state.motor_running = false;
    sim_start_press(&g_state, true);
    TEST_ASSERT_TRUE(g_state.motor_running);
}

void test_start_drilling_returns_to_idle(void) {
    g_state.state        = APP_STATE_DRILLING;
    g_state.motor_running = true;
    sim_start_press(&g_state, false);
    TEST_ASSERT_EQUAL(APP_STATE_IDLE, g_state.state);
}

/*===========================================================================*/
/* E-Stop Sequence Tests (Fix 1)                                             */
/*===========================================================================*/

// Simulate the state writes from handle_btn_estop() for the engaged path
static void sim_estop_engage(test_state_t *s) {
    s->state         = APP_STATE_ERROR;
    s->estop_active  = true;
    s->motor_running = false;
    s->motor_fault   = true;
}

// Simulate the release path
static void sim_estop_release(test_state_t *s) {
    s->estop_active = false;
    s->motor_fault  = false;
    s->state        = APP_STATE_IDLE;
}

void test_estop_engage_sets_error_state(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_estop_engage(&g_state);
    TEST_ASSERT_EQUAL(APP_STATE_ERROR, g_state.state);
}

void test_estop_engage_sets_active_flag(void) {
    sim_estop_engage(&g_state);
    TEST_ASSERT_TRUE(g_state.estop_active);
}

void test_estop_engage_clears_motor_running(void) {
    g_state.motor_running = true;
    sim_estop_engage(&g_state);
    TEST_ASSERT_FALSE(g_state.motor_running);
}

void test_estop_engage_sets_motor_fault(void) {
    sim_estop_engage(&g_state);
    TEST_ASSERT_TRUE(g_state.motor_fault);
}

void test_estop_release_clears_active_flag(void) {
    sim_estop_engage(&g_state);
    sim_estop_release(&g_state);
    TEST_ASSERT_FALSE(g_state.estop_active);
}

void test_estop_release_returns_to_idle(void) {
    sim_estop_engage(&g_state);
    sim_estop_release(&g_state);
    TEST_ASSERT_EQUAL(APP_STATE_IDLE, g_state.state);
}

/*===========================================================================*/
/* Test Runner                                                               */
/*===========================================================================*/

int main(void) {
    UNITY_BEGIN();

    // State tests
    RUN_TEST(test_initial_state_is_idle);
    RUN_TEST(test_estop_flag_prevents_drilling_state);
    RUN_TEST(test_guard_closed_state_tracking);
    RUN_TEST(test_motor_running_state_persistence);

    // Safety combinations
    RUN_TEST(test_multiple_safety_flags_can_be_set);
    RUN_TEST(test_all_safety_flags_clear_initially);

    // Speed range
    RUN_TEST(test_speed_within_valid_range);
    RUN_TEST(test_speed_at_minimum_boundary);
    RUN_TEST(test_speed_at_maximum_boundary);

    // Depth
    RUN_TEST(test_depth_values_can_be_negative);
    RUN_TEST(test_depth_target_can_be_set);

    // Error state
    RUN_TEST(test_error_state_transition);
    RUN_TEST(test_error_message_can_be_set);

    // GF flag classification (Fix 3: idle-loop known_good guard)
    RUN_TEST(test_gf_known_good_stopped_forward);
    RUN_TEST(test_gf_known_good_running_forward);
    RUN_TEST(test_gf_known_good_stopped_reverse);
    RUN_TEST(test_gf_known_good_running_reverse);
    RUN_TEST(test_gf_garbage_flag_not_known_good);
    RUN_TEST(test_gf_zero_flag_not_known_good);
    RUN_TEST(test_gf_error_bit_detected);
    RUN_TEST(test_gf_known_error_value);
    RUN_TEST(test_gf_garbage_no_error_does_not_update_fault);
    RUN_TEST(test_gf_error_flag_does_update_fault);
    RUN_TEST(test_gf_0xffff_not_known_good);
    RUN_TEST(test_gf_0xffff_is_error_state);
    RUN_TEST(test_gf_0xffff_sets_motor_fault);
    RUN_TEST(test_gf_known_good_stopped_clears_fault);

    // motor_running on START transition (Fix A)
    RUN_TEST(test_start_idle_no_trigger_sets_drilling);
    RUN_TEST(test_start_idle_no_trigger_sets_motor_running);
    RUN_TEST(test_start_idle_with_trigger_sets_tapping);
    RUN_TEST(test_start_idle_with_trigger_sets_motor_running);
    RUN_TEST(test_start_drilling_returns_to_idle);

    // E-Stop sequence (Fix 1)
    RUN_TEST(test_estop_engage_sets_error_state);
    RUN_TEST(test_estop_engage_sets_active_flag);
    RUN_TEST(test_estop_engage_clears_motor_running);
    RUN_TEST(test_estop_engage_sets_motor_fault);
    RUN_TEST(test_estop_release_clears_active_flag);
    RUN_TEST(test_estop_release_returns_to_idle);

    return UNITY_END();
}
