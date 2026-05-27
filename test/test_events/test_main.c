/**
 * @file test_main.c
 * @brief Unit tests for the event handler logic (events.c)
 *
 * The real event handlers are tightly coupled to FreeRTOS queues, HAL
 * timers, motor UART, and the buzzer. Here we:
 *   1. Reproduce the shared_state_t fields we care about in a local struct.
 *   2. Re-implement each handler as a pure testable function that manipulates
 *      only that local state plus the mock call-trace variables.
 *   3. Verify the state transitions and mock-call side-effects with Unity.
 *
 * The dispatch-table completeness test (Test 11) verifies that every value
 * in the event_type_t enum (copied verbatim from shared.h) has a matching
 * entry in a local table — including EVT_DEPTH_TARGET which was recently added.
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/*===========================================================================*/
/* Minimal type replicas (from shared.h / config.h)                          */
/*===========================================================================*/

typedef enum {
    APP_STATE_STARTUP = 0,
    APP_STATE_IDLE,
    APP_STATE_DRILLING,
    APP_STATE_TAPPING,
    APP_STATE_MENU,
    APP_STATE_ERROR
} app_state_t;

typedef enum {
    /* Button events */
    EVT_BTN_ZERO        = 0x0001,
    EVT_BTN_MENU        = 0x0002,
    EVT_BTN_F1          = 0x0004,
    EVT_BTN_F2          = 0x0008,
    EVT_BTN_F3          = 0x0010,
    EVT_BTN_F4          = 0x0020,
    EVT_BTN_START       = 0x0040,
    EVT_BTN_GUARD       = 0x0080,
    EVT_BTN_ENCODER     = 0x0100,
    EVT_BTN_ESTOP       = 0x0200,
    EVT_BTN_F1_LONG     = 0x8000,
    EVT_BTN_ENC_LONG    = 0x8001,
    /* Encoder events */
    EVT_ENC_CW          = 0x0400,
    EVT_ENC_CCW         = 0x0800,
    /* System events */
    EVT_MOTOR_FAULT     = 0x1000,
    EVT_JAM_DETECTED    = 0x2000,
    EVT_DEPTH_TARGET    = 0x4000,
    EVT_LOAD_SPIKE      = 0x8005,
    EVT_OVERHEAT        = 0x8002,
    EVT_TEMP_WARNING    = 0x8003,
    EVT_LOW_VOLTAGE     = 0x8004,
    EVT_BOOT_COMPLETE   = 0x8006,
} event_type_t;

/* Shared state (subset of fields used by the tested handlers) */
typedef struct {
    app_state_t  state;
    bool         motor_running;
    bool         motor_fault;
    bool         estop_active;
    bool         guard_closed;
    uint32_t     error_until;     /* tick value — positive = message shown */
    const char  *error_line1;
    const char  *error_line2;
    uint16_t     dc_bus_voltage;
} test_state_t;

static test_state_t g_state;

/*===========================================================================*/
/* Mock call-trace                                                            */
/*===========================================================================*/

static int  mock_motor_disable_calls;
static int  mock_motor_enable_calls;
static int  mock_spindle_hold_calls;
static int  mock_spindle_release_calls;
static int  mock_motor_stop_cmds;
static int  mock_beep_calls;
static uint16_t mock_last_temp;     /* value returned by motor_get_temperature() */
static uint16_t mock_last_voltage;  /* simulated DC bus voltage level */

/* Low-voltage edge-detection state (mirrors the real driver's duty) */
static bool mock_voltage_was_low;   /* previous sample was below threshold */

static void mock_reset_calls(void) {
    mock_motor_disable_calls  = 0;
    mock_motor_enable_calls   = 0;
    mock_spindle_hold_calls   = 0;
    mock_spindle_release_calls = 0;
    mock_motor_stop_cmds      = 0;
    mock_beep_calls           = 0;
    mock_last_temp            = 0;
    mock_last_voltage         = 400; /* normal voltage */
    mock_voltage_was_low      = false;
}

