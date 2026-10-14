/**
 * @file test_main.c
 * @brief Unit tests for jam/stall detection module (jam.c)
 *
 * Self-contained: mocks all dependencies so jam.c can be compiled
 * directly without FreeRTOS, HAL, or motor hardware.
 *
 * Coverage:
 *   - Startup timeout detection (JAM_STARTUP_TIMEOUT)
 *   - Stall detection after successful start (JAM_STALL_DETECTED)
 *   - Communication timeout (JAM_COMM_TIMEOUT)
 *   - Normal operation — no false positives
 *   - jam_acknowledge() clears jam state
 *   - Load spike detection via jam_load_update()
 *   - Spike detection gated by jam_enabled flag
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*===========================================================================*/
/* Pull in mock infrastructure (same pattern as test_motor_uart)             */
/*===========================================================================*/

#include "../mocks/stm32f1xx_hal.c"  /* provides mock_tick_count + HAL_GetTick */
#include "../mocks/FreeRTOS.c"       /* provides mock_tick_step + xTaskGetTickCount */

/*===========================================================================*/
/* portTICK_PERIOD_MS — not in mocks, but used by jam_load_update()          */
/* pdMS_TO_TICKS(ms) == ms in the mock (1 tick = 1 ms), so period = 1.      */
/*===========================================================================*/

#ifndef portTICK_PERIOD_MS
#define portTICK_PERIOD_MS 1
#endif

/*===========================================================================*/
/* Motor status mock — motor_get_status() / motor_get_vibration()           */
/*===========================================================================*/

typedef enum {
    MOTOR_STOPPED = 0,
    MOTOR_FORWARD = 1,
    MOTOR_REVERSE = 2,
    MOTOR_BRAKING = 3
} motor_state_t;

typedef struct {
    motor_state_t state;
    uint16_t speed_rpm;
    uint16_t target_speed;
    uint16_t actual_rpm;
    bool fault;
    bool overload;
    bool jam_detected;
    uint32_t last_update_ms;  /* unused by jam.c since 2026-08-30 — see below */
    uint16_t vibration;
    uint16_t load_percent;
    uint16_t temperature;
    uint16_t raw_flags;
    bool rps_error;
    bool pfc_fault;
    bool voltage_error;
    bool overheat;
    uint8_t retry_count;
} motor_status_t;

static motor_status_t mock_motor_status;

const motor_status_t* motor_get_status(void) {
    return &mock_motor_status;
}

uint16_t motor_get_vibration(void) {
    return mock_motor_status.vibration;
}

/*===========================================================================*/
/* motor_emergency_stop() mock — records calls                               */
/*===========================================================================*/

static bool emergency_stop_called = false;

void motor_emergency_stop(void) {
    emergency_stop_called = true;
}

/*===========================================================================*/
/* Event queue mock — records last event sent via SEND_EVENT                 */
/*===========================================================================*/

/* shared.h defines SEND_EVENT via xQueueSend + g_event_queue.
 * We mock both here before jam.c (via shared.h) is compiled. */

typedef void* QueueHandle_t;
typedef long  BaseType_t;
typedef uint32_t TickType_t;   /* already via FreeRTOS.h — harmless redecl */

#define pdTRUE  1
#define pdFALSE 0

static int last_event = 0;
static int event_count = 0;   /* how many events one trip queued */
QueueHandle_t g_event_queue = (QueueHandle_t)1;

BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t wait) {
    (void)q; (void)wait;
    last_event = *(const int*)item;
    event_count++;
    return pdTRUE;
}

/*===========================================================================*/
/* Stubs for other shared.h / config.h symbols jam.c might pull in          */
/*===========================================================================*/

/* shared.h references g_state for overflow counters inside SEND_EVENT and
 * for current_rpm (read by the low-load detector). Minimal stub so the
 * macros compile without the full struct. */
struct {
    uint16_t event_queue_overflows;
    uint16_t current_rpm;
} g_state;

/* uart_puts: used by jam.c for debug logging in non-NDEBUG builds */
void uart_puts(const char* s) { (void)s; }

/*===========================================================================*/
/* SEND_EVENT macro — replaces the shared.h version                         */
/*    The shared.h version expands to xQueueSend + g_state.overflows.       */
/*    We define our own simpler version here so jam.c gets a working macro. */
/*===========================================================================*/

#define SEND_EVENT(evt) do { \
    int _e = (int)(evt); \
    xQueueSend(g_event_queue, &_e, 0); \
} while(0)

/*===========================================================================*/
/* Jam detection constants (copied from jam.h / jam.c)                       */
/*===========================================================================*/

/* From jam.h */
#define JAM_STARTUP_TIMEOUT_MS   3000   /* 3 s to start */
#define JAM_STALL_TIMEOUT_MS      500   /* 500 ms stall  */
#define JAM_COMM_TIMEOUT_MS      1000   /* 1 s no comms  */
#define JAM_VIBRATION_THRESHOLD   800
#define JAM_VIBRATION_TIMEOUT_MS  200

/* From jam.c (private constants) */
#define JAM_LOAD_THRESHOLD       90     /* 90% → potential jam */
#define JAM_LOAD_TIMEOUT_MS    5000     /* 5 s sustained high load */

