/**
 * @file motor_load.h
 * @brief Motor load (KR) filter + idle-baseline learning + inrush/stability windows.
 *
 * Service module: characterizes the motor's load signal so consumers
 * (jam detection, display split-bar, tapping triggers) can act on smoothed,
 * baseline-relative numbers instead of raw single samples.
 *
 *  raw KR ──► EMA filter ──► filtered_load ──┬─► jam.c (spike + sustained)
 *                                            ├─► display.c (cutting = load - baseline)
 *                                            └─► (future) tapping
 *
 * Lifecycle:
 *   motor_load_motor_started(target)         on motor START
 *   motor_load_motor_speed_change(old, new)  on SET_SPEED while running
 *   motor_load_motor_stopped()               on motor STOP
 *   motor_load_update(raw, cv, sv, running)  every motor poll cycle
 */

#ifndef MOTOR_LOAD_H
#define MOTOR_LOAD_H

#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

void motor_load_init(void);

/**
 * @brief Arm inrush grace + stability window for a fresh motor run.
 *        Resets filter and baseline.
 */
void motor_load_motor_started(uint16_t target_rpm);

/**
 * @brief Step-up extends inrush grace; either direction invalidates the
 *        baseline (new operating point). Filter is kept continuous.
 */
void motor_load_motor_speed_change(uint16_t old_target, uint16_t new_target);

/**
 * @brief Clear all per-run state. Filter and baseline reset.
 */
void motor_load_motor_stopped(void);

/**
 * @brief Feed a KR sample. Advances EMA filter and baseline-learning state.
 *
 * @param raw_load   Raw KR percent (0-100)
 * @param cv         Current motor RPM (used to detect baseline-ready window)
 * @param sv         Target motor RPM (used as the ±tolerance center)
 * @param is_running True while motor is commanded and running
 */
void motor_load_update(uint8_t raw_load, uint16_t cv, uint16_t sv, bool is_running);

/** @return EMA-filtered load percent (0 before first sample / after stop). */
uint8_t motor_load_get_filtered(void);

/** @return Last raw KR sample fed to motor_load_update (0 if none). */
uint8_t motor_load_get_raw(void);

/** @return The raw KR sample before the last one (0 if none yet).
 *  The step detector needs it: the OEM test required both samples to be
 *  non-zero so a dropout could not read as a full-load step. */
uint8_t motor_load_get_raw_prev(void);

/**
 * @brief The CV/SV pair that arrived with the last load sample.
 *
 * The commanded speed as the motor task actually sent it, not the operator's
 * dial — the step detector compares the two to decide whether the spindle is
 * holding the speed it was told to hold.
 */
void motor_load_get_speeds(uint16_t *cv, uint16_t *sv);

/**
 * @brief Signed step delta between the most recent two raw KR samples.
 *        Positive = load rose this poll cycle. Zero on the very first sample
 *        after motor start (no previous to compare).
 */
int8_t motor_load_get_step_delta(void);

/**
 * @brief Read the learned idle-load baseline.
 * @param[out] out  Baseline percent when armed; untouched otherwise.
 * @return true if armed, false if still learning.
 */
bool motor_load_get_baseline(uint8_t *out);

/** @return true if the inrush-grace deadline has not passed. */
bool motor_load_in_spike_grace(void);

/**
 * @brief Has the spindle actually REACHED the speed it was last commanded?
 *
 * True once CV has held jam.h's spindle-regulation band - the same band the
 * droop detector trips on - for MLOAD_AT_TARGET_SAMPLES consecutive polls
 * since the last motor start or speed change. False again the instant the
 * setpoint moves in either direction.
 *
 * The dwell is what keeps a STALE CV from arming it: the MCB refreshes CV only
 * every ~6 polls, so a single in-band sample may be a reading taken before the
 * setpoint moved.
 *
 * This is what makes "CV is under SV" mean something. During a ramp CV is
 * legitimately under SV, so a time-based grace window is a guess at how long
 * the ramp takes - and the guess was wrong on the real machine (see jam.c's
 * droop path). Having reached the setpoint once is not a guess: it is the
 * precondition for the sentence "the spindle LOST the speed it was holding".
 */
bool motor_load_at_target_seen(void);

typedef struct {
    uint8_t  filtered_load;
    uint8_t  baseline;
    bool     baseline_armed;
    bool     filter_initialized;
    uint32_t spike_grace_remaining_ms;
    uint32_t stability_elapsed_ms;
    uint32_t stability_required_ms;
} motor_load_debug_t;

/** @brief Diagnostic snapshot — used by LOADINFO / JAMINFO console commands. */
void motor_load_get_debug(motor_load_debug_t *out);

/*===========================================================================*/
/* Load trace (jam flight recorder)                                          */
/*===========================================================================*/

/* Rolling record of the last MLOAD_TRACE_LEN poll samples, so a jam trip can
 * be read back afterwards instead of being reconstructed from the operator's
 * memory of the LCD. 160 samples at the 50 ms running poll = 8 s of history.
 *
 * Why this exists: JAM_LOAD_STEP fires on a single-sample raw delta, and
 * nothing recorded what that sample or its predecessor actually were. Sizing
 * the thresholds — or telling a real grab apart from heavy cutting — needs the
 * samples, and a drill press is not a place to reproduce a trip on demand. */