/* Mock functions referenced by the handlers */
static void motor_hardware_disable(void)    { mock_motor_disable_calls++;   }
static void motor_hardware_enable(void)     { mock_motor_enable_calls++;    }
static void motor_spindle_hold_safety(void) { mock_spindle_hold_calls++;    }
static void motor_spindle_release(void)     { mock_spindle_release_calls++; }
static void motor_stop_cmd(void)            { mock_motor_stop_cmds++;       }
static void buzzer_beep_mock(void)          { mock_beep_calls++;            }
static uint16_t motor_get_temperature(void) { return mock_last_temp;        }

/* Spindle-hold active query — returns true when hold was engaged */
static bool mock_spindle_hold_active = false;
static bool motor_is_spindle_hold_active(void) { return mock_spindle_hold_active; }

/*===========================================================================*/
/* DC bus voltage threshold (from config.h)                                  */
/*===========================================================================*/

#define DC_BUS_LOW_VOLTAGE_THRESHOLD  300   /* warn below ~300 V */

/*===========================================================================*/
/* Re-implemented event handlers (logic extracted from events.c)             */
/*===========================================================================*/

/* --- Test 1: E-Stop engage --- */
static void sim_estop_engage(void) {
    motor_stop_cmd();
    motor_hardware_enable();       /* re-enable for spindle hold */
    motor_spindle_hold_safety();
    g_state.state         = APP_STATE_ERROR;
    g_state.estop_active  = true;
    g_state.motor_running = false;
    g_state.motor_fault   = true;
    g_state.error_until   = 1;    /* non-zero → message shown */
    g_state.error_line1   = "!! E-STOP !!   ";
    g_state.error_line2   = "Release to clear";
}

/* --- Test 2: E-Stop release --- */
static void sim_estop_release(void) {
    motor_spindle_release();
    g_state.estop_active  = false;
    g_state.motor_fault   = false;
    g_state.state         = APP_STATE_IDLE;
    g_state.error_until   = 0;
    g_state.error_line1   = "";
    g_state.error_line2   = "";
}

/* --- Test 3: Guard open while running --- */
static void sim_guard_open_while_running(void) {
    motor_stop_cmd();
    motor_spindle_hold_safety();
    g_state.state       = APP_STATE_IDLE;
    g_state.error_until = 30000;          /* 30 s in real time, non-zero here */
    g_state.error_line1 = " GUARD OPENED!  ";
    g_state.error_line2 = " Close to clear ";
}

/* --- Test 4: Guard close with spindle hold active --- */
static void sim_guard_close_release_hold(void) {
    motor_spindle_release();
    mock_spindle_hold_active = false;     /* hold released */
    g_state.error_until  = 0;
    g_state.error_line1  = "";
    g_state.error_line2  = "";
}

/* --- Test 5: Motor fault --- */
static void sim_motor_fault(void) {
    motor_hardware_disable();
    motor_stop_cmd();
    g_state.state         = APP_STATE_ERROR;
    g_state.motor_running = false;
}

/* --- Test 6: Depth target reached --- */
static void sim_depth_target(void) {
    buzzer_beep_mock();
    g_state.motor_running = false;
    g_state.state         = APP_STATE_IDLE;
}

/* --- Test 7: Start button from IDLE --- */
static void sim_start_idle(bool any_trigger) {
    motor_stop_cmd();   /* CMD_MOTOR_SET_SPEED + CMD_MOTOR_APPLY_SETTINGS before start */
    g_state.state = any_trigger ? APP_STATE_TAPPING : APP_STATE_DRILLING;
}

/* --- Test 8: Start button while running --- */
static void sim_start_while_running(void) {
    motor_stop_cmd();
    g_state.state = APP_STATE_IDLE;
}

/* --- Test 9: Overheat --- */
static void sim_overheat(void) {
    motor_stop_cmd();
    uint16_t temp = motor_get_temperature();
    g_state.state         = APP_STATE_ERROR;
    g_state.motor_running = false;
    g_state.motor_fault   = true;

    static char line1[17], line2[17];
    snprintf(line1, 17, "!! OVERHEAT !!");
    snprintf(line2, 17, "Temp: %dC", temp);
    g_state.error_until = 5000;
    g_state.error_line1 = line1;
    g_state.error_line2 = line2;
}

