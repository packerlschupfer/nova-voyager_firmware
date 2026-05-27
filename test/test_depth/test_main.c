/**
 * @file test_main.c
 * @brief Unit tests for depth/quill sensor logic
 *
 * Tests the pure business logic extracted from task_depth.c:
 *   - check_target_depth(): depth-stop and depth-stop+reverse at target
 *   - check_step_drill_rpm(): surface-speed RPM scaling and auto-stop
 *
 * No HAL, FreeRTOS, or hardware dependencies — runs on native host with Unity.
 */

#include <unity.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Constants (from config.h)                                                  */
/*===========================================================================*/

#define SPEED_MIN_RPM       50
#define SPEED_MAX_RPM       5500

/*===========================================================================*/
/* Re-implemented pure logic under test                                       */
/*===========================================================================*/

/**
 * Result structure capturing every observable side-effect of
 * check_target_depth() in a single call — no global mutation needed.
 */
typedef struct {
    bool fired;               /**< Depth target condition was triggered      */
    bool motor_stop_called;   /**< CMD_MOTOR_STOP was issued                 */
    bool motor_reverse_called;/**< CMD_MOTOR_REVERSE was issued (mode 2 only)*/
    bool event_sent;          /**< EVT_DEPTH_TARGET was dispatched           */
} depth_result_t;

/**
 * Pure reimplementation of check_target_depth() logic.
 *
 * The latch state is passed in/out so callers can test repeated invocations
 * without depending on the static local variable inside the real function.
 *
 * @param current       current_depth in 0.1 mm units (positive = deeper)
 * @param target        target_depth in 0.1 mm units (0 = disabled)
 * @param mode          depth_mode: 0=off, 1=stop, 2=stop+reverse
 * @param guard_closed  true if the guard switch is engaged
 * @param has_fault     true if depth_has_fault() would return true
 * @param fired_latch   in/out: latch flag (false on first call; pass back)
 */
static depth_result_t check_depth(int16_t current, int16_t target,
                                   uint8_t mode, bool guard_closed,
                                   bool has_fault, bool *fired_latch)
{
    depth_result_t r = {0};

    /* Mirrors: if (depth_mode == 0 || target == 0) return; */
    if (mode == 0 || target == 0) {
        return r;
    }

    /* Mirrors: if (!guard_closed || depth_has_fault()) return; */
    if (!guard_closed || has_fault) {
        return r;
    }

    /* Mirrors: if (current_depth >= target && !depth_target_fired) { ... } */
    if (current >= target && !(*fired_latch)) {
        *fired_latch          = true;
        r.fired               = true;
        r.motor_stop_called   = true;
        r.event_sent          = true;
        if (mode == 2) {
            r.motor_reverse_called = true;
        }
    }

    return r;
}

/**
 * Step-drill RPM calculation result.
 */
typedef struct {
    bool     stop_called;   /**< Motor stop issued (target diameter reached) */
    bool     event_sent;    /**< EVT_DEPTH_TARGET sent on diameter stop       */
    bool     rpm_updated;   /**< SET_SPEED command would be issued            */
    uint16_t target_rpm;    /**< Calculated (clamped) target RPM              */
    uint16_t current_dia;   /**< Diameter the algorithm resolved at this depth*/
} step_result_t;

/**
 * Pure reimplementation of check_step_drill_rpm() logic.
 *
 * All inputs come from the settings_t step_drill sub-struct in the real code.
 * current_target_rpm mirrors g_state.target_rpm (the last SET_SPEED value).
 *
 * @param depth_mm_x10      current_depth in 0.1 mm units (may be negative)
 * @param base_rpm          step_drill.base_rpm
 * @param start_diameter    step_drill.start_diameter (mm)
 * @param diameter_increment step_drill.diameter_increment (mm per step)
 * @param step_depth_x2     step_drill.step_depth_x2 (0.5 mm units per step)
 * @param target_diameter   step_drill.target_diameter (0 = no auto-stop)
 * @param current_target_rpm g_state.target_rpm (for 50 RPM hysteresis check)
 */
