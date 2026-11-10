/**
 * @file voltage_monitor.h
 * @brief DC bus low-voltage decision: is the bus LOW, or is the reading junk?
 *
 * Header-inline on purpose. `pio test -e native` compiles no `src/`, so logic
 * that lives in a .c file can only be tested by a copy in the test — and this
 * particular logic was. test_events carried a `sim_voltage_sample()` that
 * re-implemented an EDGE detector ("falling edge only", mock_voltage_was_low)
 * long after the firmware had replaced it with the debounce below. The suite
 * was green against behaviour the firmware did not have, and would have stayed
 * green whatever task_motor.c did. Keeping the decision here means the tests
 * exercise the shipping code.
 *
 * TWO FIELD BUGS SHAPED THIS, both on the same day (2026-08-30):
 *
 *   - An edge detector seeded at 0 treated boot as "already low", so a bus
 *     that was genuinely low from power-up never warned at all.
 *   - Seeded at the threshold instead, it warned on EVERY boot.
 *
 * Hence a debounce rather than an edge: what matters is that the bus STAYS
 * low. The charge transient is ignored, a bus that is low from power-up still
 * reports after DC_BUS_LOW_DEBOUNCE polls, and the warning re-arms once the
 * bus recovers so a later sag is reported again.
 *
 * A THIRD BUG, 2026-11-10, is why voltage_is_plausible() exists. The warning
 * appeared on every cold boot, and it was not a slow charge — measured live,
 * the bus is over 400 V before anything can read it and then decays to ~353 V.
 * The cause was the first UD reply after boot being 0 V, which the parse
 * accepted as a measurement because its range started at 0. Nought is not a
 * bus voltage on a machine whose MCB is answering: the MCB is powered FROM the
 * bus. A reply below DC_BUS_MIN_PLAUSIBLE has no reading behind it and must be
 * rejected as a bad frame, not counted as a flat bus.
 */

#ifndef VOLTAGE_MONITOR_H
#define VOLTAGE_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/**
 * @brief Could this UD reply be a real measurement?
 *
 * Rejects the physically impossible rather than the merely alarming: a genuine
 * sag worth warning about reads in the hundreds and passes this.
 */
static inline bool voltage_is_plausible(int32_t volts) {
    return volts >= DC_BUS_MIN_PLAUSIBLE && volts <= 500;
}

typedef struct {
    uint8_t low_count;   /**< consecutive low-but-plausible readings */
    bool    reported;    /**< warned for the current sag; cleared on recovery */
} voltage_monitor_t;

/**
 * @brief Feed one PLAUSIBLE reading. Implausible replies must not reach here —
 *        drop them as frame errors so they neither advance nor reset the count.
 *
 * @return true exactly once per sustained sag, on the poll that confirms it.
 */
static inline bool voltage_monitor_sample(voltage_monitor_t* st, uint16_t volts) {
    if (!st) return false;

    if (volts < DC_BUS_LOW_VOLTAGE_THRESHOLD) {
        if (st->low_count < DC_BUS_LOW_DEBOUNCE) {
            st->low_count++;
        }
        if (st->low_count >= DC_BUS_LOW_DEBOUNCE && !st->reported) {
            st->reported = true;
            return true;
        }
        return false;
    }

    /* Recovered: re-arm, so a later sag is reported again rather than being
     * swallowed by the first one having already fired. */
    st->low_count = 0;
    st->reported = false;
    return false;
}

#endif /* VOLTAGE_MONITOR_H */