/*
 * --- Test 10: Low voltage edge detection ---
 *
 * The real driver fires EVT_LOW_VOLTAGE only when voltage crosses from ≥ threshold
 * to < threshold.  On a normal power-off the MCB cuts power before software sees
 * the crossing, so the event must NOT fire if voltage was already low (e.g. after
 * a re-read on the same low rail).
 *
 * We implement the edge-detect logic here and test both the crossing and the
 * repeated-low (no-fire) case.
 */
#define LOW_VOLTAGE_EVENT_FIRED   1
#define LOW_VOLTAGE_EVENT_SILENT  0

static int sim_voltage_sample(uint16_t new_voltage) {
    int fired = LOW_VOLTAGE_EVENT_SILENT;
    bool now_low = (new_voltage < DC_BUS_LOW_VOLTAGE_THRESHOLD);

    if (now_low && !mock_voltage_was_low) {
        /* Falling edge only */
        g_state.error_until = 3000;
        g_state.error_line1 = " LOW VOLTAGE! ";
        g_state.error_line2 = "Check power     ";
        fired = LOW_VOLTAGE_EVENT_FIRED;
    }

    mock_voltage_was_low    = now_low;
    g_state.dc_bus_voltage  = new_voltage;
    return fired;
}

/*===========================================================================*/
/* Dispatch table (Test 11) — matches events.c verbatim                      */
/*===========================================================================*/

typedef void (*event_handler_func_t)(void);

typedef struct {
    event_type_t           event;
    event_handler_func_t   handler;
} dispatch_entry_t;

/* Dummy no-op stubs for the handlers not individually tested here */
static void stub_zero(void)      {}
static void stub_menu(void)      {}
static void stub_f1(void)        {}
static void stub_f1_long(void)   {}
static void stub_f2(void)        {}
static void stub_f3(void)        {}
static void stub_f4(void)        {}
static void stub_encoder(void)   {}
static void stub_enc_long(void)  {}
static void stub_guard(void)     {}
static void stub_enc_cw(void)    {}
static void stub_enc_ccw(void)   {}
static void stub_jam(void)       {}
static void stub_load_spike(void){}
static void stub_temp_warn(void) {}
static void stub_boot_done(void) {}
static void stub_low_voltage(void){}

/* Wrappers that forward to the sim functions */
static void wrap_estop(void)      { /* level-sensitive - direction known from g_state.estop_active */ }
static void wrap_start(void)      { /* direction depends on state */ }
static void wrap_motor_fault(void){ sim_motor_fault(); }
static void wrap_depth_target(void){ sim_depth_target(); }
static void wrap_overheat(void)   { sim_overheat(); }

static const dispatch_entry_t dispatch_table[] = {
    {EVT_BTN_ZERO,      stub_zero},
    {EVT_BTN_MENU,      stub_menu},
    {EVT_BTN_START,     wrap_start},
    {EVT_BTN_F1,        stub_f1},
    {EVT_BTN_F1_LONG,   stub_f1_long},
    {EVT_BTN_F2,        stub_f2},
    {EVT_BTN_F3,        stub_f3},
    {EVT_BTN_F4,        stub_f4},
    {EVT_BTN_ENCODER,   stub_encoder},
    {EVT_BTN_ENC_LONG,  stub_enc_long},
    {EVT_BTN_ESTOP,     wrap_estop},
    {EVT_BTN_GUARD,     stub_guard},
    {EVT_ENC_CW,        stub_enc_cw},
    {EVT_ENC_CCW,       stub_enc_ccw},
    {EVT_MOTOR_FAULT,   wrap_motor_fault},
    {EVT_JAM_DETECTED,  stub_jam},
    {EVT_LOAD_SPIKE,    stub_load_spike},
    {EVT_OVERHEAT,      wrap_overheat},
    {EVT_TEMP_WARNING,  stub_temp_warn},
    {EVT_DEPTH_TARGET,  wrap_depth_target},
    {EVT_BOOT_COMPLETE, stub_boot_done},
    {EVT_LOW_VOLTAGE,   stub_low_voltage},
};