/* Event codes (from shared.h) */
#define EVT_JAM_DETECTED  0x2000
#define EVT_LOAD_SPIKE    0x8005

/*===========================================================================*/
/* Include the module under test                                              */
/*===========================================================================*/

/* Prevent jam.c from re-including headers whose types we already defined.   */
#define MOTOR_H            /* skip motor.h — we defined motor_status_t above */
#define SHARED_H           /* skip shared.h — we mocked its key pieces       */
#define CONFIG_H           /* skip config.h — constants duplicated above      */

/* jam.h defines JAM_* constants and the jam_status_t struct.
 * We include it — our #defines above match exactly, so no conflict.        */
#include "../../src/motor_load.c"
#include "../../src/jam.c"

/*===========================================================================*/
/* Test helpers                                                               */
/*===========================================================================*/

/** Advance the mock tick counter by @p ms milliseconds. */
static void tick_advance(uint32_t ms) {
    mock_tick_count += ms;
}

/** Reset all mutable test state. Called from setUp(). */
static void reset_mocks(void) {
    emergency_stop_called = false;
    last_event = 0;
    event_count = 0;
    memset(&mock_motor_status, 0, sizeof(mock_motor_status));
    /* Start at tick 1, not 0.
     * jam.c uses motor_start_time > 0 as a sentinel meaning "motor was started".
     * If HAL_GetTick() returns 0 when jam_motor_started() is called, the
     * startup-timeout check is permanently skipped.  A non-zero baseline
     * avoids that edge case without changing any timeout arithmetic. */
    mock_tick_count = 1;
    mock_tick_step  = 0;
    memset(&g_state, 0, sizeof(g_state));
}

/*===========================================================================*/
/* Unity setUp / tearDown                                                     */
/*===========================================================================*/

void setUp(void) {
    reset_mocks();
    jam_init();
    motor_load_init();
    /* jam_init() caches HAL_GetTick() as last_response_time.
     * With tick starting at 1, last_response_time = 1, matching the baseline. */
}

void tearDown(void) {
    /* nothing */
}

/*===========================================================================*/
/* 1. Startup Timeout                                                         */
/*    Motor commanded, never starts → JAM_STARTUP_TIMEOUT after 3 s         */
/*===========================================================================*/

void test_startup_timeout_triggers_after_3s(void) {
    /* Motor commanded to start but never reports running.
     * We call jam_notify_response() so the comm-timeout (1 s) doesn't fire
     * before the startup timeout (3 s) under test.
     *
     * These used to write mock_motor_status.last_update_ms instead, mirroring
     * a shadow copy in jam.c that read motor_get_status()->last_update_ms and
     * overwrote the value jam_notify_response() had just set. That field is
     * written only by motor_read_response(), which has no callers, so in the
     * firmware it was always 0 and the copy was dead code — but the tests kept
     * it alive and so described a data flow the machine never had.
     * jam_notify_response(), called from the motor task's hot path, is the
     * single authority. */
    jam_motor_started();

    /* Advance to just before threshold — no jam yet. */
    tick_advance(JAM_STARTUP_TIMEOUT_MS - 1);
    jam_notify_response();  /* feed comm-timeout */
    bool jammed = jam_update(false /* not running */, true /* commanded */);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_FALSE(jam_is_active());
    TEST_ASSERT_FALSE(emergency_stop_called);

    /* Advance past threshold — startup timeout must now fire. */
    tick_advance(2);
    jam_notify_response();  /* keep comm alive */
    jammed = jam_update(false, true);

    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_TRUE(jam_is_active());
    TEST_ASSERT_EQUAL(JAM_STARTUP_TIMEOUT, jam_get_status()->type);
    TEST_ASSERT_TRUE(emergency_stop_called);
}

void test_startup_no_jam_when_motor_starts_in_time(void) {
    /* Motor commanded and starts within 1 s — no jam. */
    jam_motor_started();

    tick_advance(1000);
    /* Motor is now running. */
    bool jammed = jam_update(true /* running */, true /* commanded */);

    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_FALSE(jam_is_active());
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/*===========================================================================*/
/* 2. Stall Detection                                                         */
/*    Motor was running, then stops while still commanded → JAM_STALL after  */
/*    JAM_STALL_TIMEOUT_MS                                                   */
/*===========================================================================*/

void test_stall_triggers_after_500ms(void) {
    /* Successful startup: motor starts within 1 s.
     * Notify a response throughout so comm-timeout (1 s) doesn't
     * interfere with the stall timeout (500 ms) under test. */
    jam_motor_started();
    jam_notify_response();
    tick_advance(500);
    jam_notify_response();
    jam_update(true, true);   /* startup_complete = true */

    /* Motor stops while still commanded. */
    tick_advance(10);
    jam_notify_response();
    jam_update(false, true);  /* stall_start_time set here */

    /* Advance to just before stall threshold — no jam yet. */
    tick_advance(JAM_STALL_TIMEOUT_MS - 1);
    jam_notify_response();
    bool jammed = jam_update(false, true);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_FALSE(jam_is_active());

    /* Cross the threshold — stall must fire before comm-timeout. */
    tick_advance(2);
    jam_notify_response();
    jammed = jam_update(false, true);

    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_TRUE(jam_is_active());
    TEST_ASSERT_EQUAL(JAM_STALL_DETECTED, jam_get_status()->type);
    TEST_ASSERT_TRUE(emergency_stop_called);
}

void test_stall_no_jam_before_startup_complete(void) {
    /* Motor commanded but startup not yet confirmed (motor_was_running=false).
     * A brief not-running period should not trigger stall — only startup timeout
     * is in play, and 500ms is well within the 3s startup window. */
    jam_motor_started();

    tick_advance(500);
    /* Motor still not running — no startup_complete, so stall logic skipped. */
    bool jammed = jam_update(false, true);

    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
}

/*===========================================================================*/
/* 3. Communication Timeout                                                   */
/*    Motor controller silent for JAM_COMM_TIMEOUT_MS while commanded        */
/*===========================================================================*/

void test_comm_timeout_triggers_after_1s(void) {
    /* jam_init() sets last_response_time = HAL_GetTick() = 0.
     * No jam_notify_response() calls (no updates from the controller).
     * Advance beyond comm timeout — jam must fire. */

    tick_advance(JAM_COMM_TIMEOUT_MS + 1);
    bool jammed = jam_update(false /* not running */, true /* commanded */);

    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_COMM_TIMEOUT, jam_get_status()->type);
    TEST_ASSERT_TRUE(emergency_stop_called);
}

