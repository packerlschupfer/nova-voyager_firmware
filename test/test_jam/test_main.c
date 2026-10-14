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
    /* grace = max(1000 rpm x 1 ms, 500) = 1000 ms; stability = 2000 ms. The
     * comment here said grace was 2000 ms for a long time; it is not, and the
     * assertion below is what keeps that from mattering again. */
    motor_load_motor_started(1000);
    /* MLOAD_AT_TARGET_SAMPLES in-band polls, so the droop gate latches too -
     * a real run spends far more than 8 polls on speed before anything cuts. */
    for (int i = 0; i < MLOAD_AT_TARGET_SAMPLES; i++) {
        motor_load_update(baseline_load, 1000, 1000, true);
    }
    tick_advance(2001);
    motor_load_update(baseline_load, 1000, 1000, true);  // stability complete -> armed
    /* Every test built on this helper assumes the detectors are actually
     * reachable afterwards. Say so once, here, instead of in each of them. */
    TEST_ASSERT_FALSE(motor_load_in_spike_grace());
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
/* 6a-2. The sustained tier may not sit below what the machine idles at       */
/*===========================================================================*/

/* THE REGRESSION THIS WAS WRITTEN FOR (review finding, 2026-10-14):
 * spike_thresh is settable down to 20, and `spike - 15` put the lowest menu
 * positions at 5/10/14% - below this machine's free-run KR (15-16% at 750 rpm,
 * 24-25% at 1250). Spike Thresh 20 therefore emergency-stopped an IDLE spindle
 * 5 s after start. */