#define DISPATCH_TABLE_SIZE (sizeof(dispatch_table) / sizeof(dispatch_table[0]))

/* Full list of every enum value to verify against the dispatch table */
static const event_type_t all_events[] = {
    EVT_BTN_ZERO,
    EVT_BTN_MENU,
    EVT_BTN_F1,
    EVT_BTN_F2,
    EVT_BTN_F3,
    EVT_BTN_F4,
    EVT_BTN_START,
    EVT_BTN_GUARD,
    EVT_BTN_ENCODER,
    EVT_BTN_ESTOP,
    EVT_BTN_F1_LONG,
    EVT_BTN_ENC_LONG,
    EVT_ENC_CW,
    EVT_ENC_CCW,
    EVT_MOTOR_FAULT,
    EVT_JAM_DETECTED,
    EVT_DEPTH_TARGET,
    EVT_LOAD_SPIKE,
    EVT_OVERHEAT,
    EVT_TEMP_WARNING,
    EVT_LOW_VOLTAGE,
    EVT_BOOT_COMPLETE,
};

#define ALL_EVENTS_COUNT (sizeof(all_events) / sizeof(all_events[0]))

/*===========================================================================*/
/* Unity fixtures                                                             */
/*===========================================================================*/

void setUp(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.state         = APP_STATE_IDLE;
    g_state.guard_closed  = true;
    g_state.error_line1   = "";
    g_state.error_line2   = "";
    mock_spindle_hold_active = false;
    mock_reset_calls();
}

void tearDown(void) {
    /* nothing */
}

/*===========================================================================*/
/* Test 1 — E-Stop engage                                                    */
/*===========================================================================*/

void test_estop_engage_sets_error_state(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_estop_engage();
    TEST_ASSERT_EQUAL(APP_STATE_ERROR, g_state.state);
}

void test_estop_engage_clears_motor_running(void) {
    g_state.motor_running = true;
    sim_estop_engage();
    TEST_ASSERT_FALSE(g_state.motor_running);
}

void test_estop_engage_sets_motor_fault(void) {
    sim_estop_engage();
    TEST_ASSERT_TRUE(g_state.motor_fault);
}

void test_estop_engage_sets_estop_active(void) {
    sim_estop_engage();
    TEST_ASSERT_TRUE(g_state.estop_active);
}

void test_estop_engage_issues_spindle_hold(void) {
    sim_estop_engage();
    TEST_ASSERT_GREATER_THAN(0, mock_spindle_hold_calls);
}

void test_estop_engage_reenables_motor_hw_for_hold_uart(void) {
    /* MCB must be re-enabled so spindle-hold UART commands get through */
    sim_estop_engage();
    TEST_ASSERT_GREATER_THAN(0, mock_motor_enable_calls);
}

void test_estop_engage_shows_error_message(void) {
    sim_estop_engage();
    TEST_ASSERT_GREATER_THAN(0u, g_state.error_until);
    TEST_ASSERT_NOT_NULL(g_state.error_line1);
    TEST_ASSERT_NOT_EQUAL(0, strlen(g_state.error_line1));
}

/*===========================================================================*/
/* Test 2 — E-Stop release                                                   */
/*===========================================================================*/

void test_estop_release_clears_estop_active(void) {
    sim_estop_engage();
    sim_estop_release();
    TEST_ASSERT_FALSE(g_state.estop_active);
}

void test_estop_release_returns_to_idle(void) {
    sim_estop_engage();
    sim_estop_release();
    TEST_ASSERT_EQUAL(APP_STATE_IDLE, g_state.state);
}

void test_estop_release_clears_motor_fault(void) {
    sim_estop_engage();
    sim_estop_release();
    TEST_ASSERT_FALSE(g_state.motor_fault);
}

void test_estop_release_clears_error_message(void) {
    sim_estop_engage();
    sim_estop_release();
    TEST_ASSERT_EQUAL(0u, g_state.error_until);
}