void test_comm_timeout_resets_when_status_received(void) {
    /* Provide a status update — last_response_time should advance. */
    tick_advance(500);
    jam_notify_response();  /* MCB responded */
    jam_update(true, true);

    /* Now advance another 500 ms (total 1000 ms, but last response was at 500).
     * Comm timer restarts from 500 → only 500 ms elapsed since last update. */
    tick_advance(500);
    bool jammed = jam_update(true, true);

    /* 500 ms < 1000 ms threshold — no jam yet. */
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/*===========================================================================*/
/* 4. Normal Operation — no false positives                                  */
/*    Motor starts, runs continuously, no jam should ever fire               */
/*===========================================================================*/

void test_normal_operation_no_jam(void) {
    jam_motor_started();

    /* Simulate 10 seconds of healthy operation at 50 ms intervals. */
    for (int i = 0; i < 200; i++) {
        tick_advance(50);
        /* Keep comm-timeout fed: controller responds every 50 ms. */
        jam_notify_response();
        bool jammed = jam_update(true /* running */, true /* commanded */);
        TEST_ASSERT_FALSE(jammed);
    }

    TEST_ASSERT_FALSE(jam_is_active());
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

void test_no_jam_when_not_commanded(void) {
    /* Motor not commanded at all — detector must stay silent. */
    tick_advance(10000);
    bool jammed = jam_update(false, false);

    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_FALSE(jam_is_active());
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/*===========================================================================*/
/* 5. Acknowledge                                                              */
/*    After a jam fires, jam_acknowledge() must clear the active flag.       */
/*===========================================================================*/

void test_acknowledge_clears_active_jam(void) {
    /* Trigger a startup timeout jam. */
    jam_motor_started();
    tick_advance(JAM_STARTUP_TIMEOUT_MS + 1);
    jam_update(false, true);

    TEST_ASSERT_TRUE(jam_is_active());

    jam_acknowledge();

    /* Acknowledged — is_active() must return false. */
    TEST_ASSERT_FALSE(jam_is_active());
    TEST_ASSERT_TRUE(jam_get_status()->acknowledged);
}

void test_acknowledge_and_next_update_clears_type(void) {
    /* Trigger jam, acknowledge, then call jam_update() again — type clears. */
    jam_motor_started();
    tick_advance(JAM_STARTUP_TIMEOUT_MS + 1);
    jam_update(false, true);
    jam_acknowledge();

    /* Next update with motor not commanded should clear the jam type. */
    jam_update(false, false);

    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
}

void test_jam_motor_stopped_clears_after_acknowledge(void) {
    /* Trigger jam, acknowledge, signal motor stopped → type clears. */
    jam_motor_started();
    tick_advance(JAM_STARTUP_TIMEOUT_MS + 1);
    jam_update(false, true);
    jam_acknowledge();

    jam_motor_stopped();

    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
}

void test_unacknowledged_jam_persists_through_update(void) {
    /* Trigger a startup-timeout jam (feed comm-timeout to isolate scenario). */
    jam_motor_started();
    tick_advance(JAM_STARTUP_TIMEOUT_MS + 1);
    jam_notify_response();  /* keep comm alive */
    jam_update(false, true);

    /* Record which jam type fired. */
    jam_type_t fired_type = jam_get_status()->type;
    TEST_ASSERT_NOT_EQUAL(JAM_NONE, fired_type);

    /* Additional updates while jam is unacknowledged — jam must stay. */
    bool still_jammed = jam_update(false, true);
    TEST_ASSERT_TRUE(still_jammed);
    TEST_ASSERT_TRUE(jam_is_active());
    TEST_ASSERT_EQUAL(fired_type, jam_get_status()->type);
}

/*===========================================================================*/
/* 6. Load Spike                                                               */
/*    jam_load_update() with load above spike_threshold → EVT_LOAD_SPIKE     */
/*===========================================================================*/

/* Run the motor up to a settled, armed baseline and leave inrush grace
 * expired, so a following feed_load()/feed_step() reaches the detectors.
 *
 * The learned baseline no longer sets any trip point — the absolute tiers come
 * from the operator's Spike Thresh and the step tier from the raw delta — so
 * the value primed here only matters to tests that care about the display-side
 * baseline. It still has to be CALLED, because it is what gets the inrush
 * grace out of the way. */
static void prime_baseline_at(uint8_t baseline_load) {
    motor_load_motor_started(1000);   // grace=2000ms, stability_required=2000ms
    motor_load_update(baseline_load, 1000, 1000, true);  // first sample, in window
    tick_advance(2001);
    motor_load_update(baseline_load, 1000, 1000, true);  // stability complete -> armed
    /* Reset the EMA so the next motor_load_update warm-starts at the test
     * value. motor_load.c's `filter_initialized` is a static in the same
     * translation unit (we #include the source), so accessing it directly
     * is safe in this unit-test scope. */
    filter_initialized = false;
}

/* Feed a sample with the low-load detector ENABLED. No motor_load_motor_started
 * here on purpose: that sets the inrush grace, which gates low-load off. */
static bool feed_lowload(uint8_t raw, uint16_t cv, uint8_t low_thr) {
    motor_load_update(raw, cv, 1000, true);
    return jam_load_update(true /* running */, true /* jam_en */,
                           true /* spike_en */, 95 /* spike_thr */,
                           0 /* step disabled */,
                           true /* low-load enabled */, low_thr);
}

/* Feed a raw KR sample then poll jam_load_update. Step + low-load disabled
 * so legacy spike/sustained semantics remain testable in isolation. */
static bool feed_load(uint8_t raw, bool is_running, bool jam_en,
                      bool spike_en, uint8_t spike_thr) {
    motor_load_update(raw, 1000, 1000, is_running);
    return jam_load_update(is_running, jam_en, spike_en, spike_thr,
                           0 /* step disabled */,
                           false /* low-load disabled */, 0);
}

/* The 2026-10-13 machine finding: a start at SV=500 reports CV=0 for the whole
 * run while the spindle turns and KR reads 11-14%. The low-load detector read
 * that as a snapped belt and emergency-stopped a cut that was fine. A detector
 * may not treat "this run never produced a speed reading" as physical evidence
 * that the spindle has stopped. */
void test_low_load_holds_off_until_cv_has_read_something(void) {
    /* CV never rises above JAM_CV_VALID_RPM. Far more than the 18-sample
     * debounce, with KR below the low-load threshold the whole time. */
    for (int i = 0; i < 40; i++) {
        bool jammed = feed_lowload(12, 0 /* CV */, 15);
        TEST_ASSERT_FALSE(jammed);
    }
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
    TEST_ASSERT_FALSE(jam_cv_feedback_seen());
}

void test_low_load_trips_once_cv_has_been_valid(void) {
    /* The spindle comes up and CV reads properly ... */
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE(feed_lowload(12, 1000, 15));
    }
    TEST_ASSERT_TRUE(jam_cv_feedback_seen());

    /* ... then it stops turning with no load on it. That is the real belt
     * break this detector exists for, and it must still fire. */
    bool jammed = false;
    for (int i = 0; i < JAM_LOW_LOAD_DEBOUNCE && !jammed; i++) {
        jammed = feed_lowload(12, 0, 15);
    }
    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOW_LOAD, jam_get_status()->type);
    TEST_ASSERT_TRUE(emergency_stop_called);
}

/* AUDIT FIX (LOW, jam.c:417): a spike used to queue TWO events — the
 * EVT_JAM_DETECTED that trigger_jam() sends, plus an EVT_LOAD_SPIKE from the
 * caller. events.c handled them in that order, so the persistent "DRILL BIT
 * JAM" screen was overwritten by a 2-second "LOAD SPIKE" one, and when that
 * expired the operator saw a normal screen on a machine latched in ERROR.
 * One trip now sends exactly one event, and handle_jam_detected() names the
 * detector from jam_get_status()->type. This test asserts that contract; it
 * previously asserted the duplicate. */
void test_load_spike_sends_exactly_one_jam_event(void) {
    /* spike_threshold = 95, load = 96 → immediate spike. */
    prime_baseline_at(70);  /* spike_thresh_eff = min(70+25, 95) = 95 */
    event_count = 0;
    bool jammed = feed_load(96, true, true, true, 95);
    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOAD_SPIKE, jam_get_status()->type);
    TEST_ASSERT_TRUE(emergency_stop_called);
    TEST_ASSERT_EQUAL(EVT_JAM_DETECTED, last_event);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, event_count,
                                  "one jam trip must queue exactly one event");
}