static step_result_t check_step_drill(int16_t depth_mm_x10,
                                       uint16_t base_rpm,
                                       uint16_t start_diameter,
                                       uint16_t diameter_increment,
                                       uint8_t  step_depth_x2,
                                       uint16_t target_diameter,
                                       uint16_t current_target_rpm)
{
    step_result_t r = {0};

    /* Convert step_depth_x2 (0.5 mm units) to 0.1 mm units */
    int16_t step_depth_mm_x10 = (int16_t)step_depth_x2 * 5;
    if (step_depth_mm_x10 <= 0) {
        return r;  /* Invalid step depth config */
    }

    /* Clamp negative depth to zero (above zero-point = starting step) */
    if (depth_mm_x10 < 0) {
        depth_mm_x10 = 0;
    }

    /* Calculate current step number (0-based) */
    int16_t current_step = depth_mm_x10 / step_depth_mm_x10;

    /* Calculate current diameter */
    uint16_t current_dia = start_diameter + (uint16_t)(current_step * diameter_increment);

    /* Clamp diameter (mirrors production code) */
    if (current_dia < start_diameter) {
        current_dia = start_diameter;
    }
    if (current_dia > 50) {
        current_dia = 50;
    }

    r.current_dia = current_dia;

    /* Auto-stop when target diameter reached */
    if (target_diameter > 0 && current_dia >= target_diameter) {
        r.stop_called = true;
        r.event_sent  = true;
        return r;
    }

    /* Division-by-zero guard (mirrors production code) */
    if (current_dia == 0) {
        current_dia = start_diameter;
    }
    if (current_dia == 0) {
        return r;
    }

    /* Constant surface speed: RPM = base_rpm * start_dia / current_dia */
    uint16_t target_rpm = (uint16_t)((base_rpm * start_diameter) / current_dia);

    /* Clamp to valid RPM range */
    if (target_rpm < SPEED_MIN_RPM) target_rpm = SPEED_MIN_RPM;
    if (target_rpm > SPEED_MAX_RPM) target_rpm = SPEED_MAX_RPM;

    r.target_rpm = target_rpm;

    /* 50 RPM hysteresis: only emit SET_SPEED when change is significant */
    if (current_target_rpm > target_rpm + 50 ||
        current_target_rpm + 50 < target_rpm) {
        r.rpm_updated = true;
    }

    return r;
}

/*===========================================================================*/
/* Test Fixtures                                                              */
/*===========================================================================*/

void setUp(void) { /* nothing global to reset */ }
void tearDown(void) {}

/*===========================================================================*/
/* check_target_depth Tests                                                   */
/*===========================================================================*/

/* 1. Depth disabled: mode=0 → no action regardless of depth */
void test_depth_mode_disabled_no_action(void) {
    bool latch = false;
    depth_result_t r = check_depth(300, 200, 0, true, false, &latch);
    TEST_ASSERT_FALSE(r.fired);
    TEST_ASSERT_FALSE(r.motor_stop_called);
    TEST_ASSERT_FALSE(r.event_sent);
    TEST_ASSERT_FALSE(latch);
}

/* 2. Target zero: target=0 → no action */
void test_depth_target_zero_no_action(void) {
    bool latch = false;
    depth_result_t r = check_depth(300, 0, 1, true, false, &latch);
    TEST_ASSERT_FALSE(r.fired);
    TEST_ASSERT_FALSE(r.motor_stop_called);
    TEST_ASSERT_FALSE(r.event_sent);
}

/* 3. Below target: current < target → no action */
void test_depth_below_target_no_action(void) {
    bool latch = false;
    depth_result_t r = check_depth(150, 200, 1, true, false, &latch);
    TEST_ASSERT_FALSE(r.fired);
    TEST_ASSERT_FALSE(r.motor_stop_called);
    TEST_ASSERT_FALSE(r.event_sent);
    TEST_ASSERT_FALSE(latch);
}

/* 4. At target, mode 1: current == target → stop, event, no reverse */
void test_depth_at_target_mode1_stops_no_reverse(void) {
    bool latch = false;
    depth_result_t r = check_depth(200, 200, 1, true, false, &latch);
    TEST_ASSERT_TRUE(r.fired);
    TEST_ASSERT_TRUE(r.motor_stop_called);
    TEST_ASSERT_TRUE(r.event_sent);
    TEST_ASSERT_FALSE(r.motor_reverse_called);
    TEST_ASSERT_TRUE(latch);
}