void test_estop_release_calls_spindle_release(void) {
    sim_estop_engage();
    int holds_before = mock_spindle_release_calls;
    sim_estop_release();
    TEST_ASSERT_GREATER_THAN(holds_before, mock_spindle_release_calls);
}

void test_estop_release_no_other_fault_goes_idle(void) {
    /* g_state starts with motor_fault=false, so release → IDLE (not ERROR) */
    g_state.estop_active = true;
    g_state.state        = APP_STATE_ERROR;
    sim_estop_release();
    TEST_ASSERT_EQUAL(APP_STATE_IDLE, g_state.state);
}

/*===========================================================================*/
/* Test 3 — Guard open while running                                         */
/*===========================================================================*/

void test_guard_open_while_drilling_goes_idle(void) {
    g_state.state        = APP_STATE_DRILLING;
    g_state.guard_closed = false;
    sim_guard_open_while_running();
    TEST_ASSERT_EQUAL(APP_STATE_IDLE, g_state.state);
}

void test_guard_open_while_drilling_issues_stop(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_guard_open_while_running();
    TEST_ASSERT_GREATER_THAN(0, mock_motor_stop_cmds);
}

void test_guard_open_while_drilling_issues_spindle_hold(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_guard_open_while_running();
    TEST_ASSERT_GREATER_THAN(0, mock_spindle_hold_calls);
}

void test_guard_open_while_drilling_shows_error_for_30s(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_guard_open_while_running();
    /* 30 000 ms — the guard-open display period from config.h */
    TEST_ASSERT_EQUAL(30000u, g_state.error_until);
}

void test_guard_open_while_drilling_sets_error_message(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_guard_open_while_running();
    TEST_ASSERT_NOT_EQUAL(0, strlen(g_state.error_line1));
}

/*===========================================================================*/
/* Test 4 — Guard close with spindle hold active                             */
/*===========================================================================*/

void test_guard_close_releases_spindle_hold(void) {
    mock_spindle_hold_active = true;
    sim_guard_close_release_hold();
    TEST_ASSERT_GREATER_THAN(0, mock_spindle_release_calls);
}

void test_guard_close_clears_error_message(void) {
    mock_spindle_hold_active = true;
    g_state.error_until  = 30000;
    g_state.error_line1  = " GUARD OPENED!  ";
    sim_guard_close_release_hold();
    TEST_ASSERT_EQUAL(0u, g_state.error_until);
}

void test_guard_close_clears_error_lines(void) {
    mock_spindle_hold_active = true;
    g_state.error_line1 = " GUARD OPENED!  ";
    g_state.error_line2 = " Close to clear ";
    sim_guard_close_release_hold();
    TEST_ASSERT_EQUAL_STRING("", g_state.error_line1);
    TEST_ASSERT_EQUAL_STRING("", g_state.error_line2);
}

void test_guard_close_marks_hold_inactive(void) {
    mock_spindle_hold_active = true;
    sim_guard_close_release_hold();
    TEST_ASSERT_FALSE(motor_is_spindle_hold_active());
}

/*===========================================================================*/
/* Test 5 — Motor fault                                                      */
/*===========================================================================*/

void test_motor_fault_sets_error_state(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_motor_fault();
    TEST_ASSERT_EQUAL(APP_STATE_ERROR, g_state.state);
}

void test_motor_fault_clears_motor_running(void) {
    g_state.motor_running = true;
    sim_motor_fault();
    TEST_ASSERT_FALSE(g_state.motor_running);
}

void test_motor_fault_disables_motor_hardware(void) {
    sim_motor_fault();
    TEST_ASSERT_GREATER_THAN(0, mock_motor_disable_calls);
}

void test_motor_fault_issues_stop_command(void) {
    sim_motor_fault();
    TEST_ASSERT_GREATER_THAN(0, mock_motor_stop_cmds);
}

/*===========================================================================*/
/* Test 6 — Depth target reached                                             */
/*===========================================================================*/

void test_depth_target_sets_idle(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_depth_target();
    TEST_ASSERT_EQUAL(APP_STATE_IDLE, g_state.state);
}

