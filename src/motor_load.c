/**
 * @file motor_load.c
 * @brief Motor load (KR) filter, idle-baseline learning, inrush/stability windows.
 *
 * See motor_load.h for the contract. All state is module-local and accessed
 * from the motor task only — no mutex.
 */

#include "motor_load.h"

// Two-tier load characterization parameters.
//
// EMA filter on raw KR — kills single-sample MCB glitches without slowing real
// spike response. new = (old*7 + raw)/8 ≈ 100 ms time constant at 20 Hz.
#define MLOAD_EMA_ALPHA           8

// Inrush grace: defers the spike-detector during motor spin-up and step-ups.
// 1 ms per RPM of delta, floored at 500 ms. At 150 RPM → 500 ms; 5500 → 5500 ms.
#define MLOAD_SPIKE_GRACE_FLOOR_MS    500
#define MLOAD_SPIKE_GRACE_PER_RPM_MS    1

// Stability window required to latch baseline. Floor 2000 ms, +1 ms per RPM
// of step delta — same shape as inrush grace (higher delta needs more time).
#define MLOAD_STABLE_FLOOR_MS     2000
#define MLOAD_STABLE_PER_RPM_MS      1

// CV must be within ±MLOAD_BASELINE_CV_TOL_PCT of SV to count as "at target".
#define MLOAD_BASELINE_CV_TOL_PCT    5

static uint8_t  filtered_load = 0;
static bool     filter_initialized = false;
static uint8_t  raw_last = 0;             // Most recent raw KR sample
static uint8_t  raw_prev = 0;             // Previous raw KR sample (one cycle behind)
static bool     prev_valid = false;       // True after second sample arrives
static uint8_t  load_baseline = 0;
static bool     baseline_armed = false;
static uint32_t spike_grace_end = 0;        // HAL_GetTick deadline; 0 = expired/none
static uint32_t stable_since = 0;           // HAL_GetTick when CV first entered window
static uint32_t stability_required_ms = 0;
static uint16_t last_cv = 0;              // CV that came with the last sample
static uint16_t last_sv = 0;              // SV that came with the last sample

/* Jam flight recorder — see motor_load.h. Written once per poll from the motor
 * task, read from the console task via motor_load_trace_get(). Both run under
 * the scheduler and the buffer is only ever appended to, so a torn read shows
 * one stale sample rather than corrupt state; that is cheaper than taking a
 * lock on the 20 Hz hot path for a diagnostic.
 *
 * The buffer lives in .noinit (NOLOAD — see ldscript.ld) so that startup does
 * NOT clear it. This is not a micro-optimisation: on 2026-10-13 a jam trip on
 * the real machine coincided with an NRST reset, and the recorder lost the only
 * recording of the only event it exists to record. SRAM keeps its contents
 * across a pin, soft or watchdog reset — the C startup is the only thing that
 * erases it — so surviving one is purely a matter of not being zeroed. A cold
 * boot leaves indeterminate bytes here instead, which is what the magic word
 * and the range checks in trace_boot_recover() are for. */
#define MLOAD_NOINIT __attribute__((section(".noinit")))
#define MLOAD_TRACE_MAGIC 0x4A54524Eu   /* 'JTRN' */

MLOAD_NOINIT static uint32_t trace_magic;
MLOAD_NOINIT static motor_load_trace_entry_t trace_buf[MLOAD_TRACE_LEN];
MLOAD_NOINIT static uint16_t trace_head;        /* next slot to write */
MLOAD_NOINIT static uint16_t trace_count;       /* 0..MLOAD_TRACE_LEN */
MLOAD_NOINIT static bool     trace_frozen;
MLOAD_NOINIT static uint32_t trace_frozen_tick;
MLOAD_NOINIT static uint8_t  trace_origin;      /* motor_load_trace_origin_t */

/* Set at boot, so .bss is right: it describes THIS boot's recovery, and must
 * not itself survive into the next one. */
static bool trace_rebooted = false;

static void trace_push(uint8_t kr, uint16_t cv, uint16_t sv) {
    if (trace_frozen) return;
    /* The index is used before the wrap below, and this buffer is .noinit — so
     * an unvalidated head is an out-of-bounds WRITE, not just a lost sample.
     * trace_boot_recover() is what normally guarantees the range; this is here
     * so that a missed init path can never corrupt RAM again. */
    if (trace_head >= MLOAD_TRACE_LEN) trace_head = 0;
    motor_load_trace_entry_t *e = &trace_buf[trace_head];
    e->kr       = kr;
    e->filtered = filtered_load;
    e->flags    = (uint8_t)((baseline_armed ? MLOAD_TRACE_F_ARMED : 0) |
                            (motor_load_in_spike_grace() ? MLOAD_TRACE_F_GRACE : 0));
    e->baseline = load_baseline;
    e->cv       = cv;
    e->sv       = sv;
    trace_head = (uint16_t)((trace_head + 1) % MLOAD_TRACE_LEN);
    if (trace_count < MLOAD_TRACE_LEN) trace_count++;
}