/* 160 samples = 8 s at the 50 ms running poll, 1280 bytes in .noinit.
 *
 * The games and read-only demo builds cannot afford that. They carry the game
 * task's state plus a 4 KB FreeRTOS heap, which puts their static RAM about
 * 340 bytes from the DFU hand-off word at 0x20004FF0 - and ldscript.ld's
 * ASSERT turns that into a link error rather than a silent overlap, which is
 * exactly what it caught in CI on 2026-10-13. Those builds are not what anyone
 * diagnoses a jam with, so they get a shorter recorder rather than the product
 * build getting a worse one. */
#ifdef BUILD_GAMES
/* 40, not 80, since each entry grew a dt byte on 2026-10-14: at 80 the games
 * and demo builds were left 132 bytes short of the DFU hand-off word, and the
 * product build should not carry a worse recorder to buy headroom for a build
 * that plays Pong. Nobody diagnoses a jam on these. */
#define MLOAD_TRACE_LEN 40
#else
#define MLOAD_TRACE_LEN 160     /* ~13 s at the real ~85 ms running poll */
#endif

#define MLOAD_TRACE_F_ARMED  0x01   /* baseline was armed for this sample */
#define MLOAD_TRACE_F_GRACE  0x02   /* inrush grace still active (detectors muted) */
#define MLOAD_TRACE_F_ATTGT  0x04   /* CV had reached SV at least once by here */
#define MLOAD_TRACE_F_DTSAT  0x08   /* dt_ms saturated - the real gap was longer */

typedef struct {
    uint8_t  kr;        /* raw KR sample as fed to motor_load_update() */
    uint8_t  filtered;  /* EMA output after this sample */
    uint8_t  flags;     /* MLOAD_TRACE_F_* */
    uint8_t  baseline;  /* learned baseline, 0 while learning */
    uint16_t cv;        /* actual RPM at this sample */
    uint16_t sv;        /* commanded RPM at this sample */
    /* Milliseconds since the previous sample, 0 on the first one. Capped at
     * 255 with MLOAD_TRACE_F_DTSAT set when it was.
     *
     * Per-sample and not a single average, because the samples are NOT evenly
     * spaced and no scalar can make them so:
     *   - the running poll is four 9600-baud queries, ~85 ms, not the nominal
     *     50 ms the t_ms column is printed from;
     *   - one lost reply adds a MOTOR_RESPONSE_TIMEOUT_MS (250 ms) stall;
     *   - and on a STALL the poll loop drops to the 500 ms idle interval while
     *     motor_enabled is still true, so the recorder keeps sampling at
     *     500 ms. Those rows are the whole reason the recorder exists, and
     *     they were being printed 10x too close together. */
    uint8_t  dt_ms;
} motor_load_trace_entry_t;

/** @brief Stop recording and keep what is in the buffer. Called on a jam trip. */
void motor_load_trace_freeze(void);

/** @brief Discard the trace and resume recording. */
void motor_load_trace_reset(void);

/** How the held trace came to be held. */
typedef enum {
    MLOAD_TRACE_LIVE  = 0,  /**< Not frozen; recording. */
    MLOAD_TRACE_TRIP  = 1,  /**< Frozen by a jam trip; last sample is the trip. */
    MLOAD_TRACE_RESET = 2,  /**< Was still recording when the board reset. */
} motor_load_trace_origin_t;

/** @return one of motor_load_trace_origin_t. */
uint8_t motor_load_trace_origin(void);

/** @return true if this trace was recovered from .noinit across a reset. */
bool motor_load_trace_rebooted(void);

/** @brief Note that the held trace has been printed. It is kept either way;
 *  this only tells the next motor start that it may be replaced. */
void motor_load_trace_mark_read(void);

/** @return true once the held trace has been printed at least once. */
bool motor_load_trace_was_read(void);

/** @return true while the trace is held for readout. */
bool motor_load_trace_is_frozen(void);

/** @return HAL tick at which the trace was frozen, 0 if never. */
uint32_t motor_load_trace_frozen_at(void);

/**
 * @brief Take a consistent view of the ring buffer for readout.
 *
 * The console task prints 160 rows over ~0.6 s while the motor task keeps
 * pushing at ~85 ms. Recomputing the base index per row made rows repeat and
 * every timestamp after the first shift wrong; copying an 8-byte entry
 * non-atomically out of the slot being overwritten produced rows spliced from
 * two different samples. Snapshot once, then read against the snapshot.
 *
 * A frozen trace is not being written at all, so this costs nothing there; it
 * matters for a LIVE read while the spindle turns.
 *
 * @param[out] count Samples available through motor_load_trace_get_at().
 * @param[out] base  Opaque index of the oldest sample; pass it back unchanged.
 */
void motor_load_trace_snapshot(uint16_t *count, uint16_t *base);

/**
 * @brief Read one sample relative to a snapshot taken by the call above.
 * @return false if i >= the snapshot's count.
 */
bool motor_load_trace_get_at(uint16_t base, uint16_t i,
                             motor_load_trace_entry_t *out);

/** @return number of samples held, 0..MLOAD_TRACE_LEN. */
uint16_t motor_load_trace_count(void);

/**
 * @brief Read one trace sample.
 * @param i    0 = oldest held sample, count-1 = newest.
 * @param out  Destination, untouched when i is out of range.
 * @return false if i >= motor_load_trace_count().
 */
bool motor_load_trace_get(uint16_t i, motor_load_trace_entry_t *out);

#endif /* MOTOR_LOAD_H */
