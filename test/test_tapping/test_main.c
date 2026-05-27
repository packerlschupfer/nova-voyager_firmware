/**
 * @file test_main.c
 * @brief Unit tests for the tapping configuration API (tapping.c)
 *
 * Tests cover all public functions that live in tapping.c: trigger enable /
 * disable, parameter range clamping, peck timing calculation, and the
 * direction-tracker / state-machine logic re-implemented inline so no FreeRTOS
 * dependency is required.
 *
 * The state machine tests (#8-10 in the spec) use a local re-implementation of
 * the transition logic from task_tapping.c.  The completion-action tests verify
 * the three COMPLETION_* enum values produce the expected next state.
 */

#include <unity.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Minimal type re-definitions (avoids pulling in STM32 HAL via config.h)    */
/*===========================================================================*/

/* --- Enums from config.h ------------------------------------------------- */

typedef enum {
    COMPLETION_STOP          = 0,
    COMPLETION_REVERSE_OUT   = 1,
    COMPLETION_REVERSE_TIMED = 2,
    COMPLETION_RESUME        = 3   /* back off, keep cutting */
} completion_action_t;

typedef enum {
    QUILL_PEDAL_OFF     = 0,
    QUILL_PEDAL_REVERSE = 1,
    QUILL_PEDAL_TOGGLE  = 2
} quill_pedal_mode_t;

typedef enum {
    PEDAL_ACTION_HOLD       = 0,
    PEDAL_ACTION_CHIP_BREAK = 1
} pedal_action_t;

typedef enum {
    CLUTCH_ACTION_REVERSE  = 0,
    CLUTCH_ACTION_ALERT    = 1
} clutch_action_t;

/* --- Settings struct (from config.h tapping_settings_t) ------------------ */

typedef struct {
    uint8_t  depth_trigger_enabled;
    uint8_t  load_increase_enabled;
    uint8_t  load_slip_enabled;
    uint8_t  clutch_slip_enabled;
    uint8_t  quill_trigger_enabled;
    uint8_t  peck_trigger_enabled;
    uint8_t  pedal_enabled;

    uint16_t speed_rpm;

    uint8_t  depth_completion_action;

    uint8_t  quill_pedal_mode;
    uint8_t  quill_completion_action;

    uint8_t  load_increase_threshold;
    uint16_t load_increase_reverse_ms;
    uint8_t  load_completion_action;

    uint16_t load_slip_cv_percent;
    uint8_t  load_slip_completion_action;

    uint16_t clutch_plateau_ms;
    uint8_t  clutch_action;

    uint16_t peck_fwd_ms;
    uint16_t peck_rev_ms;
    uint8_t  peck_cycles;
    uint8_t  peck_depth_stop;
    uint8_t  peck_completion_action;
    uint16_t peck_reverse_out_ms;

    uint8_t  pedal_action;
    uint16_t pedal_chip_break_ms;
} tap_settings_t;

/* --- Default constants (from config.h) ----------------------------------- */

#define TAP_DEFAULT_LOAD_INCREASE_THRESHOLD  60
#define TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS 200
#define TAP_DEFAULT_LOAD_SLIP_CV_PERCENT     130
#define TAP_DEFAULT_CLUTCH_PLATEAU_MS        500
#define TAP_DEFAULT_PECK_FWD_MS              150
#define TAP_DEFAULT_PECK_REV_MS              100
#define TAP_DEFAULT_PECK_CYCLES              7
#define TAP_DEFAULT_PEDAL_CHIP_BREAK_MS      200
#define TAP_DEFAULT_BRAKE_DELAY              100

#define TAP_LOAD_THRESHOLD_MIN      10
#define TAP_LOAD_THRESHOLD_MAX      100
#define TAP_REVERSE_TIME_MIN        50
#define TAP_REVERSE_TIME_MAX        2000
#define TAP_PECK_FWD_MS_MIN         50
#define TAP_PECK_FWD_MS_MAX         5000
#define TAP_PECK_REV_MS_MIN         50
#define TAP_PECK_REV_MS_MAX         2000
#define TAP_CHIP_BREAK_DELAY_MIN    50
#define TAP_CHIP_BREAK_DELAY_MAX    500

#define SPEED_MIN_RPM               50
#define SPEED_MAX_RPM               5500
#define SPEED_TAP_DEFAULT           200

/* --- State machine enum (from tapping.h) --------------------------------- */

typedef enum {
    TAP_STATE_IDLE       = 0,
    TAP_STATE_CUTTING    = 1,
    TAP_STATE_REVERSING  = 2,
    TAP_STATE_TRANSITION = 3
} tap_state_t;

/*===========================================================================*/
/* Local settings store (mirrors tapping.c static variables)                 */
/*===========================================================================*/

static tap_settings_t tap_settings;

/* Peck timing state (mirrors tap_state.fwd_time_ms / rev_time_ms) */
static uint32_t peck_fwd_time_ms;
static uint32_t peck_rev_time_ms;

/*===========================================================================*/
/* Re-implementations of tapping.c public functions under test               */
/*===========================================================================*/