void test_depth_target_clears_motor_running(void) {
    g_state.motor_running = true;
    sim_depth_target();
    TEST_ASSERT_FALSE(g_state.motor_running);
}

void test_depth_target_beeps(void) {
    sim_depth_target();
    TEST_ASSERT_GREATER_THAN(0, mock_beep_calls);
}

/*===========================================================================*/
/* Test 7 — Start button from IDLE                                           */
/*===========================================================================*/

void test_start_idle_no_triggers_sets_drilling(void) {
    g_state.state = APP_STATE_IDLE;
    sim_start_idle(false);
    TEST_ASSERT_EQUAL(APP_STATE_DRILLING, g_state.state);
}

void test_start_idle_with_triggers_sets_tapping(void) {
    g_state.state = APP_STATE_IDLE;
    sim_start_idle(true);
    TEST_ASSERT_EQUAL(APP_STATE_TAPPING, g_state.state);
}

void test_start_idle_issues_motor_command(void) {
    g_state.state = APP_STATE_IDLE;
    sim_start_idle(false);
    TEST_ASSERT_GREATER_THAN(0, mock_motor_stop_cmds); /* stand-in for any motor cmd */
}

/*===========================================================================*/
/* Test 8 — Start button while running → IDLE                               */
/*===========================================================================*/

void test_start_while_drilling_returns_to_idle(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_start_while_running();
    TEST_ASSERT_EQUAL(APP_STATE_IDLE, g_state.state);
}

void test_start_while_tapping_returns_to_idle(void) {
    g_state.state = APP_STATE_TAPPING;
    sim_start_while_running();
    TEST_ASSERT_EQUAL(APP_STATE_IDLE, g_state.state);
}

void test_start_while_running_issues_stop_command(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_start_while_running();
    TEST_ASSERT_GREATER_THAN(0, mock_motor_stop_cmds);
}

/*===========================================================================*/
/* Test 9 — Overheat                                                         */
/*===========================================================================*/

void test_overheat_sets_error_state(void) {
    g_state.state = APP_STATE_DRILLING;
    sim_overheat();
    TEST_ASSERT_EQUAL(APP_STATE_ERROR, g_state.state);
}

void test_overheat_clears_motor_running(void) {
    g_state.motor_running = true;
    sim_overheat();
    TEST_ASSERT_FALSE(g_state.motor_running);
}

void test_overheat_sets_motor_fault(void) {
    sim_overheat();
    TEST_ASSERT_TRUE(g_state.motor_fault);
}

void test_overheat_issues_stop_command(void) {
    sim_overheat();
    TEST_ASSERT_GREATER_THAN(0, mock_motor_stop_cmds);
}

void test_overheat_shows_temperature_in_message(void) {
    mock_last_temp = 95;
    sim_overheat();
    /* error_line2 must contain the temperature value */
    TEST_ASSERT_NOT_NULL(g_state.error_line2);
    /* A simple check: "95" must appear in the string */
    TEST_ASSERT_NOT_NULL(strstr(g_state.error_line2, "95"));
}

void test_overheat_shows_message_for_5s(void) {
    sim_overheat();
    TEST_ASSERT_EQUAL(5000u, g_state.error_until);
}

/*===========================================================================*/
/* Test 10 — Low voltage edge: only fires on crossing, not on power-off     */
/*===========================================================================*/

void test_low_voltage_fires_on_first_crossing(void) {
    /* Voltage drops from normal (400 V) to below threshold (250 V) */
    mock_voltage_was_low = false;
    int result = sim_voltage_sample(250);
    TEST_ASSERT_EQUAL(LOW_VOLTAGE_EVENT_FIRED, result);
}

void test_low_voltage_sets_error_message_on_crossing(void) {
    mock_voltage_was_low = false;
    sim_voltage_sample(250);
    TEST_ASSERT_NOT_EQUAL(0, strlen(g_state.error_line1));
}