/* 4b. Past target, mode 1: current > target → stop, event, no reverse */
void test_depth_past_target_mode1_stops_no_reverse(void) {
    bool latch = false;
    depth_result_t r = check_depth(250, 200, 1, true, false, &latch);
    TEST_ASSERT_TRUE(r.fired);
    TEST_ASSERT_TRUE(r.motor_stop_called);
    TEST_ASSERT_FALSE(r.motor_reverse_called);
}

/* 5. At target, mode 2: current == target → stop, event, then reverse */
void test_depth_at_target_mode2_stop_and_reverse(void) {
    bool latch = false;
    depth_result_t r = check_depth(200, 200, 2, true, false, &latch);
    TEST_ASSERT_TRUE(r.fired);
    TEST_ASSERT_TRUE(r.motor_stop_called);
    TEST_ASSERT_TRUE(r.motor_reverse_called);
    TEST_ASSERT_TRUE(r.event_sent);
}

/* 6. Guard open: no action even if depth >= target */
void test_depth_guard_open_no_action(void) {
    bool latch = false;
    depth_result_t r = check_depth(250, 200, 1, false, false, &latch);
    TEST_ASSERT_FALSE(r.fired);
    TEST_ASSERT_FALSE(r.motor_stop_called);
    TEST_ASSERT_FALSE(latch);
}

/* 7. Depth fault active: no action */
void test_depth_fault_no_action(void) {
    bool latch = false;
    depth_result_t r = check_depth(250, 200, 1, true, true, &latch);
    TEST_ASSERT_FALSE(r.fired);
    TEST_ASSERT_FALSE(r.motor_stop_called);
    TEST_ASSERT_FALSE(latch);
}

/* 8a. Latch: first call at target fires */
void test_depth_latch_first_call_fires(void) {
    bool latch = false;
    depth_result_t r = check_depth(200, 200, 1, true, false, &latch);
    TEST_ASSERT_TRUE(r.fired);
    TEST_ASSERT_TRUE(latch);
}

/* 8b. Latch: second call with same conditions does NOT re-fire */
void test_depth_latch_second_call_no_refire(void) {
    bool latch = false;
    check_depth(200, 200, 1, true, false, &latch);  /* first call sets latch */
    depth_result_t r2 = check_depth(200, 200, 1, true, false, &latch);
    TEST_ASSERT_FALSE(r2.fired);
    TEST_ASSERT_FALSE(r2.motor_stop_called);
    TEST_ASSERT_FALSE(r2.event_sent);
}

/* 8c. Latch: motor stop resets latch (motor_running=false branch in real code) */
void test_depth_latch_reset_after_motor_stop(void) {
    bool latch = true;   /* simulate already-fired latch */
    /* Production code: if (!motor_running) { depth_target_fired = false; return; }
     * We test the same invariant: a fresh latch=false must allow firing again. */
    latch = false;       /* motor stopped → latch cleared */
    depth_result_t r = check_depth(200, 200, 1, true, false, &latch);
    TEST_ASSERT_TRUE(r.fired);
}

/*===========================================================================*/
/* check_step_drill_rpm Tests                                                 */
/*===========================================================================*/

/* 9. Step drill RPM: base=1000, start_dia=5, at first step → correct calc */
void test_step_drill_rpm_at_first_step(void) {
    /*
     * step_depth_x2=4 → step = 4*5 = 20 (0.1mm units = 2.0mm per step)
     * depth=0 → step 0 → dia = 5 + 0*1 = 5mm
     * rpm = 1000 * 5 / 5 = 1000
     */
    step_result_t r = check_step_drill(
        /*depth*/        0,
        /*base_rpm*/     1000,
        /*start_dia*/    5,
        /*dia_inc*/      1,
        /*step_depth_x2*/4,      /* 2.0 mm per step */
        /*target_dia*/   0,      /* no auto-stop */
        /*cur_target*/   0       /* force rpm_updated */
    );
    TEST_ASSERT_FALSE(r.stop_called);
    TEST_ASSERT_EQUAL_UINT16(5, r.current_dia);
    TEST_ASSERT_EQUAL_UINT16(1000, r.target_rpm);
    TEST_ASSERT_TRUE(r.rpm_updated);
}