void test_load_spike_below_threshold_no_jam(void) {
    prime_baseline_at(70);
    bool jammed = feed_load(94, true, true, true, 95);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

void test_load_sustained_triggers_after_5s(void) {
    /* baseline=70 → sustained_thresh = min(70+30, 90) = 90 → triggers at >=90 */
    prime_baseline_at(70);
    /* First call: arm the timer. */
    bool jammed = feed_load(92, true, true, false, 95);
    TEST_ASSERT_FALSE(jammed);

    tick_advance(JAM_LOAD_TIMEOUT_MS + 1);

    /* Second call at high load: elapsed > 5000 ms → sustained jam. */
    jammed = feed_load(92, true, true, false, 95);

    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOAD_SUSTAINED, jam_get_status()->type);
    TEST_ASSERT_TRUE(emergency_stop_called);
    TEST_ASSERT_EQUAL(EVT_JAM_DETECTED, last_event);
}

void test_load_jam_timer_resets_when_load_drops(void) {
    prime_baseline_at(70);
    feed_load(92, true, true, false, 95);
    tick_advance(3000);
    /* Several samples at the lower load, not one: jam.c tests the EMA, and the
     * EMA needs a few 50 ms polls to walk down from 92 to below the sustained
     * threshold. A single sample leaves filtered at 87 — still above the line,
     * so the timer would legitimately keep running. */
    for (int i = 0; i < 6; i++) {
        feed_load(50, true, true, false, 95);
    }
    tick_advance(3000);
    bool jammed = feed_load(92, true, true, false, 95);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
}