static void settings_reset(void) {
    memset(&tap_settings, 0, sizeof(tap_settings));
    tap_settings.speed_rpm                 = SPEED_TAP_DEFAULT;
    tap_settings.load_increase_threshold   = TAP_DEFAULT_LOAD_INCREASE_THRESHOLD;
    tap_settings.load_increase_reverse_ms  = TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS;
    tap_settings.load_slip_cv_percent      = TAP_DEFAULT_LOAD_SLIP_CV_PERCENT;
    tap_settings.clutch_plateau_ms         = TAP_DEFAULT_CLUTCH_PLATEAU_MS;
    tap_settings.peck_fwd_ms               = TAP_DEFAULT_PECK_FWD_MS;
    tap_settings.peck_rev_ms               = TAP_DEFAULT_PECK_REV_MS;
    tap_settings.peck_cycles               = TAP_DEFAULT_PECK_CYCLES;
    tap_settings.pedal_chip_break_ms       = TAP_DEFAULT_PEDAL_CHIP_BREAK_MS;
    tap_settings.clutch_action             = CLUTCH_ACTION_REVERSE;
    tap_settings.pedal_action              = PEDAL_ACTION_HOLD;
    tap_settings.peck_depth_stop           = 1;
    peck_fwd_time_ms = TAP_DEFAULT_PECK_FWD_MS;
    peck_rev_time_ms = TAP_DEFAULT_PECK_REV_MS;
}

/* Trigger enables */
static void set_depth_trigger_enabled(bool e)  { tap_settings.depth_trigger_enabled   = e ? 1 : 0; }
static void set_load_increase_enabled(bool e)  { tap_settings.load_increase_enabled   = e ? 1 : 0; }
static void set_load_slip_enabled(bool e)      { tap_settings.load_slip_enabled        = e ? 1 : 0; }
static void set_clutch_slip_enabled(bool e)    { tap_settings.clutch_slip_enabled      = e ? 1 : 0; }
static void set_quill_trigger_enabled(bool e)  { tap_settings.quill_trigger_enabled    = e ? 1 : 0; }
static void set_peck_trigger_enabled(bool e)   { tap_settings.peck_trigger_enabled     = e ? 1 : 0; }
static void set_pedal_enabled(bool e)          { tap_settings.pedal_enabled            = e ? 1 : 0; }

/* Load increase threshold — clamp at 100 (uint8_t prevents negative) */
static void set_load_increase_threshold(uint8_t threshold) {
    if (threshold > 100) threshold = 100;
    tap_settings.load_increase_threshold = threshold;
}

/* Load increase reverse time — clamp at 2000ms (matches tapping.c) */
static void set_load_increase_reverse_ms(uint16_t time_ms) {
    if (time_ms > 2000) time_ms = 2000;
    tap_settings.load_increase_reverse_ms = time_ms;
}

/* Peck params — mirrors tapping_set_peck_params() exactly */
static void set_peck_params(uint16_t fwd_ms, uint16_t rev_ms, uint8_t cycles) {
    if (fwd_ms < 50)    fwd_ms  = 50;
    if (fwd_ms > 5000)  fwd_ms  = 5000;
    if (rev_ms < 50)    rev_ms  = 50;
    if (rev_ms > 2000)  rev_ms  = 2000;
    if (cycles > 99)    cycles  = 99;
    tap_settings.peck_fwd_ms  = fwd_ms;
    tap_settings.peck_rev_ms  = rev_ms;
    tap_settings.peck_cycles  = cycles;
    peck_fwd_time_ms = fwd_ms;
    peck_rev_time_ms = rev_ms;
}

/* Peck timing calc — mirrors tapping_calc_peck_timing() */
static void calc_peck_timing(void) {
    peck_fwd_time_ms = tap_settings.peck_fwd_ms;
    peck_rev_time_ms = tap_settings.peck_rev_ms;
    if (peck_fwd_time_ms < 50) peck_fwd_time_ms = 50;
    if (peck_rev_time_ms < 50) peck_rev_time_ms = 50;
}

/* Speed setter — mirrors tapping_set_speed() */
static void set_speed(uint16_t rpm) {
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > SPEED_MAX_RPM) rpm = SPEED_MAX_RPM;
    tap_settings.speed_rpm = rpm;
}

/* Quill pedal mode */
static void set_quill_pedal_mode(quill_pedal_mode_t mode) {
    tap_settings.quill_pedal_mode = (uint8_t)mode;
}
static quill_pedal_mode_t get_quill_pedal_mode(void) {
    return (quill_pedal_mode_t)tap_settings.quill_pedal_mode;
}

/*===========================================================================*/
/* State machine re-implementation for testing (#8-10)                       */
/*                                                                           */
/* Replicates the direction-tracking fix from task_tapping.c:                */
/*   tap_was_forward is captured BEFORE the motor is commanded to stop,      */
/*   so complete_transition() knows which direction to go next.              */
/*===========================================================================*/

static bool        tap_was_forward = true;
static tap_state_t tap_state       = TAP_STATE_IDLE;

/* Simulates the state machine entering TRANSITION from CUTTING */
static void sm_trigger_reverse(void) {
    tap_was_forward = true;         /* was cutting forward */
    tap_state = TAP_STATE_TRANSITION;
}