static uint32_t grace_for_delta(uint16_t delta_rpm) {
    uint32_t g = (uint32_t)delta_rpm * MLOAD_SPIKE_GRACE_PER_RPM_MS;
    return (g > MLOAD_SPIKE_GRACE_FLOOR_MS) ? g : MLOAD_SPIKE_GRACE_FLOOR_MS;
}

static uint32_t stability_for_delta(uint16_t delta_rpm) {
    uint32_t s = (uint32_t)delta_rpm * MLOAD_STABLE_PER_RPM_MS;
    return (s > MLOAD_STABLE_FLOOR_MS) ? s : MLOAD_STABLE_FLOOR_MS;
}

/* Decide, once per boot, whether what is sitting in .noinit is a real trace
 * from before a reset or just uninitialised SRAM. */
static void trace_boot_recover(void) {
    const bool sane = (trace_magic == MLOAD_TRACE_MAGIC) &&
                      (trace_head  <  MLOAD_TRACE_LEN) &&
                      (trace_count <= MLOAD_TRACE_LEN);

    if (!sane || trace_count == 0) {
        /* Cold boot, a corrupted buffer, or nothing recorded. Start clean. */
        trace_magic = MLOAD_TRACE_MAGIC;
        motor_load_trace_reset();
        return;
    }

    /* Samples survived a reset. Hold them for readout however they got here:
     * an already-frozen buffer is a trip that was followed by a reset, and a
     * live one is a reset that happened mid-cut with no trip at all — which is
     * itself the more interesting recording of the two, and the one that was
     * previously thrown away. */
    trace_rebooted = true;
    if (!trace_frozen) {
        trace_frozen = true;
        trace_origin = MLOAD_TRACE_RESET;
    }
    /* The pre-reset tick counter means nothing against this boot's clock. */
    trace_frozen_tick = 0;
}

void motor_load_init(void) {
    trace_boot_recover();
    filtered_load = 0;
    filter_initialized = false;
    raw_last = 0;
    raw_prev = 0;
    prev_valid = false;
    load_baseline = 0;
    baseline_armed = false;
    spike_grace_end = 0;
    stable_since = 0;
    stability_required_ms = 0;
}

void motor_load_motor_started(uint16_t target_rpm) {
    /* A frozen trace is a captured trip waiting to be read. Restarting the
     * spindle is the first thing an operator does after one, so keep it and
     * leave the recorder blind until JAMTRACE has had it. */
    if (!trace_frozen) motor_load_trace_reset();
    spike_grace_end = HAL_GetTick() + grace_for_delta(target_rpm);
    stability_required_ms = stability_for_delta(target_rpm);
    filter_initialized = false;
    filtered_load = 0;
    raw_last = 0;
    raw_prev = 0;
    prev_valid = false;
    load_baseline = 0;
    baseline_armed = false;
    stable_since = 0;
}

void motor_load_motor_speed_change(uint16_t old_target, uint16_t new_target) {
    if (new_target == old_target) return;
    uint16_t delta = (new_target > old_target) ? (uint16_t)(new_target - old_target)
                                               : (uint16_t)(old_target - new_target);
    // Step-up extends inrush grace (step-down has no inrush, leave grace).
    if (new_target > old_target) {
        uint32_t new_end = HAL_GetTick() + grace_for_delta(delta);
        /* REVIEW FIX: plain `>` is an unsigned compare across the 49.7-day
         * HAL_GetTick wrap — motor_load_in_spike_grace() below was fixed for
         * exactly this and this sibling was missed. A speed step-up just before
         * the wrap compared a small new_end against a huge spike_grace_end and
         * silently declined to extend the inrush grace. */
        if ((int32_t)(new_end - spike_grace_end) > 0) spike_grace_end = new_end;
    }
    // Any speed change invalidates the learned baseline (new operating point).
    // EMA continues — load changes smoothly through the transition.
    baseline_armed = false;
    load_baseline = 0;
    stable_since = 0;
    stability_required_ms = stability_for_delta(delta);
}

void motor_load_motor_stopped(void) {
    last_cv = 0;
    last_sv = 0;
    spike_grace_end = 0;
    stable_since = 0;
    stability_required_ms = 0;
    filter_initialized = false;
    filtered_load = 0;
    raw_last = 0;
    raw_prev = 0;
    prev_valid = false;
    load_baseline = 0;
    baseline_armed = false;
}