/*===========================================================================*/
/* 6b. Step detector — the qualifiers added 2026-10-13                        */
/*===========================================================================*/

/* Feed a raw KR sample with an explicit CV against a commanded 1000 RPM, and
 * run the step detector (spike/sustained left enabled but far out of reach at
 * spike_thr=95, so only the step path can fire). */
static bool feed_step(uint8_t raw, uint16_t cv, uint8_t step_thr) {
    motor_load_update(raw, cv, 1000, true);
    return jam_load_update(true, true, true, 95, step_thr,
                           false /* low-load disabled */, 0);
}

/* THE REGRESSION THIS WAS WRITTEN FOR: a 17 mm bi-metal hole saw in aluminium
 * emergency-stopped the machine twice on 2026-10-13. The bare step test saw a
 * +34 jump in raw KR as the teeth engaged and called it a jam, while the
 * spindle was holding its commanded speed and the MCB was at CL=100%. Load
 * stepping up IS what an interrupted cut does. */
void test_step_without_droop_no_jam(void) {
    prime_baseline_at(10);
    feed_step(11, 1000, 27);
    bool jammed = feed_step(45, 1000, 27);   /* +34 step, spindle on speed */
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* A grab is a step the spindle cannot absorb: CV falls out of regulation. */
void test_step_with_droop_trips_after_confirmation(void) {
    prime_baseline_at(10);
    feed_step(11, 1000, 27);

    /* Qualified sample 1: the step plus a 300 RPM droop. Not yet a trip —
     * one sample could be a sampling artefact. */
    bool jammed = feed_step(45, 700, 27);
    TEST_ASSERT_FALSE(jammed);

    /* Still losing on the next sample → trip. */
    jammed = feed_step(46, 700, 27);
    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOAD_STEP, jam_get_status()->type);
    TEST_ASSERT_TRUE(emergency_stop_called);
}

/* The load rises before the speed sags — spindle inertia — so the step and the
 * droop land in different samples. An AND of the two within one 50 ms poll
 * would have meant the detector never fired on a real grab. */
void test_grab_trips_when_droop_follows_the_step(void) {
    prime_baseline_at(10);
    feed_step(11, 1000, 27);
    TEST_ASSERT_FALSE(feed_step(45, 1000, 27));   /* step, speed still held */
    TEST_ASSERT_FALSE(feed_step(48, 1000, 27));   /* load up, still on speed */
    TEST_ASSERT_FALSE(feed_step(52, 880, 27));    /* speed starts to go */
    bool jammed = feed_step(55, 820, 27);         /* second drooping sample */
    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOAD_STEP, jam_get_status()->type);
    TEST_ASSERT_TRUE(emergency_stop_called);
}

/* A step that lands near the controller's ceiling needs no droop. */
void test_step_at_high_load_trips_without_droop(void) {
    prime_baseline_at(10);
    feed_step(50, 1000, 27);
    bool jammed = feed_step(82, 1000, 27);   /* +32, lands above the floor */
    TEST_ASSERT_FALSE(jammed);               /* one sample is not enough */
    jammed = feed_step(83, 1000, 27);
    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOAD_STEP, jam_get_status()->type);
}

/* The OEM detector required both samples to be non-zero. A single KR=0 from
 * the MCB mid-cut otherwise reads as a full-load step on the sample after it. */