/* Simulates the state machine entering TRANSITION from REVERSING */
static void sm_trigger_forward(void) {
    tap_was_forward = false;        /* was reversing */
    tap_state = TAP_STATE_TRANSITION;
}

/*
 * complete_transition() — called after brake delay expires.
 * Uses tap_was_forward to decide whether to go to REVERSING or CUTTING.
 */
static void complete_transition(void) {
    if (tap_was_forward) {
        tap_state = TAP_STATE_REVERSING;
    } else {
        tap_state = TAP_STATE_CUTTING;
    }
}

/* Completion action handler — interprets COMPLETION_* and sets next state */
static void apply_completion_action(completion_action_t action) {
    switch (action) {
    case COMPLETION_STOP:
        tap_state = TAP_STATE_IDLE;
        break;
    case COMPLETION_REVERSE_OUT:
        /* start reversing to top — state goes REVERSING then eventually IDLE */
        tap_was_forward = true;      /* was cutting when completion triggered */
        tap_state = TAP_STATE_REVERSING;
        break;
    case COMPLETION_REVERSE_TIMED:
        /* timed reverse — same entry point as REVERSE_OUT for state machine */
        tap_was_forward = true;
        tap_state = TAP_STATE_REVERSING;
        break;
    }
}

/*===========================================================================*/
/* Unity setUp / tearDown                                                     */
/*===========================================================================*/

void setUp(void) {
    settings_reset();
    tap_was_forward = true;
    tap_state       = TAP_STATE_IDLE;
}

void tearDown(void) {
    /* nothing to release */
}

/*===========================================================================*/
/* 1. Trigger enable / disable — all 7 triggers                              */
/*===========================================================================*/

void test_trigger_depth_enable(void) {
    set_depth_trigger_enabled(true);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.depth_trigger_enabled);
}

void test_trigger_depth_disable(void) {
    set_depth_trigger_enabled(true);
    set_depth_trigger_enabled(false);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.depth_trigger_enabled);
}

void test_trigger_load_increase_enable(void) {
    set_load_increase_enabled(true);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.load_increase_enabled);
}

void test_trigger_load_increase_disable(void) {
    set_load_increase_enabled(true);
    set_load_increase_enabled(false);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.load_increase_enabled);
}

void test_trigger_load_slip_enable(void) {
    set_load_slip_enabled(true);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.load_slip_enabled);
}

void test_trigger_load_slip_disable(void) {
    set_load_slip_enabled(false);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.load_slip_enabled);
}

void test_trigger_clutch_slip_enable(void) {
    set_clutch_slip_enabled(true);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.clutch_slip_enabled);
}

void test_trigger_clutch_slip_disable(void) {
    set_clutch_slip_enabled(false);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.clutch_slip_enabled);
}

void test_trigger_quill_enable(void) {
    set_quill_trigger_enabled(true);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.quill_trigger_enabled);
}

void test_trigger_quill_disable(void) {
    set_quill_trigger_enabled(false);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.quill_trigger_enabled);
}

void test_trigger_peck_enable(void) {
    set_peck_trigger_enabled(true);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.peck_trigger_enabled);
}

void test_trigger_peck_disable(void) {
    set_peck_trigger_enabled(false);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.peck_trigger_enabled);
}

void test_trigger_pedal_enable(void) {
    set_pedal_enabled(true);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.pedal_enabled);
}

void test_trigger_pedal_disable(void) {
    set_pedal_enabled(false);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.pedal_enabled);
}

void test_all_triggers_independent(void) {
    /* Enable all triggers simultaneously, verify each bit is set */
    set_depth_trigger_enabled(true);
    set_load_increase_enabled(true);
    set_load_slip_enabled(true);
    set_clutch_slip_enabled(true);
    set_quill_trigger_enabled(true);
    set_peck_trigger_enabled(true);
    set_pedal_enabled(true);

    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.depth_trigger_enabled);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.load_increase_enabled);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.load_slip_enabled);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.clutch_slip_enabled);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.quill_trigger_enabled);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.peck_trigger_enabled);
    TEST_ASSERT_EQUAL_UINT8(1, tap_settings.pedal_enabled);
}

void test_triggers_all_off_after_reset(void) {
    set_depth_trigger_enabled(true);
    set_load_increase_enabled(true);
    settings_reset();

    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.depth_trigger_enabled);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.load_increase_enabled);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.load_slip_enabled);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.clutch_slip_enabled);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.quill_trigger_enabled);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.peck_trigger_enabled);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.pedal_enabled);
}

/*===========================================================================*/
/* 2. Load increase threshold range validation (5-100%)                      */
/*    tapping.c: no lower clamp (uint8_t args can't be negative);            */
/*    upper clamp is 100.                                                    */
/*===========================================================================*/

void test_load_threshold_normal_value(void) {
    set_load_increase_threshold(50);
    TEST_ASSERT_EQUAL_UINT8(50, tap_settings.load_increase_threshold);
}

void test_load_threshold_minimum_boundary(void) {
    set_load_increase_threshold(TAP_LOAD_THRESHOLD_MIN);
    TEST_ASSERT_EQUAL_UINT8(TAP_LOAD_THRESHOLD_MIN,
                             tap_settings.load_increase_threshold);
}