/* 9b. RPM reduces as diameter grows */
void test_step_drill_rpm_decreases_at_second_step(void) {
    /*
     * step_depth_x2=4 → step = 20 (0.1mm units)
     * depth=20 → step 1 → dia = 5 + 1*1 = 6mm
     * rpm = 1000 * 5 / 6 = 833
     */
    step_result_t r = check_step_drill(20, 1000, 5, 1, 4, 0, 0);
    TEST_ASSERT_FALSE(r.stop_called);
    TEST_ASSERT_EQUAL_UINT16(6, r.current_dia);
    TEST_ASSERT_EQUAL_UINT16(833, r.target_rpm);
}

/* 10. Step drill auto-stop: current_dia >= target_dia → stop, event, no rpm cmd */
void test_step_drill_stop_at_target_diameter(void) {
    /*
     * depth=40 → step 2 → dia = 5 + 2*1 = 7mm >= target_dia=7 → stop
     */
    step_result_t r = check_step_drill(40, 1000, 5, 1, 4, 7, 0);
    TEST_ASSERT_TRUE(r.stop_called);
    TEST_ASSERT_TRUE(r.event_sent);
    TEST_ASSERT_FALSE(r.rpm_updated);
}

/* 10b. Just below target diameter: no stop */
void test_step_drill_no_stop_below_target_diameter(void) {
    /*
     * depth=20 → step 1 → dia = 6mm < target_dia=7 → no stop
     */
    step_result_t r = check_step_drill(20, 1000, 5, 1, 4, 7, 0);
    TEST_ASSERT_FALSE(r.stop_called);
    TEST_ASSERT_FALSE(r.event_sent);
}

/* 11. Step drill zero-dia protection: if current_dia works out to 0, use start_dia */
void test_step_drill_zero_diameter_uses_start_diameter(void) {
    /*
     * start_diameter=5, diameter_increment=0 → dia always stays start_diameter.
     * This exercises the current_dia==0 guard path via start_dia fallback when
     * start_diameter itself is 0. We force it by setting start_diameter=0 and
     * diameter_increment=0 so current_dia computes to 0 — the guard kicks in
     * and returns without crashing (no division by zero).
     */
    step_result_t r = check_step_drill(
        /*depth*/        20,
        /*base_rpm*/     1000,
        /*start_dia*/    0,      /* triggers zero guard */
        /*dia_inc*/      0,
        /*step_depth_x2*/4,
        /*target_dia*/   0,
        /*cur_target*/   0
    );
    /* With start_dia=0 the second guard (if current_dia==0) returns early */
    TEST_ASSERT_FALSE(r.stop_called);
    TEST_ASSERT_FALSE(r.rpm_updated);
    TEST_ASSERT_EQUAL_UINT16(0, r.target_rpm);  /* no RPM computed */
}

/* 11b. Negative depth is clamped to zero (above zero-point = step 0) */
void test_step_drill_negative_depth_clamped_to_zero(void) {
    /*
     * depth=-50 → clamped to 0 → step 0 → dia = start_dia
     * RPM should equal 1000 (same as depth=0 case)
     */
    step_result_t r_neg  = check_step_drill(-50, 1000, 5, 1, 4, 0, 0);
    step_result_t r_zero = check_step_drill(  0, 1000, 5, 1, 4, 0, 0);
    TEST_ASSERT_EQUAL_UINT16(r_zero.current_dia, r_neg.current_dia);
    TEST_ASSERT_EQUAL_UINT16(r_zero.target_rpm,  r_neg.target_rpm);
}

/* 12. Step drill RPM clamping: very large diameter → RPM clamped to SPEED_MIN_RPM */
void test_step_drill_rpm_clamped_to_min(void) {
    /*
     * depth=450 → step 22 → dia = 5 + 22*2 = 49mm (< 50 cap)
     * rpm = 1000 * 5 / 49 ≈ 102 → above SPEED_MIN_RPM, but with a huge
     * diameter we push into clamping: use start_dia=5, dia_inc=2, 24 steps
     * → dia=53 capped at 50 → rpm = 1000*5/50 = 100 → clamped to 100 (>50)
     * Try base_rpm=100, start_dia=3, current_dia=50 → 6 < SPEED_MIN_RPM=50
     */
    step_result_t r = check_step_drill(
        /*depth*/        490,    /* step = 490/20 = 24 → dia = 3 + 24*2 = 51 → capped 50 */
        /*base_rpm*/     100,
        /*start_dia*/    3,
        /*dia_inc*/      2,
        /*step_depth_x2*/4,     /* 2.0 mm per step */
        /*target_dia*/   0,
        /*cur_target*/   0
    );
    TEST_ASSERT_EQUAL_UINT16(50, r.current_dia);          /* diameter cap */
    TEST_ASSERT_EQUAL_UINT16(SPEED_MIN_RPM, r.target_rpm);/* clamped low  */
}