void test_droop_holds_off_until_cv_has_read_something(void) {
    /* CV=0 against SV=1000 satisfies cv + tol < sv trivially, and the load is
     * above JAM_GRAB_MIN_LOAD - so without the gate this is an instant "grab"
     * on every cut of a run whose CV never came up. */
    for (int i = 0; i < 10; i++) {
        bool jammed = feed_step(30, 0 /* CV */, 27);
        TEST_ASSERT_FALSE(jammed);
    }
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/*===========================================================================*/
/* 6c. The droop path needs the spindle to have REACHED the setpoint          */
/*===========================================================================*/

/* Same idea as feed_step(), but the commanded speed is the caller's - these
 * tests are about what happens when SV moves. */
static bool feed_step_sv(uint8_t raw, uint16_t cv, uint16_t sv, uint8_t step_thr) {
    motor_load_update(raw, cv, sv, true);
    return jam_load_update(true, true, true, 95, step_thr,
                           false /* low-load disabled */, 0);
}

/* THE REGRESSION THIS WAS WRITTEN FOR, from the machine's own flight recorder
 * (trip at tick 83791, 2026-10-14). Running at 1250 with the baseline armed,
 * the operator swept the speed dial 1250 -> 1050 -> 1450 mid-cut. Grace after
 * the step-up is max(delta, 500) = 500 ms and expired 5 polls later; the
 * newest CV available was 1225, taken BEFORE the step and ~6 polls stale.
 * 1225 + 145 < 1450 at KR 29% gave two qualified samples and an emergency
 * stop, with the spindle accelerating normally the whole time. */
void test_speed_step_up_mid_run_is_not_a_grab(void) {
    motor_load_motor_started(1250);
    motor_load_update(25, 1250, 1250, true);
    tick_advance(2001);
    motor_load_update(25, 1250, 1250, true);
    TEST_ASSERT_TRUE(motor_load_at_target_seen());

    /* Dial sweep: down 200, then up 400. */
    motor_load_motor_speed_change(1250, 1050);
    motor_load_motor_speed_change(1050, 1450);
    TEST_ASSERT_FALSE(motor_load_at_target_seen());

    /* Grace runs out while CV is still the pre-step reading. */
    tick_advance(501);

    for (int i = 0; i < 20; i++) {
        bool jammed = feed_step_sv(29, 1225, 1450, 27);
        TEST_ASSERT_FALSE(jammed);
    }
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* ...and the detector comes back the moment the spindle is actually there, so
 * the gate is a delay until the comparison is meaningful, not a hole. */
void test_droop_arms_once_the_new_speed_is_reached(void) {
    motor_load_motor_started(1250);
    motor_load_update(25, 1250, 1250, true);
    motor_load_motor_speed_change(1250, 1450);
    /* Past the START grace too, not just the speed-change grace: a step-up
     * only ever EXTENDS the deadline, so the 1250 ms from the start is what
     * has to expire here before any detector is live. */
    tick_advance(1251);

    /* Ramp: under speed, real cutting load, no trip. */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_FALSE(feed_step_sv(30, 1250, 1450, 27));
    }
    /* Arrives at the new setpoint. */
    TEST_ASSERT_FALSE(feed_step_sv(30, 1445, 1450, 27));
    TEST_ASSERT_TRUE(motor_load_at_target_seen());

    /* NOW it loses the speed under load: that is a grab. */
    TEST_ASSERT_FALSE(feed_step_sv(45, 1200, 1450, 27));   /* sample 1 */
    bool jammed = feed_step_sv(46, 1180, 1450, 27);        /* sample 2 */
    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOAD_STEP, jam_get_status()->type);
}

/* The "it tripped as I started the drill" case. Spin-up to 1250 took ~2.6 s on
 * the machine; grace allows max(1250, 500) = 1250 ms. Past that the spindle is
 * still ~140 rpm short and climbing, and the recorded KR was 24% against a
 * JAM_GRAB_MIN_LOAD of 25 - one point from an emergency stop at the moment the
 * bit touches the work. Feeding it at 30% is that run with a slightly heavier
 * bit, and it must survive the whole ramp. */
