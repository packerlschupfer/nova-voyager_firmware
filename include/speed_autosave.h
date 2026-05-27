/**
 * @file speed_autosave.h
 * @brief Debounce policy for persisting the last-used spindle speed.
 *
 * Header-only so the timing logic can be unit tested without an EEPROM, a
 * clock, or a drill press — same pattern as safety.h and settings_pack.h.
 *
 * WHY IT EXISTS
 * -------------
 * The encoder writes g_state.target_rpm directly (events.c). The field that
 * actually persists is settings.speed.default_rpm at EE_DEFAULT_RPM (0x32),
 * whose own comment reads "Default/last used speed" — but settings_set_speed()
 * had only two callers, the menu's apply path and the console `SET
 * speed.default`. So turning the knob never marked settings dirty, nothing
 * auto-saved, and pressing OFF (which is wired to NRST) restored whatever
 * speed was last written by a menu edit. The original Teknatool firmware kept
 * the last-used speed across a power cycle; this restores that.
 *
 * WHY DEBOUNCED
 * -------------
 * One EEPROM write per encoder detent would be thousands per session against a
 * finite-endurance AT24C02, and the operator sweeps through dozens of values on
 * the way to the one they want. Waiting for the knob to be still collapses a
 * whole adjustment into a single 2-byte write.
 *
 * Only the two OEM bytes are written, never settings_save(): that now also
 * mirrors the full struct to flash, which erases a page and stalls the CPU for
 * ~20 ms. A 2-byte EEPROM write is safe with the spindle turning, which matters
 * because the operator may well press OFF mid-cut.
 */

#ifndef SPEED_AUTOSAVE_H
#define SPEED_AUTOSAVE_H

#include <stdint.h>
#include <stdbool.h>

/** How long the speed must hold still before it is committed. */
#define SPEED_AUTOSAVE_DEBOUNCE_MS  5000u

/**
 * State for one autosave channel. Zero-initialise.
 *
 * REVIEW FIX: this used to POLL g_state.target_rpm and treat any settled value
 * as the operator's choice. It is not theirs alone — update_sv_state()
 * (task_motor.c:255) writes that field from whatever SV value the MCB echoes
 * back, and step-drill mode (task_depth.c:481) writes a machine-computed RPM.
 * Neither is a speed anybody selected.
 *
 * Intent is now captured where intent exists: the encoder, the favourite
 * recall and the console SPEED command call speed_autosave_note(). Nothing
 * samples a shared field, so a writer that is not the operator cannot be
 * mistaken for one — and because the value no longer has to be inferred, a
 * speed dialled in mid-cut is remembered too, which the polling version had to
 * give up to stay safe.
 */
typedef struct {
    uint16_t pending;   /**< the value the operator last chose */
    uint32_t at_ms;     /**< when they chose it */
    bool     armed;     /**< a choice is waiting to settle */
} speed_autosave_t;

/**
 * @brief Record that the operator selected a speed.
 *
 * Restarts the debounce, so a sweep through forty detents arms forty times and
 * commits once.
 */
static inline void speed_autosave_note(speed_autosave_t* st,
                                       uint16_t rpm,
                                       uint32_t now_ms) {
    st->pending = rpm;
    st->at_ms   = now_ms;
    st->armed   = true;
}

/**
 * @brief Discard any pending choice.
 *
 * Called when something else sets the stored speed explicitly — the menu's
 * Speed>Target, or the console SET speed.default. Without this, a knob turn
 * before opening the menu stayed armed while the poll was gated off by
 * `menu_active`, and the first poll after the menu closed overwrote the value
 * the operator had just typed in and saved. Most recent explicit action wins.
 */
static inline void speed_autosave_forget(speed_autosave_t* st) {
    st->armed = false;
}

/** @brief Is a choice waiting? Lets a caller skip expensive work when not. */
static inline bool speed_autosave_armed(const speed_autosave_t* st) {
    return st->armed;
}

/**
 * @brief Has a recorded choice settled long enough to be written?
 *
 * @param persisted_rpm  What storage already holds.
 * @param out_rpm        Receives the value to write when this returns true.
 * @return true exactly once per settled change.
 *
 * A choice equal to what is already stored disarms without a write — turning
 * the knob away and back costs nothing.
 */
static inline bool speed_autosave_due(speed_autosave_t* st,
                                      uint16_t persisted_rpm,
                                      uint32_t now_ms,
                                      uint16_t* out_rpm) {
    if (!st->armed) {
        return false;
    }
    if (st->pending == persisted_rpm) {
        st->armed = false;   /* nothing to do */
        return false;
    }
    /* Unsigned difference: correct across the 32-bit tick wrap. */
    if ((now_ms - st->at_ms) < SPEED_AUTOSAVE_DEBOUNCE_MS) {
        return false;
    }
    if (out_rpm) {
        *out_rpm = st->pending;
    }
    st->armed = false;
    return true;
}

#endif /* SPEED_AUTOSAVE_H */