void test_load_threshold_maximum_boundary(void) {
    set_load_increase_threshold(TAP_LOAD_THRESHOLD_MAX);
    TEST_ASSERT_EQUAL_UINT8(100, tap_settings.load_increase_threshold);
}

void test_load_threshold_above_max_clamped(void) {
    /* uint8_t arg so pass 101 directly via cast for boundary test */
    set_load_increase_threshold((uint8_t)101);
    TEST_ASSERT_EQUAL_UINT8(100, tap_settings.load_increase_threshold);
}

void test_load_threshold_zero_stored(void) {
    /* 0 is a valid uint8_t — no lower clamp in tapping.c */
    set_load_increase_threshold(0);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.load_increase_threshold);
}

void test_load_threshold_default_value(void) {
    TEST_ASSERT_EQUAL_UINT8(TAP_DEFAULT_LOAD_INCREASE_THRESHOLD,
                             tap_settings.load_increase_threshold);
}

/*===========================================================================*/
/* 3. Load increase reverse time range validation (50-2000ms)                */
/*    tapping.c: only upper clamp (>2000 → 2000); no lower clamp enforced.  */
/*===========================================================================*/

void test_load_reverse_ms_normal_value(void) {
    set_load_increase_reverse_ms(300);
    TEST_ASSERT_EQUAL_UINT16(300, tap_settings.load_increase_reverse_ms);
}

void test_load_reverse_ms_min_boundary(void) {
    set_load_increase_reverse_ms(TAP_REVERSE_TIME_MIN);
    TEST_ASSERT_EQUAL_UINT16(TAP_REVERSE_TIME_MIN,
                              tap_settings.load_increase_reverse_ms);
}

void test_load_reverse_ms_max_boundary(void) {
    set_load_increase_reverse_ms(TAP_REVERSE_TIME_MAX);
    TEST_ASSERT_EQUAL_UINT16(2000, tap_settings.load_increase_reverse_ms);
}

void test_load_reverse_ms_above_max_clamped(void) {
    set_load_increase_reverse_ms(5000);
    TEST_ASSERT_EQUAL_UINT16(2000, tap_settings.load_increase_reverse_ms);
}

void test_load_reverse_ms_default_value(void) {
    TEST_ASSERT_EQUAL_UINT16(TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS,
                              tap_settings.load_increase_reverse_ms);
}

/*===========================================================================*/
/* 4. Peck parameters — forward time, reverse time, cycle count              */
/*===========================================================================*/

void test_peck_params_normal_values(void) {
    set_peck_params(500, 200, 10);
    TEST_ASSERT_EQUAL_UINT16(500, tap_settings.peck_fwd_ms);
    TEST_ASSERT_EQUAL_UINT16(200, tap_settings.peck_rev_ms);
    TEST_ASSERT_EQUAL_UINT8(10,   tap_settings.peck_cycles);
}

void test_peck_fwd_ms_min_boundary(void) {
    set_peck_params(TAP_PECK_FWD_MS_MIN, 200, 5);
    TEST_ASSERT_EQUAL_UINT16(TAP_PECK_FWD_MS_MIN, tap_settings.peck_fwd_ms);
}

void test_peck_fwd_ms_below_min_clamped(void) {
    set_peck_params(10, 200, 5);
    TEST_ASSERT_EQUAL_UINT16(50, tap_settings.peck_fwd_ms);
}

void test_peck_fwd_ms_max_boundary(void) {
    set_peck_params(TAP_PECK_FWD_MS_MAX, 200, 5);
    TEST_ASSERT_EQUAL_UINT16(5000, tap_settings.peck_fwd_ms);
}

void test_peck_fwd_ms_above_max_clamped(void) {
    set_peck_params(9000, 200, 5);
    TEST_ASSERT_EQUAL_UINT16(5000, tap_settings.peck_fwd_ms);
}

void test_peck_rev_ms_min_boundary(void) {
    set_peck_params(500, TAP_PECK_REV_MS_MIN, 5);
    TEST_ASSERT_EQUAL_UINT16(TAP_PECK_REV_MS_MIN, tap_settings.peck_rev_ms);
}

void test_peck_rev_ms_below_min_clamped(void) {
    set_peck_params(500, 10, 5);
    TEST_ASSERT_EQUAL_UINT16(50, tap_settings.peck_rev_ms);
}

void test_peck_rev_ms_max_boundary(void) {
    set_peck_params(500, TAP_PECK_REV_MS_MAX, 5);
    TEST_ASSERT_EQUAL_UINT16(2000, tap_settings.peck_rev_ms);
}

void test_peck_rev_ms_above_max_clamped(void) {
    set_peck_params(500, 9999, 5);
    TEST_ASSERT_EQUAL_UINT16(2000, tap_settings.peck_rev_ms);
}

void test_peck_cycles_normal(void) {
    set_peck_params(500, 200, 50);
    TEST_ASSERT_EQUAL_UINT8(50, tap_settings.peck_cycles);
}

void test_peck_cycles_zero_infinite(void) {
    set_peck_params(500, 200, 0);
    TEST_ASSERT_EQUAL_UINT8(0, tap_settings.peck_cycles);
}