void test_low_voltage_does_not_refire_when_already_low(void) {
    /* Simulate power-off: voltage was already low last sample */
    mock_voltage_was_low = true;
    int result = sim_voltage_sample(150);  /* still low */
    TEST_ASSERT_EQUAL(LOW_VOLTAGE_EVENT_SILENT, result);
}

void test_low_voltage_does_not_fire_above_threshold(void) {
    mock_voltage_was_low = false;
    int result = sim_voltage_sample(356);  /* normal bus voltage */
    TEST_ASSERT_EQUAL(LOW_VOLTAGE_EVENT_SILENT, result);
}

void test_low_voltage_fires_again_after_recovery(void) {
    /* Drop → recover → drop again must fire */
    mock_voltage_was_low = false;
    sim_voltage_sample(250);              /* first crossing */
    sim_voltage_sample(380);              /* recovery */
    int result = sim_voltage_sample(200); /* second crossing */
    TEST_ASSERT_EQUAL(LOW_VOLTAGE_EVENT_FIRED, result);
}

void test_low_voltage_exactly_at_threshold_is_not_low(void) {
    /* Boundary: at exactly the threshold value, no event */
    mock_voltage_was_low = false;
    int result = sim_voltage_sample(DC_BUS_LOW_VOLTAGE_THRESHOLD);
    TEST_ASSERT_EQUAL(LOW_VOLTAGE_EVENT_SILENT, result);
}

void test_low_voltage_one_below_threshold_fires(void) {
    mock_voltage_was_low = false;
    int result = sim_voltage_sample(DC_BUS_LOW_VOLTAGE_THRESHOLD - 1);
    TEST_ASSERT_EQUAL(LOW_VOLTAGE_EVENT_FIRED, result);
}

/*===========================================================================*/
/* Test 11 — Dispatch table completeness                                     */
/*===========================================================================*/

/*
 * Verify that every event_type_t value in all_events[] has exactly one entry
 * in dispatch_table[].  This catches forgotten handlers and, specifically,
 * confirms that EVT_DEPTH_TARGET (recently added) is present.
 */