void test_spin_up_under_cutting_load_is_not_a_grab(void) {
    motor_load_motor_started(1250);
    tick_advance(1251);                      /* inrush grace is over */

    const uint16_t ramp[] = {0, 22, 601, 900, 1113, 1150, 1193, 1222, 1238, 1247};
    for (unsigned i = 0; i < sizeof(ramp)/sizeof(ramp[0]); i++) {
        bool jammed = feed_step_sv(30, ramp[i], 1250, 27);
        TEST_ASSERT_FALSE(jammed);
    }
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* A step DOWN also clears the gate. CV sitting above the new SV is not droop,
 * but it is not evidence of regulation either - and the next step up must not
 * inherit it. */
void test_speed_step_down_clears_the_at_target_gate(void) {
    motor_load_motor_started(1000);
    motor_load_update(25, 1000, 1000, true);
    TEST_ASSERT_TRUE(motor_load_at_target_seen());
    motor_load_motor_speed_change(1000, 600);
    TEST_ASSERT_FALSE(motor_load_at_target_seen());
}

/* The arming window (+/-5%, floor 25 rpm) is tighter than the droop trip
 * window (+/-10%, floor 60 rpm) on purpose: a CV good enough to arm the gate
 * is by construction inside the no-trip band, so a stale reading from a nearby
 * old setpoint cannot arm straight into a trip. */
void test_arming_window_is_tighter_than_the_trip_window(void) {
    motor_load_motor_started(1000);
    motor_load_update(30, 1000, 1000, true);
    motor_load_motor_speed_change(1000, 1040);   /* 4% - within arming tol */
    tick_advance(1001);                          /* clear the start grace */

    /* The stale CV=1000 arms the gate against SV=1040... */
    bool jammed = feed_step_sv(30, 1000, 1040, 27);
    TEST_ASSERT_TRUE(motor_load_at_target_seen());
    TEST_ASSERT_FALSE(jammed);
    /* ...and is still nowhere near out of regulation, twice over. */
    jammed = feed_step_sv(30, 1000, 1040, 27);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
}

/* A trace recovered from .noinit comes back frozen. It must not then suppress
 * recording of the run the operator just started - that cost a deliberate
 * capture on 2026-10-13. A trip-frozen trace still survives a restart. */
void test_reset_recovered_trace_yields_to_a_new_run(void) {
    /* setUp() calls motor_load_init(), whose boot recovery RESTORES a
     * non-empty buffer rather than clearing it - correct at a real boot,
     * but it carries the previous test's samples in. Start from empty. */
    motor_load_trace_reset();
    motor_load_update(42, 1000, 1000, true);
    TEST_ASSERT_EQUAL(1, motor_load_trace_count());

    /* Stand in for trace_boot_recover(): frozen, origin = RESET. */
    trace_frozen = true;
    trace_origin = MLOAD_TRACE_RESET;

    motor_load_motor_started(1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, motor_load_trace_count(),
        "a reset-recovered trace must yield to a run the operator just started");
    TEST_ASSERT_FALSE(motor_load_trace_is_frozen());
}

/* Reading the trace must not destroy it: a truncated or piped console read
 * used to be the end of the only copy, which lost a real hole-saw cut's
 * recording on 2026-10-13. It is kept until the NEXT motor start. */
void test_reading_the_trace_keeps_it_until_the_next_run(void) {
    motor_load_trace_reset();
    motor_load_update(42, 1000, 1000, true);
    motor_load_trace_freeze();
    TEST_ASSERT_EQUAL(1, motor_load_trace_count());

    motor_load_trace_mark_read();           /* as JAMTRACE does */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, motor_load_trace_count(),
        "reading must not discard the trace");
    TEST_ASSERT_TRUE(motor_load_trace_is_frozen());

    /* Read it again - still there. */
    motor_load_trace_mark_read();
    TEST_ASSERT_EQUAL(1, motor_load_trace_count());

    /* The next run replaces it. */
    motor_load_motor_started(1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, motor_load_trace_count(),
        "a read trace must yield to the next run");
    TEST_ASSERT_FALSE(motor_load_trace_was_read());
}

void test_trip_frozen_trace_survives_a_restart(void) {
    /* setUp() calls motor_load_init(), whose boot recovery RESTORES a
     * non-empty buffer rather than clearing it - correct at a real boot,
     * but it carries the previous test's samples in. Start from empty. */
    motor_load_trace_reset();
    motor_load_update(42, 1000, 1000, true);
    motor_load_trace_freeze();              /* as trigger_jam() does */
    TEST_ASSERT_EQUAL(MLOAD_TRACE_TRIP, motor_load_trace_origin());

    motor_load_motor_started(1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, motor_load_trace_count(),
        "a captured trip must survive the restart that follows it");
    TEST_ASSERT_TRUE(motor_load_trace_is_frozen());
}

void test_step_ignores_dropout_sample(void) {
    prime_baseline_at(10);
    /* Climb to a high steady load in increments below the step threshold, so
     * getting there is not itself a step. Spindle stays on speed throughout;
     * 85 is above JAM_STEP_ABS_FLOOR, so the step path is live without droop. */
    feed_step(30, 1000, 27);
    feed_step(50, 1000, 27);
    feed_step(70, 1000, 27);
    feed_step(85, 1000, 27);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);

    feed_step(0, 1000, 27);                       /* the MCB reports 0 once */
    bool jammed = feed_step(85, 1000, 27);        /* +85 against a zero predecessor */
    TEST_ASSERT_FALSE(jammed);
    jammed = feed_step(85, 1000, 27);             /* and no confirmation either */
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* A step that collapses on the next sample is not a grab — the bit broke
 * through, or the MCB dropped a reading. Confirmation must cancel. */
void test_step_cancels_when_load_collapses(void) {
    prime_baseline_at(10);
    feed_step(11, 1000, 27);
    bool jammed = feed_step(45, 700, 27);    /* qualified step: armed */
    TEST_ASSERT_FALSE(jammed);
    jammed = feed_step(9, 700, 27);          /* load gone, spindle still slow */
    TEST_ASSERT_FALSE(jammed);
    jammed = feed_step(9, 700, 27);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
}

/* step_thresh == 0 means off — jam.c, settings_pack.h and the menu entry all
 * say so, and settings.c used to round it up to 5 (the most sensitive value
 * the detector has) instead. */
void test_step_threshold_zero_disables_detector(void) {
    prime_baseline_at(10);
    feed_step(11, 700, 0);
    bool jammed = feed_step(90, 700, 0);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
}

/* The absolute ceiling used to be gated on baseline_armed, which only happens
 * after CV sits within ±5% of SV for the full stability window. Feeding the
 * bit during spin-up therefore disabled it for the whole cut. */