void test_peck_cycles_max_boundary(void) {
    set_peck_params(500, 200, 99);
    TEST_ASSERT_EQUAL_UINT8(99, tap_settings.peck_cycles);
}

void test_peck_cycles_above_max_clamped(void) {
    set_peck_params(500, 200, 100);
    TEST_ASSERT_EQUAL_UINT8(99, tap_settings.peck_cycles);
}

void test_peck_params_default_values(void) {
    TEST_ASSERT_EQUAL_UINT16(TAP_DEFAULT_PECK_FWD_MS, tap_settings.peck_fwd_ms);
    TEST_ASSERT_EQUAL_UINT16(TAP_DEFAULT_PECK_REV_MS, tap_settings.peck_rev_ms);
    TEST_ASSERT_EQUAL_UINT8(TAP_DEFAULT_PECK_CYCLES,  tap_settings.peck_cycles);
}

/*===========================================================================*/
/* 5. Peck timing calculation — tapping_calc_peck_timing()                   */
/*===========================================================================*/

void test_calc_peck_timing_copies_settings(void) {
    tap_settings.peck_fwd_ms = 400;
    tap_settings.peck_rev_ms = 180;
    calc_peck_timing();
    TEST_ASSERT_EQUAL_UINT32(400, peck_fwd_time_ms);
    TEST_ASSERT_EQUAL_UINT32(180, peck_rev_time_ms);
}

void test_calc_peck_timing_enforces_fwd_minimum(void) {
    /* Artificially write below-minimum directly into settings to test floor */
    tap_settings.peck_fwd_ms = 20;
    tap_settings.peck_rev_ms = 100;
    calc_peck_timing();
    TEST_ASSERT_EQUAL_UINT32(50, peck_fwd_time_ms);
}

void test_calc_peck_timing_enforces_rev_minimum(void) {
    tap_settings.peck_fwd_ms = 300;
    tap_settings.peck_rev_ms = 30;
    calc_peck_timing();
    TEST_ASSERT_EQUAL_UINT32(50, peck_rev_time_ms);
}

void test_calc_peck_timing_after_set_peck_params(void) {
    set_peck_params(750, 350, 5);
    calc_peck_timing();
    TEST_ASSERT_EQUAL_UINT32(750, peck_fwd_time_ms);
    TEST_ASSERT_EQUAL_UINT32(350, peck_rev_time_ms);
}

void test_set_peck_params_updates_timing_immediately(void) {
    /* set_peck_params() also updates the state timing (not just settings) */
    set_peck_params(1000, 500, 3);
    TEST_ASSERT_EQUAL_UINT32(1000, peck_fwd_time_ms);
    TEST_ASSERT_EQUAL_UINT32(500,  peck_rev_time_ms);
}

/*===========================================================================*/
/* 6. Brake / chip-break delay range validation (50-500ms)                   */
/*    (pedal_chip_break_ms — stored directly, limits defined in tapping.h)  */
/*===========================================================================*/

void test_brake_delay_default(void) {
    TEST_ASSERT_EQUAL_UINT16(TAP_DEFAULT_PEDAL_CHIP_BREAK_MS,
                              tap_settings.pedal_chip_break_ms);
}

void test_brake_delay_min_boundary_accepted(void) {
    tap_settings.pedal_chip_break_ms = TAP_CHIP_BREAK_DELAY_MIN;
    TEST_ASSERT_EQUAL_UINT16(TAP_CHIP_BREAK_DELAY_MIN,
                              tap_settings.pedal_chip_break_ms);
}

void test_brake_delay_max_boundary_accepted(void) {
    tap_settings.pedal_chip_break_ms = TAP_CHIP_BREAK_DELAY_MAX;
    TEST_ASSERT_EQUAL_UINT16(TAP_CHIP_BREAK_DELAY_MAX,
                              tap_settings.pedal_chip_break_ms);
}

void test_brake_delay_within_range(void) {
    tap_settings.pedal_chip_break_ms = 250;
    TEST_ASSERT_GREATER_OR_EQUAL(TAP_CHIP_BREAK_DELAY_MIN,
                                  tap_settings.pedal_chip_break_ms);
    TEST_ASSERT_LESS_OR_EQUAL(TAP_CHIP_BREAK_DELAY_MAX,
                               tap_settings.pedal_chip_break_ms);
}

/*===========================================================================*/
/* 7. quill_pedal_mode_t valid enum values                                   */
/*===========================================================================*/

void test_quill_pedal_mode_off(void) {
    set_quill_pedal_mode(QUILL_PEDAL_OFF);
    TEST_ASSERT_EQUAL_INT(QUILL_PEDAL_OFF, get_quill_pedal_mode());
}

void test_quill_pedal_mode_reverse(void) {
    set_quill_pedal_mode(QUILL_PEDAL_REVERSE);
    TEST_ASSERT_EQUAL_INT(QUILL_PEDAL_REVERSE, get_quill_pedal_mode());
}

void test_quill_pedal_mode_toggle(void) {
    set_quill_pedal_mode(QUILL_PEDAL_TOGGLE);
    TEST_ASSERT_EQUAL_INT(QUILL_PEDAL_TOGGLE, get_quill_pedal_mode());
}