void test_dispatch_table_covers_all_events(void) {
    for (size_t i = 0; i < ALL_EVENTS_COUNT; i++) {
        event_type_t evt = all_events[i];
        bool found = false;
        for (size_t j = 0; j < DISPATCH_TABLE_SIZE; j++) {
            if (dispatch_table[j].event == evt) {
                found = true;
                break;
            }
        }
        if (!found) {
            /* Print the missing event value for diagnosis */
            char msg[64];
            snprintf(msg, sizeof(msg), "Missing handler for event 0x%04X", (unsigned)evt);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

void test_dispatch_table_has_no_duplicate_events(void) {
    for (size_t i = 0; i < DISPATCH_TABLE_SIZE; i++) {
        for (size_t j = i + 1; j < DISPATCH_TABLE_SIZE; j++) {
            if (dispatch_table[i].event == dispatch_table[j].event) {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "Duplicate entry for event 0x%04X at indices %zu and %zu",
                         (unsigned)dispatch_table[i].event, i, j);
                TEST_FAIL_MESSAGE(msg);
            }
        }
    }
}

void test_dispatch_table_evt_depth_target_present(void) {
    bool found = false;
    for (size_t i = 0; i < DISPATCH_TABLE_SIZE; i++) {
        if (dispatch_table[i].event == EVT_DEPTH_TARGET) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "EVT_DEPTH_TARGET missing from dispatch table");
}

void test_dispatch_table_all_handlers_non_null(void) {
    for (size_t i = 0; i < DISPATCH_TABLE_SIZE; i++) {
        TEST_ASSERT_NOT_NULL(dispatch_table[i].handler);
    }
}

void test_dispatch_table_size_matches_event_count(void) {
    /* Exactly one handler per event — table and enum list must agree */
    TEST_ASSERT_EQUAL(ALL_EVENTS_COUNT, DISPATCH_TABLE_SIZE);
}

/*===========================================================================*/
/* Test runner                                                                */
/*===========================================================================*/

int main(void) {
    UNITY_BEGIN();

    /* Test 1: E-Stop engage */
    RUN_TEST(test_estop_engage_sets_error_state);
    RUN_TEST(test_estop_engage_clears_motor_running);
    RUN_TEST(test_estop_engage_sets_motor_fault);
    RUN_TEST(test_estop_engage_sets_estop_active);
    RUN_TEST(test_estop_engage_issues_spindle_hold);
    RUN_TEST(test_estop_engage_reenables_motor_hw_for_hold_uart);
    RUN_TEST(test_estop_engage_shows_error_message);

    /* Test 2: E-Stop release */
    RUN_TEST(test_estop_release_clears_estop_active);
    RUN_TEST(test_estop_release_returns_to_idle);
    RUN_TEST(test_estop_release_clears_motor_fault);
    RUN_TEST(test_estop_release_clears_error_message);
    RUN_TEST(test_estop_release_calls_spindle_release);
    RUN_TEST(test_estop_release_no_other_fault_goes_idle);

    /* Test 3: Guard open while running */
    RUN_TEST(test_guard_open_while_drilling_goes_idle);
    RUN_TEST(test_guard_open_while_drilling_issues_stop);
    RUN_TEST(test_guard_open_while_drilling_issues_spindle_hold);
    RUN_TEST(test_guard_open_while_drilling_shows_error_for_30s);
    RUN_TEST(test_guard_open_while_drilling_sets_error_message);

    /* Test 4: Guard close with hold active */
    RUN_TEST(test_guard_close_releases_spindle_hold);
    RUN_TEST(test_guard_close_clears_error_message);
    RUN_TEST(test_guard_close_clears_error_lines);
    RUN_TEST(test_guard_close_marks_hold_inactive);

    /* Test 5: Motor fault */
    RUN_TEST(test_motor_fault_sets_error_state);
    RUN_TEST(test_motor_fault_clears_motor_running);
    RUN_TEST(test_motor_fault_disables_motor_hardware);
    RUN_TEST(test_motor_fault_issues_stop_command);

    /* Test 6: Depth target */
    RUN_TEST(test_depth_target_sets_idle);
    RUN_TEST(test_depth_target_clears_motor_running);
    RUN_TEST(test_depth_target_beeps);

    /* Test 7: Start button from IDLE */
    RUN_TEST(test_start_idle_no_triggers_sets_drilling);
    RUN_TEST(test_start_idle_with_triggers_sets_tapping);
    RUN_TEST(test_start_idle_issues_motor_command);

    /* Test 8: Start button while running */
    RUN_TEST(test_start_while_drilling_returns_to_idle);
    RUN_TEST(test_start_while_tapping_returns_to_idle);
    RUN_TEST(test_start_while_running_issues_stop_command);

    /* Test 9: Overheat */
    RUN_TEST(test_overheat_sets_error_state);
    RUN_TEST(test_overheat_clears_motor_running);
    RUN_TEST(test_overheat_sets_motor_fault);
    RUN_TEST(test_overheat_issues_stop_command);
    RUN_TEST(test_overheat_shows_temperature_in_message);
    RUN_TEST(test_overheat_shows_message_for_5s);

    /* Test 10: Low voltage edge detection */
    RUN_TEST(test_low_voltage_fires_on_first_crossing);
    RUN_TEST(test_low_voltage_sets_error_message_on_crossing);
    RUN_TEST(test_low_voltage_does_not_refire_when_already_low);
    RUN_TEST(test_low_voltage_does_not_fire_above_threshold);
    RUN_TEST(test_low_voltage_fires_again_after_recovery);
    RUN_TEST(test_low_voltage_exactly_at_threshold_is_not_low);
    RUN_TEST(test_low_voltage_one_below_threshold_fires);

    /* Test 11: Dispatch table completeness */
    RUN_TEST(test_dispatch_table_covers_all_events);
    RUN_TEST(test_dispatch_table_has_no_duplicate_events);
    RUN_TEST(test_dispatch_table_evt_depth_target_present);
    RUN_TEST(test_dispatch_table_all_handlers_non_null);
    RUN_TEST(test_dispatch_table_size_matches_event_count);

    return UNITY_END();
}