void motor_load_update(uint8_t raw_load, uint16_t cv, uint16_t sv, bool is_running) {
    last_cv = cv;
    last_sv = sv;

    if (!is_running) {
        filter_initialized = false;
        filtered_load = 0;
        raw_last = 0;
        raw_prev = 0;
        prev_valid = false;
        baseline_armed = false;
        load_baseline = 0;
        stable_since = 0;
        return;
    }

    // Track raw + previous for OEM-style step-delta detection.
    if (filter_initialized) {
        raw_prev = raw_last;
        prev_valid = true;
    }
    raw_last = raw_load;

    if (!filter_initialized) {
        filtered_load = raw_load;
        filter_initialized = true;
    } else {
        // AUDIT FIX (LOW, motor_load.c:131): add half-divisor to the numerator
        // for round-to-nearest instead of floor. Old code left filtered_load
        // stuck ~7 points below a steady raw KR (100 → 93), which under-read
        // the load bar at high load and required ~7 extra percentage points
        // of real load to trip the absolute-cap paths in jam.c.
        filtered_load = (uint8_t)(((uint16_t)filtered_load * (MLOAD_EMA_ALPHA - 1)
                                   + raw_load + (MLOAD_EMA_ALPHA / 2)) / MLOAD_EMA_ALPHA);
    }

    if (!baseline_armed && sv > 0) {
        uint16_t tol = (uint16_t)(((uint32_t)sv * MLOAD_BASELINE_CV_TOL_PCT) / 100);
        if (tol < 25) tol = 25;
        uint16_t lo = (sv > tol) ? (uint16_t)(sv - tol) : 0;
        uint16_t hi = (uint16_t)(sv + tol);
        bool in_window = (cv >= lo) && (cv <= hi);
        if (in_window) {
            uint32_t now_ms = HAL_GetTick();
            if (stable_since == 0) {
                stable_since = now_ms;
            } else if ((now_ms - stable_since) >= stability_required_ms) {
                load_baseline = filtered_load;
                baseline_armed = true;
            }
        } else {
            stable_since = 0;
        }
    }

    trace_push(raw_load, cv, sv);
}

uint8_t motor_load_get_filtered(void) {
    return filtered_load;
}

uint8_t motor_load_get_raw(void) {
    return raw_last;
}

uint8_t motor_load_get_raw_prev(void) {
    return prev_valid ? raw_prev : 0;
}

void motor_load_get_speeds(uint16_t *cv, uint16_t *sv) {
    if (cv) *cv = last_cv;
    if (sv) *sv = last_sv;
}

int8_t motor_load_get_step_delta(void) {
    if (!prev_valid) return 0;
    int16_t d = (int16_t)raw_last - (int16_t)raw_prev;
    if (d > 100) d = 100;
    if (d < -100) d = -100;
    return (int8_t)d;
}

bool motor_load_get_baseline(uint8_t *out) {
    if (!baseline_armed) return false;
    if (out) *out = load_baseline;
    return true;
}

bool motor_load_in_spike_grace(void) {
    // AUDIT FIX (LOW, motor_load.c:178): use signed subtraction so this is
    // safe across the 49.7-day HAL_GetTick wraparound. On a continuously-
    // powered machine, a start just before wrap would otherwise compute a
    // grace_end < now for the whole spin-up window and let the low-load
    // detector false-trip during the CV<25 ramp.
    if (spike_grace_end == 0) return false;
    return (int32_t)(spike_grace_end - HAL_GetTick()) > 0;
}

void motor_load_get_debug(motor_load_debug_t *out) {
    if (!out) return;
    uint32_t now = HAL_GetTick();
    out->filtered_load = filtered_load;
    out->baseline = load_baseline;
    out->baseline_armed = baseline_armed;
    out->filter_initialized = filter_initialized;
    out->spike_grace_remaining_ms = (now < spike_grace_end) ? (spike_grace_end - now) : 0;
    out->stability_elapsed_ms = (stable_since > 0) ? (now - stable_since) : 0;
    out->stability_required_ms = stability_required_ms;
}

/*===========================================================================*/
/* Load trace                                                                */
/*===========================================================================*/

void motor_load_trace_freeze(void) {
    if (trace_frozen) return;        /* keep the first trip, not the last */
    trace_frozen = true;
    trace_frozen_tick = HAL_GetTick();
    trace_origin = MLOAD_TRACE_TRIP;
}

void motor_load_trace_reset(void) {
    trace_magic = MLOAD_TRACE_MAGIC;
    trace_head = 0;
    trace_count = 0;
    trace_frozen = false;
    trace_frozen_tick = 0;
    trace_origin = MLOAD_TRACE_LIVE;
    trace_rebooted = false;
}

uint8_t motor_load_trace_origin(void) {
    return trace_origin;
}

bool motor_load_trace_rebooted(void) {
    return trace_rebooted;
}

bool motor_load_trace_is_frozen(void) {
    return trace_frozen;
}

uint32_t motor_load_trace_frozen_at(void) {
    return trace_frozen_tick;
}

uint16_t motor_load_trace_count(void) {
    return trace_count;
}

bool motor_load_trace_get(uint16_t i, motor_load_trace_entry_t *out) {
    if (!out || i >= trace_count) return false;
    /* Oldest-first: once the buffer has wrapped, trace_head is the oldest. */
    uint16_t base = (trace_count == MLOAD_TRACE_LEN) ? trace_head : 0;
    *out = trace_buf[(base + i) % MLOAD_TRACE_LEN];
    return true;
}