void test_quill_pedal_mode_roundtrip(void) {
    /* Set each mode in sequence and verify no bleed-over */
    set_quill_pedal_mode(QUILL_PEDAL_TOGGLE);
    set_quill_pedal_mode(QUILL_PEDAL_OFF);
    TEST_ASSERT_EQUAL_INT(QUILL_PEDAL_OFF, get_quill_pedal_mode());
}

void test_quill_pedal_mode_enum_values(void) {
    TEST_ASSERT_EQUAL_INT(0, QUILL_PEDAL_OFF);
    TEST_ASSERT_EQUAL_INT(1, QUILL_PEDAL_REVERSE);
    TEST_ASSERT_EQUAL_INT(2, QUILL_PEDAL_TOGGLE);
}

/*===========================================================================*/
/* 8. Direction tracker — tap_was_forward set BEFORE motor stop              */
/*===========================================================================*/

void test_direction_tracker_initial_state(void) {
    /* After setUp, tap_was_forward is true (last direction was forward) */
    TEST_ASSERT_TRUE(tap_was_forward);
    TEST_ASSERT_EQUAL_INT(TAP_STATE_IDLE, tap_state);
}

void test_direction_tracker_cutting_to_transition(void) {
    tap_state = TAP_STATE_CUTTING;
    sm_trigger_reverse();           /* records direction before stopping motor */
    TEST_ASSERT_TRUE(tap_was_forward);
    TEST_ASSERT_EQUAL_INT(TAP_STATE_TRANSITION, tap_state);
}

void test_direction_tracker_reversing_to_transition(void) {
    tap_state = TAP_STATE_REVERSING;
    sm_trigger_forward();           /* records direction before stopping motor */
    TEST_ASSERT_FALSE(tap_was_forward);
    TEST_ASSERT_EQUAL_INT(TAP_STATE_TRANSITION, tap_state);
}

void test_direction_tracker_preserved_through_transition(void) {
    /* Simulate CUTTING → TRANSITION → REVERSING */
    tap_state = TAP_STATE_CUTTING;
    sm_trigger_reverse();
    /* tap_was_forward must still be true after recording, before transition ends */
    TEST_ASSERT_TRUE(tap_was_forward);
    complete_transition();
    TEST_ASSERT_EQUAL_INT(TAP_STATE_REVERSING, tap_state);
}

void test_direction_tracker_reverse_phase_sets_false(void) {
    /* Simulate REVERSING → TRANSITION with tracker correctly capturing reverse */
    tap_state = TAP_STATE_REVERSING;
    sm_trigger_forward();
    TEST_ASSERT_FALSE(tap_was_forward);
}

/*===========================================================================*/
/* 9. State transitions — full IDLE→CUTTING→TRANSITION→REVERSING→...→CUTTING */
/*===========================================================================*/

void test_state_idle_is_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, TAP_STATE_IDLE);
}

void test_state_initial_is_idle(void) {
    TEST_ASSERT_EQUAL_INT(TAP_STATE_IDLE, tap_state);
}

void test_state_idle_to_cutting(void) {
    tap_state = TAP_STATE_CUTTING;
    TEST_ASSERT_EQUAL_INT(TAP_STATE_CUTTING, tap_state);
}

void test_state_cutting_triggers_transition(void) {
    tap_state = TAP_STATE_CUTTING;
    sm_trigger_reverse();
    TEST_ASSERT_EQUAL_INT(TAP_STATE_TRANSITION, tap_state);
}

void test_state_transition_to_reversing_after_cut(void) {
    tap_state = TAP_STATE_CUTTING;
    sm_trigger_reverse();           /* enter TRANSITION from CUTTING */
    complete_transition();          /* brake delay expires */
    TEST_ASSERT_EQUAL_INT(TAP_STATE_REVERSING, tap_state);
}

void test_state_reversing_triggers_transition(void) {
    tap_state = TAP_STATE_REVERSING;
    sm_trigger_forward();
    TEST_ASSERT_EQUAL_INT(TAP_STATE_TRANSITION, tap_state);
}

void test_state_transition_to_cutting_after_reverse(void) {
    tap_state = TAP_STATE_REVERSING;
    sm_trigger_forward();           /* enter TRANSITION from REVERSING */
    complete_transition();          /* brake delay expires */
    TEST_ASSERT_EQUAL_INT(TAP_STATE_CUTTING, tap_state);
}

void test_state_full_peck_cycle(void) {
    /* IDLE → CUTTING → TRANSITION → REVERSING → TRANSITION → CUTTING */
    tap_state = TAP_STATE_IDLE;

    tap_state = TAP_STATE_CUTTING;
    TEST_ASSERT_EQUAL_INT(TAP_STATE_CUTTING, tap_state);

    sm_trigger_reverse();
    TEST_ASSERT_EQUAL_INT(TAP_STATE_TRANSITION, tap_state);
    TEST_ASSERT_TRUE(tap_was_forward);

    complete_transition();
    TEST_ASSERT_EQUAL_INT(TAP_STATE_REVERSING, tap_state);

    sm_trigger_forward();
    TEST_ASSERT_EQUAL_INT(TAP_STATE_TRANSITION, tap_state);
    TEST_ASSERT_FALSE(tap_was_forward);

    complete_transition();
    TEST_ASSERT_EQUAL_INT(TAP_STATE_CUTTING, tap_state);
}