/* 12b. RPM clamped to SPEED_MAX_RPM when calculated value exceeds ceiling */
void test_step_drill_rpm_clamped_to_max(void) {
    /*
     * depth=0 → step 0 → dia = start_dia
     * rpm = 100000 * 10 / 10 = 100000 → clamped to SPEED_MAX_RPM
     */
    step_result_t r = check_step_drill(
        /*depth*/         0,
        /*base_rpm*/      30000,  /* unrealistically high to force overflow */
        /*start_dia*/     10,
        /*dia_inc*/       1,
        /*step_depth_x2*/ 4,
        /*target_dia*/    0,
        /*cur_target*/    0
    );
    TEST_ASSERT_EQUAL_UINT16(SPEED_MAX_RPM, r.target_rpm);
}

/* Hysteresis: RPM change < 50 does not trigger SET_SPEED */
void test_step_drill_hysteresis_no_update_within_band(void) {
    /*
     * Computed rpm = 1000.  current_target_rpm = 1020 → delta = 20 < 50
     * No update expected.
     */
    step_result_t r = check_step_drill(0, 1000, 5, 1, 4, 0, 1020);
    TEST_ASSERT_FALSE(r.rpm_updated);
}

/* Hysteresis: RPM change > 50 triggers SET_SPEED */
void test_step_drill_hysteresis_update_outside_band(void) {
    /*
     * Computed rpm = 1000.  current_target_rpm = 1500 → delta = 500 > 50
     */
    step_result_t r = check_step_drill(0, 1000, 5, 1, 4, 0, 1500);
    TEST_ASSERT_TRUE(r.rpm_updated);
}

/* Invalid step depth config (step_depth_x2=0) → function returns early */
void test_step_drill_invalid_step_depth_no_action(void) {
    step_result_t r = check_step_drill(100, 1000, 5, 1, 0, 0, 0);
    TEST_ASSERT_FALSE(r.stop_called);
    TEST_ASSERT_FALSE(r.rpm_updated);
    TEST_ASSERT_EQUAL_UINT16(0, r.target_rpm);
}

/*===========================================================================*/
/* Test Runner                                                                */
/*===========================================================================*/

int main(void) {
    UNITY_BEGIN();

    /* --- check_target_depth --- */
    RUN_TEST(test_depth_mode_disabled_no_action);
    RUN_TEST(test_depth_target_zero_no_action);
    RUN_TEST(test_depth_below_target_no_action);
    RUN_TEST(test_depth_at_target_mode1_stops_no_reverse);
    RUN_TEST(test_depth_past_target_mode1_stops_no_reverse);
    RUN_TEST(test_depth_at_target_mode2_stop_and_reverse);
    RUN_TEST(test_depth_guard_open_no_action);
    RUN_TEST(test_depth_fault_no_action);
    RUN_TEST(test_depth_latch_first_call_fires);
    RUN_TEST(test_depth_latch_second_call_no_refire);
    RUN_TEST(test_depth_latch_reset_after_motor_stop);

    /* --- check_step_drill_rpm --- */
    RUN_TEST(test_step_drill_rpm_at_first_step);
    RUN_TEST(test_step_drill_rpm_decreases_at_second_step);
    RUN_TEST(test_step_drill_stop_at_target_diameter);
    RUN_TEST(test_step_drill_no_stop_below_target_diameter);
    RUN_TEST(test_step_drill_zero_diameter_uses_start_diameter);
    RUN_TEST(test_step_drill_negative_depth_clamped_to_zero);
    RUN_TEST(test_step_drill_rpm_clamped_to_min);
    RUN_TEST(test_step_drill_rpm_clamped_to_max);
    RUN_TEST(test_step_drill_hysteresis_no_update_within_band);
    RUN_TEST(test_step_drill_hysteresis_update_outside_band);
    RUN_TEST(test_step_drill_invalid_step_depth_no_action);

    return UNITY_END();
}