void test_sustained_tier_cannot_trip_at_free_run_load(void) {
    prime_baseline_at(25);          /* free-run baseline as measured at 1250 */
    uint8_t b = 0;
    TEST_ASSERT_TRUE(motor_load_get_baseline(&b));
    TEST_ASSERT_EQUAL(25, b);

    /* Operator winds Spike Thresh to the bottom of its range. */
    for (int i = 0; i < 10; i++) {
        bool jammed = feed_load(25, true, true, false, 20);
        TEST_ASSERT_FALSE(jammed);
        tick_advance(1000);        /* well past JAM_LOAD_TIMEOUT_MS in total */
    }
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* The baseline floor is a max(), so it can only ever push the threshold UP.
 * At the factory setting it must not move it at all - that is what keeps this
 * from being the min() that c1e624e removed. */
void test_baseline_floor_does_not_move_the_factory_threshold(void) {
    prime_baseline_at(25);
    jam_load_config_t jc;
    jam_get_load_config(90, &jc);
    TEST_ASSERT_EQUAL(90, jc.spike_thresh);
    TEST_ASSERT_EQUAL(75, jc.sustained_thresh);   /* 90 - 15, unchanged */

    /* ...and a real sustained overload still trips. */
    TEST_ASSERT_FALSE(feed_load(92, true, true, false, 90));
    tick_advance(JAM_LOAD_TIMEOUT_MS + 1);
    TEST_ASSERT_TRUE(feed_load(92, true, true, false, 90));
    TEST_ASSERT_EQUAL(JAM_LOAD_SUSTAINED, jam_get_status()->type);
}

/* The floor is deliberately NOT clamped to the spike ceiling. Clamping was
 * tried first and reopened the bug: the sustained tier is gated on jam_detect
 * alone, so with Spike Detect off it is the only tier left, and a clamp put it
 * back below free-run. An inverted pair is fine - each tier means what it
 * says, and the lower one fires first when it is enabled. */
void test_sustained_floor_is_not_clamped_to_a_nonsensical_ceiling(void) {
    prime_baseline_at(60);          /* a big tool idling heavy */
    jam_load_config_t jc;
    jam_get_load_config(50, &jc);
    TEST_ASSERT_EQUAL(50, jc.spike_thresh);
    TEST_ASSERT_EQUAL(80, jc.sustained_thresh);   /* 60 + 20, above spike */
}

/* While the baseline is still learning there is no measurement to floor on, so
 * it falls back to the factory relationship rather than to a low setting. */
void test_unlearned_baseline_falls_back_high_not_low(void) {
    motor_load_motor_started(1000);
    uint8_t b = 0;
    TEST_ASSERT_FALSE(motor_load_get_baseline(&b));

    jam_load_config_t jc;
    jam_get_load_config(20, &jc);
    /* JAM_LOAD_THRESHOLD - margin = 75, NOT 5. */
    TEST_ASSERT_EQUAL(75, jc.sustained_thresh);

    jam_get_load_config(90, &jc);
    TEST_ASSERT_EQUAL(75, jc.sustained_thresh);
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

/* A grab is a step the spindle cannot absorb: CV falls out of regulation.
 *
 * Note what this does NOT test, despite the name it used to have: 45 and 46
 * are below JAM_STEP_ABS_FLOOR, so no slam is seen and the trip comes from two
 * DROOP samples alone - it would pass with step_thr set to 99. The step
 * threshold's own coverage is test_step_at_high_load_trips_without_droop and
 * test_still_up_requires_the_absolute_floor_not_just_the_step. */
void test_droop_under_load_trips_after_confirmation(void) {
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
 * droop land in different samples. An AND of the two within one poll would
 * have meant the detector never fired on a real grab.
 *
 * Since the counters were split, this is specifically the DROOP path reaching
 * its own two samples while the load climbs; the loads here are all below
 * JAM_STEP_ABS_FLOOR, so no slam contributes. That separation is the point -
 * see test_a_droop_sample_cannot_confirm_a_cancelled_slam. */
void test_grab_trips_when_droop_follows_the_load_rise(void) {
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

/* REVIEW FINDING 2026-10-14: the still_up cancel path had no test at all, and
 * the one test named for it was really exercising JAM_GRAB_MIN_LOAD. A slam
 * that collapses must cancel, not confirm. */
void test_slam_that_collapses_cancels_instead_of_confirming(void) {
    prime_baseline_at(10);
    feed_step(50, 1000, 27);
    TEST_ASSERT_FALSE(feed_step(82, 1000, 27));   /* slam: +32, lands >= 80 */
    TEST_ASSERT_TRUE(step_pending);
    /* Breakthrough: the load is gone on the next sample, speed never sagged. */
    TEST_ASSERT_FALSE(feed_step(40, 1000, 27));
    TEST_ASSERT_FALSE(step_pending);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* ...and still_up must test the absolute floor, not just the step height.
 * With a low Step Thresh, `step_from + step_threshold` alone let a sample most
 * of the way back down to free-run confirm a slam. */
void test_still_up_requires_the_absolute_floor_not_just_the_step(void) {
    prime_baseline_at(10);
    feed_step(50, 1000, 5);
    TEST_ASSERT_FALSE(feed_step(82, 1000, 5));    /* slam at StepTh=5 */
    TEST_ASSERT_TRUE(step_pending);
    /* 60 >= step_from(50) + 5, but far below JAM_STEP_ABS_FLOOR. Not slammed
     * any more, and the spindle is on speed, so this must not confirm. */
    TEST_ASSERT_FALSE(feed_step(60, 1000, 5));
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* A run whose CV never comes up must not read as a permanent grab: CV=0
 * against SV=1000 satisfies cv + tol < sv trivially, and the load is above the
 * grab minimum. Observed for real on 2026-10-13, when every start at SV=500
 * reported CV=0 for the whole run while the spindle turned.
 *
 * This no longer isolates cv_seen_valid specifically - since the arming gate
 * went banded, CV=0 fails that too, and either one alone is sufficient. The
 * invariant between them is pinned by the test below instead. */
void test_a_run_with_no_cv_reading_is_not_a_grab(void) {
    for (int i = 0; i < 10; i++) {
        bool jammed = feed_step(30, 0 /* CV */, 27);
        TEST_ASSERT_FALSE(jammed);
    }
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* cv_seen_valid is now IMPLIED by the at-target gate on the droop path: the
 * gate will not arm below JAM_CV_VALID_RPM, which is the same threshold that
 * sets cv_seen_valid. The explicit check is kept because it is free and states
 * the intent, but the implication is the thing that must not silently break -
 * these gates have drifted apart from each other once already. (low-load still
 * needs cv_seen_valid on its own account; nothing implies it there.) */
void test_at_target_cannot_arm_without_a_valid_cv_reading(void) {
    motor_load_motor_started(50);     /* SV=50: the 60 rpm floor puts 0 in band */
    tick_advance(501);
    for (int i = 0; i < MLOAD_AT_TARGET_SAMPLES * 2; i++) {
        motor_load_update(30, 0 /* CV */, 50, true);
        TEST_ASSERT_FALSE(motor_load_at_target_seen());
    }
    TEST_ASSERT_FALSE(jam_cv_feedback_seen());

    /* And the moment CV is real, both become true together. */
    for (int i = 0; i < MLOAD_AT_TARGET_SAMPLES; i++) {
        motor_load_update(30, 50, 50, true);
    }
    jam_load_update(true, true, true, 95, 27, false, 0);
    TEST_ASSERT_TRUE(motor_load_at_target_seen());
    TEST_ASSERT_TRUE(jam_cv_feedback_seen());
}

/*===========================================================================*/
/* 6b-2. Findings from the 2026-10-14 review of the step/droop paths          */
/*===========================================================================*/

/* The two paths used to share one confirm counter, so a SLAM set it to 1 and
 * any DROOP sample finished the job - two different claims about two different
 * samples adding up to a trip neither of them made.
 *
 * The case that matters is a breakthrough. 40 -> 82 is a slam; the bit then
 * breaks through, load collapses to 30, but CV is up to 6 polls stale and
 * still reports the speed the spindle had while it WAS loaded. Under the shared
 * counter that collapsed sample confirmed the cancelled slam. */
void test_a_droop_sample_cannot_confirm_a_cancelled_slam(void) {
    prime_baseline_at(10);
    feed_step(40, 1000, 27);
    TEST_ASSERT_FALSE(feed_step(82, 1000, 27));   /* slam, confirm 1 of 2 */
    TEST_ASSERT_TRUE(step_pending);

    /* Breakthrough: load gone, CV still reporting the loaded speed. */
    bool jammed = feed_step(30, 850, 27);
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_FALSE(step_pending);              /* the slam cancelled... */
    TEST_ASSERT_EQUAL(1, droop_confirm);          /* ...and droop starts at 1 */
    TEST_ASSERT_EQUAL(0, slam_confirm);
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* JAM_GRAB_MIN_LOAD is 25 and free-run KR is 24-25% at 1250 rpm, so the
 * absolute qualifier admits a spindle pulling nothing at all - and above
 * 1250 it stops discriminating entirely. Same speed-dependence that broke the
 * sustained tier, same fix: floor it on the learned baseline. */
void test_grab_min_load_is_floored_on_the_learned_baseline(void) {
    prime_baseline_at(25);          /* free-run at 1250 as measured */
    jam_load_config_t jc;
    jam_get_load_config(90, &jc);
    TEST_ASSERT_EQUAL(35, jc.grab_min_load);      /* 25 + 10, not 25 */

    /* A spindle drooping at just-above-free-run load is not grabbing. */
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_FALSE(feed_step(30, 700, 27));
    }
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);

    /* Real cutting load with the same droop still trips. */
    TEST_ASSERT_FALSE(feed_step(60, 700, 27));
    TEST_ASSERT_TRUE(feed_step(61, 700, 27));
    TEST_ASSERT_EQUAL(JAM_LOAD_STEP, jam_get_status()->type);
}

/* A small baseline must not drag the qualifier BELOW the absolute floor. */
void test_grab_min_load_never_drops_below_the_absolute_floor(void) {
    prime_baseline_at(5);
    jam_load_config_t jc;
    jam_get_load_config(90, &jc);
    TEST_ASSERT_EQUAL(JAM_GRAB_MIN_LOAD, jc.grab_min_load);   /* 5+10 -> 25 */
}

/* The raw step delta is the OEM's ONLY load-jam detector; the absolute ceiling
 * is ours. Hanging the OEM-parity detector off the switch for our addition
 * meant "Spike Detect off" removed more protection than it names. */
void test_step_detector_survives_spike_detect_being_off(void) {
    prime_baseline_at(10);
    motor_load_update(50, 1000, 1000, true);
    jam_load_update(true, true, false /* spike OFF */, 95, 27, false, 0);
    motor_load_update(82, 1000, 1000, true);
    bool jammed = jam_load_update(true, true, false, 95, 27, false, 0);
    TEST_ASSERT_FALSE(jammed);                    /* one sample is not enough */
    motor_load_update(83, 1000, 1000, true);
    jammed = jam_load_update(true, true, false, 95, 27, false, 0);
    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOAD_STEP, jam_get_status()->type);
}

/* ...but Step Thresh 0 still switches it off, and Jam Detect still masters it. */
void test_step_detector_still_obeys_its_own_switches(void) {
    prime_baseline_at(10);
    motor_load_update(50, 1000, 1000, true);
    jam_load_update(true, true, true, 95, 0 /* StepTh = off */, false, 0);
    motor_load_update(82, 1000, 1000, true);
    TEST_ASSERT_FALSE(jam_load_update(true, true, true, 95, 0, false, 0));
    motor_load_update(83, 1000, 1000, true);
    TEST_ASSERT_FALSE(jam_load_update(true, true, true, 95, 0, false, 0));
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
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
    for (int i = 0; i < MLOAD_AT_TARGET_SAMPLES; i++) {
        motor_load_update(25, 1250, 1250, true);
    }
    tick_advance(2001);
    motor_load_update(25, 1250, 1250, true);
    TEST_ASSERT_TRUE(motor_load_at_target_seen());

    /* Dial sweep: down 200, then up 400. */
    motor_load_motor_speed_change(1250, 1050);
    motor_load_motor_speed_change(1050, 1450);
    TEST_ASSERT_FALSE(motor_load_at_target_seen());

    /* Grace runs out while CV is still the pre-step reading. Asserted, not
     * assumed: this lands 1 ms past the deadline, and a test that is still
     * inside grace passes because NO detector ran - which is exactly how one
     * of these went vacuous on 2026-10-14. */
    tick_advance(501);
    TEST_ASSERT_FALSE(motor_load_in_spike_grace());

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
    TEST_ASSERT_FALSE(motor_load_in_spike_grace());

    /* Ramp: well under speed, real cutting load, no trip and no arming. */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_FALSE(feed_step_sv(30, 1250, 1450, 27));
    }
    TEST_ASSERT_FALSE(motor_load_at_target_seen());

    /* Arrives at the new setpoint and holds it for the dwell. */
    for (int i = 0; i < MLOAD_AT_TARGET_SAMPLES; i++) {
        TEST_ASSERT_FALSE(feed_step_sv(30, 1445, 1450, 27));
    }
    TEST_ASSERT_TRUE(motor_load_at_target_seen());

    /* NOW it loses the speed under load: that is a grab. */
    TEST_ASSERT_FALSE(feed_step_sv(45, 1200, 1450, 27));   /* sample 1 */
    bool jammed = feed_step_sv(46, 1180, 1450, 27);        /* sample 2 */
    TEST_ASSERT_TRUE(jammed);
    TEST_ASSERT_EQUAL(JAM_LOAD_STEP, jam_get_status()->type);
}

/* FINDING FROM THE 2026-10-14 REVIEW, and the other half of the stale-CV
 * problem. Clearing the gate on a speed change is not enough on its own: the
 * next poll still carries the PRE-change CV, because the MCB refreshes CV only
 * every ~6 polls. Sweep the dial down and straight back up and that stale
 * reading is in band for the speed you just returned to, so a single-sample
 * latch armed the gate while the spindle was really still decelerating. */
void test_stale_cv_cannot_arm_the_gate_on_a_dial_return(void) {
    motor_load_motor_started(1250);
    for (int i = 0; i < MLOAD_AT_TARGET_SAMPLES; i++) {
        motor_load_update(30, 1240, 1250, true);
    }
    TEST_ASSERT_TRUE(motor_load_at_target_seen());
    tick_advance(2001);

    /* Dial down to 1050 and immediately back to 1250. */
    motor_load_motor_speed_change(1250, 1050);
    motor_load_motor_speed_change(1050, 1250);
    TEST_ASSERT_FALSE(motor_load_at_target_seen());
    tick_advance(1251);
    TEST_ASSERT_FALSE(motor_load_in_spike_grace());

    /* One stale poll: CV is still the pre-change 1240, in band for 1250. */
    TEST_ASSERT_FALSE(feed_step_sv(30, 1240, 1250, 27));
    TEST_ASSERT_FALSE(motor_load_at_target_seen());   /* dwell not satisfied */

    /* The fresh reading arrives and the spindle really did slow down. Under
     * the old single-sample latch this was two qualified droop samples and an
     * emergency stop on a spindle on its way back up. */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_FALSE(feed_step_sv(30, 1100, 1250, 27));
    }
    TEST_ASSERT_EQUAL(JAM_NONE, jam_get_status()->type);
    TEST_ASSERT_FALSE(emergency_stop_called);
}

/* FINDING FROM THE 2026-10-14 REVIEW. Arming used motor_load's +/-5% baseline
 * window while tripping used jam's +/-10% band, so a spindle that settled
 * between the two - an ordinary heavy cut - never armed the gate, and the
 * droop detector was disarmed for the whole run. Both now use jam.h's band. */
void test_a_spindle_inside_the_band_but_not_at_setpoint_still_arms(void) {
    motor_load_motor_started(1000);
    tick_advance(1001);
    TEST_ASSERT_FALSE(motor_load_in_spike_grace());

    /* 930 rpm against 1000: 7% low. Outside the +/-5% baseline window, well
     * inside the 10% regulation band. This is a loaded cut holding speed. */
    for (int i = 0; i < MLOAD_AT_TARGET_SAMPLES; i++) {
        TEST_ASSERT_FALSE(feed_step_sv(55, 930, 1000, 27));
    }
    TEST_ASSERT_TRUE(motor_load_at_target_seen());
    /* The baseline, which uses the tighter window, is legitimately still not
     * armed - the two gates answer different questions. */
    uint8_t b;
    TEST_ASSERT_FALSE(motor_load_get_baseline(&b));

    /* So a real grab from here is caught, where before it was invisible. */
    TEST_ASSERT_FALSE(feed_step_sv(72, 450, 1000, 27));
    TEST_ASSERT_TRUE(feed_step_sv(72, 450, 1000, 27));
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
    TEST_ASSERT_FALSE(motor_load_in_spike_grace());

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
    for (int i = 0; i < MLOAD_AT_TARGET_SAMPLES; i++) {
        motor_load_update(25, 1000, 1000, true);
    }
    TEST_ASSERT_TRUE(motor_load_at_target_seen());
    motor_load_motor_speed_change(1000, 600);
    TEST_ASSERT_FALSE(motor_load_at_target_seen());
}

/* REVIEW FINDING 2026-10-14: the t_ms column was the sample INDEX times the
 * nominal 50 ms poll, and the samples are not evenly spaced - the real running
 * poll is ~85 ms, a lost reply adds a 250 ms timeout, and on a stall the loop
 * drops to the 500 ms idle interval while still recording. Each entry now
 * carries its own measured gap. */
void test_each_trace_sample_carries_its_own_measured_gap(void) {
    motor_load_trace_reset();
    motor_load_update(20, 1000, 1000, true);      /* first: no gap to measure */
    tick_advance(85);
    motor_load_update(21, 1000, 1000, true);
    tick_advance(500);                            /* a stall-branch poll */
    motor_load_update(22, 1000, 1000, true);

    motor_load_trace_entry_t e;
    TEST_ASSERT_TRUE(motor_load_trace_get(0, &e));
    TEST_ASSERT_EQUAL(0, e.dt_ms);
    TEST_ASSERT_FALSE(e.flags & MLOAD_TRACE_F_DTSAT);

    TEST_ASSERT_TRUE(motor_load_trace_get(1, &e));
    TEST_ASSERT_EQUAL(85, e.dt_ms);
    TEST_ASSERT_FALSE(e.flags & MLOAD_TRACE_F_DTSAT);

    /* 500 ms does not fit in a uint8: capped, and SAID to be capped, so the
     * number cannot be read as a real 255 ms poll. */
    TEST_ASSERT_TRUE(motor_load_trace_get(2, &e));
    TEST_ASSERT_EQUAL(255, e.dt_ms);
    TEST_ASSERT_TRUE(e.flags & MLOAD_TRACE_F_DTSAT);
}

/* The snapshot must stay self-consistent: whatever count it reports has to be
 * readable against the base it reported, even as the recorder keeps pushing. */
void test_trace_snapshot_is_readable_to_its_own_count(void) {
    motor_load_trace_reset();
    for (int i = 0; i < 12; i++) {
        tick_advance(85);
        motor_load_update((uint8_t)(30 + i), 1000, 1000, true);
    }
    uint16_t n = 0, base = 0;
    motor_load_trace_snapshot(&n, &base);
    TEST_ASSERT_EQUAL(12, n);

    motor_load_trace_entry_t e;
    for (uint16_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(motor_load_trace_get_at(base, i, &e));
    }
    /* Oldest first. */
    TEST_ASSERT_TRUE(motor_load_trace_get_at(base, 0, &e));
    TEST_ASSERT_EQUAL(30, e.kr);
    TEST_ASSERT_TRUE(motor_load_trace_get_at(base, n - 1, &e));
    TEST_ASSERT_EQUAL(41, e.kr);
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
/* Named for the step path, but 45 is below JAM_STEP_ABS_FLOOR so no step is
 * ever seen here: what it actually pins down is that a DROOP stops qualifying
 * once the load falls under the grab minimum. Kept, and renamed to say so -
 * the slam-cancel case it was credited with is covered by
 * test_slam_that_collapses_cancels_instead_of_confirming. */
void test_droop_stops_qualifying_when_the_load_goes_away(void) {
    prime_baseline_at(10);
    feed_step(11, 1000, 27);
    bool jammed = feed_step(45, 700, 27);    /* drooping at cutting load */
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(1, droop_confirm);
    jammed = feed_step(9, 700, 27);          /* load gone, spindle still slow */
    TEST_ASSERT_FALSE(jammed);
    TEST_ASSERT_EQUAL(0, droop_confirm);     /* below the grab minimum */
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
    TEST_ASSERT_FALSE(motor_load_in_spike_grace());
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
    RUN_TEST(test_sustained_tier_cannot_trip_at_free_run_load);
    RUN_TEST(test_baseline_floor_does_not_move_the_factory_threshold);
    RUN_TEST(test_sustained_floor_is_not_clamped_to_a_nonsensical_ceiling);
    RUN_TEST(test_unlearned_baseline_falls_back_high_not_low);
    RUN_TEST(test_step_without_droop_no_jam);
    RUN_TEST(test_droop_under_load_trips_after_confirmation);
    RUN_TEST(test_grab_trips_when_droop_follows_the_load_rise);
    RUN_TEST(test_step_at_high_load_trips_without_droop);
    RUN_TEST(test_slam_that_collapses_cancels_instead_of_confirming);
    RUN_TEST(test_a_droop_sample_cannot_confirm_a_cancelled_slam);
    RUN_TEST(test_grab_min_load_is_floored_on_the_learned_baseline);
    RUN_TEST(test_grab_min_load_never_drops_below_the_absolute_floor);
    RUN_TEST(test_step_detector_survives_spike_detect_being_off);
    RUN_TEST(test_step_detector_still_obeys_its_own_switches);
    RUN_TEST(test_still_up_requires_the_absolute_floor_not_just_the_step);
    RUN_TEST(test_step_ignores_dropout_sample);
    RUN_TEST(test_droop_stops_qualifying_when_the_load_goes_away);
    RUN_TEST(test_step_threshold_zero_disables_detector);
    RUN_TEST(test_spike_trips_with_unarmed_baseline);

    /* CV credibility */
    RUN_TEST(test_low_load_holds_off_until_cv_has_read_something);
    RUN_TEST(test_low_load_trips_once_cv_has_been_valid);
    RUN_TEST(test_a_run_with_no_cv_reading_is_not_a_grab);
    RUN_TEST(test_at_target_cannot_arm_without_a_valid_cv_reading);
    RUN_TEST(test_speed_step_up_mid_run_is_not_a_grab);
    RUN_TEST(test_droop_arms_once_the_new_speed_is_reached);
    RUN_TEST(test_spin_up_under_cutting_load_is_not_a_grab);
    RUN_TEST(test_speed_step_down_clears_the_at_target_gate);
    RUN_TEST(test_stale_cv_cannot_arm_the_gate_on_a_dial_return);
    RUN_TEST(test_a_spindle_inside_the_band_but_not_at_setpoint_still_arms);

    /* Flight recorder */
    RUN_TEST(test_each_trace_sample_carries_its_own_measured_gap);
    RUN_TEST(test_trace_snapshot_is_readable_to_its_own_count);
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