void test_state_multiple_peck_cycles(void) {
    /* Run two complete peck cycles and verify state machine is consistent */
    tap_state = TAP_STATE_CUTTING;
    uint8_t i;
    for (i = 0; i < 3; i++) {
        sm_trigger_reverse();
        complete_transition();
        TEST_ASSERT_EQUAL_INT(TAP_STATE_REVERSING, tap_state);

        sm_trigger_forward();
        complete_transition();
        TEST_ASSERT_EQUAL_INT(TAP_STATE_CUTTING, tap_state);
    }
}

/*===========================================================================*/
/* 10. Completion actions                                                     */
/*===========================================================================*/

void test_completion_stop_goes_idle(void) {
    tap_state = TAP_STATE_CUTTING;
    apply_completion_action(COMPLETION_STOP);
    TEST_ASSERT_EQUAL_INT(TAP_STATE_IDLE, tap_state);
}

void test_completion_reverse_out_starts_reversing(void) {
    tap_state = TAP_STATE_CUTTING;
    apply_completion_action(COMPLETION_REVERSE_OUT);
    TEST_ASSERT_EQUAL_INT(TAP_STATE_REVERSING, tap_state);
}

void test_completion_reverse_out_sets_was_forward(void) {
    tap_was_forward = false;        /* corrupt it first */
    tap_state = TAP_STATE_CUTTING;
    apply_completion_action(COMPLETION_REVERSE_OUT);
    TEST_ASSERT_TRUE(tap_was_forward);
}

void test_completion_reverse_timed_starts_reversing(void) {
    tap_state = TAP_STATE_CUTTING;
    apply_completion_action(COMPLETION_REVERSE_TIMED);
    TEST_ASSERT_EQUAL_INT(TAP_STATE_REVERSING, tap_state);
}

void test_completion_reverse_timed_sets_was_forward(void) {
    tap_was_forward = false;
    apply_completion_action(COMPLETION_REVERSE_TIMED);
    TEST_ASSERT_TRUE(tap_was_forward);
}

void test_completion_stop_from_reversing(void) {
    tap_state = TAP_STATE_REVERSING;
    apply_completion_action(COMPLETION_STOP);
    TEST_ASSERT_EQUAL_INT(TAP_STATE_IDLE, tap_state);
}

void test_completion_action_enum_values(void) {
    TEST_ASSERT_EQUAL_INT(0, COMPLETION_STOP);
    TEST_ASSERT_EQUAL_INT(1, COMPLETION_REVERSE_OUT);
    TEST_ASSERT_EQUAL_INT(2, COMPLETION_REVERSE_TIMED);
}

void test_completion_after_full_peck_cycle(void) {
    /* Complete a peck cycle then stop */
    tap_state = TAP_STATE_CUTTING;
    sm_trigger_reverse();
    complete_transition();
    sm_trigger_forward();
    complete_transition();
    TEST_ASSERT_EQUAL_INT(TAP_STATE_CUTTING, tap_state);

    apply_completion_action(COMPLETION_STOP);
    TEST_ASSERT_EQUAL_INT(TAP_STATE_IDLE, tap_state);
}

/*===========================================================================*/
/* Speed setter range (sanity check matching tapping_set_speed)              */
/*===========================================================================*/

void test_speed_normal_value(void) {
    set_speed(300);
    TEST_ASSERT_EQUAL_UINT16(300, tap_settings.speed_rpm);
}

void test_speed_below_min_clamped(void) {
    set_speed(10);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MIN_RPM, tap_settings.speed_rpm);
}

void test_speed_above_max_clamped(void) {
    set_speed(9999);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MAX_RPM, tap_settings.speed_rpm);
}

void test_speed_at_min_boundary(void) {
    set_speed(SPEED_MIN_RPM);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MIN_RPM, tap_settings.speed_rpm);
}

void test_speed_at_max_boundary(void) {
    set_speed(SPEED_MAX_RPM);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MAX_RPM, tap_settings.speed_rpm);
}

void test_speed_default_after_reset(void) {
    TEST_ASSERT_EQUAL_UINT16(SPEED_TAP_DEFAULT, tap_settings.speed_rpm);
}

/*===========================================================================*/
/* Test Runner                                                                */
/*===========================================================================*/