void test_spike_trips_with_unarmed_baseline(void) {
    motor_load_motor_started(1000);          /* grace = 1000 ms */
    tick_advance(1001);                      /* let the inrush grace expire */
    /* CV nowhere near SV, so the baseline never arms. */
    motor_load_update(96, 400, 1000, true);
    bool jammed = jam_load_update(true, true, true, 95, 0, false, 0);
    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOAD_SPIKE, jam_get_status()->type);
}

/*===========================================================================*/
/* 7. Spike Detection Disabled                                                */
/*===========================================================================*/

void test_load_spike_disabled_no_detection(void) {
    prime_baseline_at(70);
    bool jammed = feed_load(100, true, true, false /* spike_enabled = OFF */, 95);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
    TEST_ASSERT_NOT_EQUAL(EVT_JAM_DETECTED, last_event);
}

void test_load_update_disabled_when_jam_detect_off(void) {
    bool jammed = feed_load(100, true, false /* jam off */, true, 95);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

void test_load_update_noop_when_not_running(void) {
    bool jammed = feed_load(100, false /* not running */, true, true, 95);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/*===========================================================================*/
/* 8. jam_get_description() sanity                                            */
/*===========================================================================*/

void test_get_description_returns_strings(void) {
    TEST_ASSERT_NOT_NULL(jam_get_description(JAM_NONE));
    TEST_ASSERT_NOT_NULL(jam_get_description(JAM_STARTUP_TIMEOUT));
    TEST_ASSERT_NOT_NULL(jam_get_description(JAM_STALL_DETECTED));
    TEST_ASSERT_NOT_NULL(jam_get_description(JAM_COMM_TIMEOUT));
    TEST_ASSERT_NOT_NULL(jam_get_description(JAM_OVERCURRENT));
    TEST_ASSERT_NOT_NULL(jam_get_description(JAM_VIBRATION));
    TEST_ASSERT_NOT_NULL(jam_get_description(JAM_LOAD_SUSTAINED));
    TEST_ASSERT_NOT_NULL(jam_get_description(JAM_LOAD_SPIKE));
}

/*===========================================================================*/
/* Test Runner                                                                */
/*===========================================================================*/

int main(void) {
    UNITY_BEGIN();

    /* Startup timeout */
    RUN_TEST(test_startup_timeout_triggers_after_3s);
    RUN_TEST(test_startup_no_jam_when_motor_starts_in_time);

    /* Stall detection */
    RUN_TEST(test_stall_triggers_after_500ms);
    RUN_TEST(test_stall_no_jam_before_startup_complete);

    /* Communication timeout */
    RUN_TEST(test_comm_timeout_triggers_after_1s);
    RUN_TEST(test_comm_timeout_resets_when_status_received);

    /* Normal operation */
    RUN_TEST(test_normal_operation_no_jam);
    RUN_TEST(test_no_jam_when_not_commanded);

    /* Acknowledge */
    RUN_TEST(test_acknowledge_clears_active_jam);
    RUN_TEST(test_acknowledge_and_next_update_clears_type);
    RUN_TEST(test_jam_motor_stopped_clears_after_acknowledge);
    RUN_TEST(test_unacknowledged_jam_persists_through_update);

    /* Load spike */
    RUN_TEST(test_load_spike_sends_exactly_one_jam_event);
    RUN_TEST(test_load_spike_below_threshold_no_jam);
    RUN_TEST(test_load_sustained_triggers_after_5s);
    RUN_TEST(test_load_jam_timer_resets_when_load_drops);

    /* Step detector qualifiers */
    RUN_TEST(test_step_without_droop_no_jam);
    RUN_TEST(test_step_with_droop_trips_after_confirmation);
    RUN_TEST(test_grab_trips_when_droop_follows_the_step);
    RUN_TEST(test_step_at_high_load_trips_without_droop);
    RUN_TEST(test_step_ignores_dropout_sample);
    RUN_TEST(test_step_cancels_when_load_collapses);
    RUN_TEST(test_step_threshold_zero_disables_detector);
    RUN_TEST(test_spike_trips_with_unarmed_baseline);

    /* CV credibility */
    RUN_TEST(test_low_load_holds_off_until_cv_has_read_something);
    RUN_TEST(test_low_load_trips_once_cv_has_been_valid);
    RUN_TEST(test_droop_holds_off_until_cv_has_read_something);
    RUN_TEST(test_speed_step_up_mid_run_is_not_a_grab);
    RUN_TEST(test_droop_arms_once_the_new_speed_is_reached);
    RUN_TEST(test_spin_up_under_cutting_load_is_not_a_grab);
    RUN_TEST(test_speed_step_down_clears_the_at_target_gate);
    RUN_TEST(test_arming_window_is_tighter_than_the_trip_window);

    /* Flight recorder */
    RUN_TEST(test_reset_recovered_trace_yields_to_a_new_run);
    RUN_TEST(test_trip_frozen_trace_survives_a_restart);
    RUN_TEST(test_reading_the_trace_keeps_it_until_the_next_run);

    /* Spike disabled */
    RUN_TEST(test_load_spike_disabled_no_detection);
    RUN_TEST(test_load_update_disabled_when_jam_detect_off);
    RUN_TEST(test_load_update_noop_when_not_running);

    /* Descriptions */
    RUN_TEST(test_get_description_returns_strings);

    return UNITY_END();
}