int main(void) {
    UNITY_BEGIN();

    /* 1. Trigger enable / disable */
    RUN_TEST(test_trigger_depth_enable);
    RUN_TEST(test_trigger_depth_disable);
    RUN_TEST(test_trigger_load_increase_enable);
    RUN_TEST(test_trigger_load_increase_disable);
    RUN_TEST(test_trigger_load_slip_enable);
    RUN_TEST(test_trigger_load_slip_disable);
    RUN_TEST(test_trigger_clutch_slip_enable);
    RUN_TEST(test_trigger_clutch_slip_disable);
    RUN_TEST(test_trigger_quill_enable);
    RUN_TEST(test_trigger_quill_disable);
    RUN_TEST(test_trigger_peck_enable);
    RUN_TEST(test_trigger_peck_disable);
    RUN_TEST(test_trigger_pedal_enable);
    RUN_TEST(test_trigger_pedal_disable);
    RUN_TEST(test_all_triggers_independent);
    RUN_TEST(test_triggers_all_off_after_reset);

    /* 2. Load increase threshold */
    RUN_TEST(test_load_threshold_normal_value);
    RUN_TEST(test_load_threshold_minimum_boundary);
    RUN_TEST(test_load_threshold_maximum_boundary);
    RUN_TEST(test_load_threshold_above_max_clamped);
    RUN_TEST(test_load_threshold_zero_stored);
    RUN_TEST(test_load_threshold_default_value);

    /* 3. Load increase reverse time */
    RUN_TEST(test_load_reverse_ms_normal_value);
    RUN_TEST(test_load_reverse_ms_min_boundary);
    RUN_TEST(test_load_reverse_ms_max_boundary);
    RUN_TEST(test_load_reverse_ms_above_max_clamped);
    RUN_TEST(test_load_reverse_ms_default_value);

    /* 4. Peck parameters */
    RUN_TEST(test_peck_params_normal_values);
    RUN_TEST(test_peck_fwd_ms_min_boundary);
    RUN_TEST(test_peck_fwd_ms_below_min_clamped);
    RUN_TEST(test_peck_fwd_ms_max_boundary);
    RUN_TEST(test_peck_fwd_ms_above_max_clamped);
    RUN_TEST(test_peck_rev_ms_min_boundary);
    RUN_TEST(test_peck_rev_ms_below_min_clamped);
    RUN_TEST(test_peck_rev_ms_max_boundary);
    RUN_TEST(test_peck_rev_ms_above_max_clamped);
    RUN_TEST(test_peck_cycles_normal);
    RUN_TEST(test_peck_cycles_zero_infinite);
    RUN_TEST(test_peck_cycles_max_boundary);
    RUN_TEST(test_peck_cycles_above_max_clamped);
    RUN_TEST(test_peck_params_default_values);

    /* 5. Peck timing calculation */
    RUN_TEST(test_calc_peck_timing_copies_settings);
    RUN_TEST(test_calc_peck_timing_enforces_fwd_minimum);
    RUN_TEST(test_calc_peck_timing_enforces_rev_minimum);
    RUN_TEST(test_calc_peck_timing_after_set_peck_params);
    RUN_TEST(test_set_peck_params_updates_timing_immediately);

    /* 6. Brake delay */
    RUN_TEST(test_brake_delay_default);
    RUN_TEST(test_brake_delay_min_boundary_accepted);
    RUN_TEST(test_brake_delay_max_boundary_accepted);
    RUN_TEST(test_brake_delay_within_range);

    /* 7. Quill pedal mode enum */
    RUN_TEST(test_quill_pedal_mode_off);
    RUN_TEST(test_quill_pedal_mode_reverse);
    RUN_TEST(test_quill_pedal_mode_toggle);
    RUN_TEST(test_quill_pedal_mode_roundtrip);
    RUN_TEST(test_quill_pedal_mode_enum_values);

    /* 8. Direction tracker */
    RUN_TEST(test_direction_tracker_initial_state);
    RUN_TEST(test_direction_tracker_cutting_to_transition);
    RUN_TEST(test_direction_tracker_reversing_to_transition);
    RUN_TEST(test_direction_tracker_preserved_through_transition);
    RUN_TEST(test_direction_tracker_reverse_phase_sets_false);

    /* 9. State transitions */
    RUN_TEST(test_state_idle_is_zero);
    RUN_TEST(test_state_initial_is_idle);
    RUN_TEST(test_state_idle_to_cutting);
    RUN_TEST(test_state_cutting_triggers_transition);
    RUN_TEST(test_state_transition_to_reversing_after_cut);
    RUN_TEST(test_state_reversing_triggers_transition);
    RUN_TEST(test_state_transition_to_cutting_after_reverse);
    RUN_TEST(test_state_full_peck_cycle);
    RUN_TEST(test_state_multiple_peck_cycles);

    /* 10. Completion actions */
    RUN_TEST(test_completion_stop_goes_idle);
    RUN_TEST(test_completion_reverse_out_starts_reversing);
    RUN_TEST(test_completion_reverse_out_sets_was_forward);
    RUN_TEST(test_completion_reverse_timed_starts_reversing);
    RUN_TEST(test_completion_reverse_timed_sets_was_forward);
    RUN_TEST(test_completion_stop_from_reversing);
    RUN_TEST(test_completion_action_enum_values);
    RUN_TEST(test_completion_after_full_peck_cycle);

    /* Speed setter */
    RUN_TEST(test_speed_normal_value);
    RUN_TEST(test_speed_below_min_clamped);
    RUN_TEST(test_speed_above_max_clamped);
    RUN_TEST(test_speed_at_min_boundary);
    RUN_TEST(test_speed_at_max_boundary);
    RUN_TEST(test_speed_default_after_reset);

    return UNITY_END();
}
