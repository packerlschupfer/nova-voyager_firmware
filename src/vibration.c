/**
 * @file vibration.c
 * @brief 3-axis accelerometer read + threshold logic, ported from the OEM.
 *
 * Every constant here comes from the original firmware, not from taste. The
 * decode and thresholds are in FUN_080075dc @ 0x080075dc (image base
 * 0x08003000 — notes quoting 0x080075dc-0x3000 are from a mis-based import).
 *
 * THE SENSOR DOES NOT ANSWER ON THIS MACHINE, and that is not for want of
 * looking. The operator confirms the part IS fitted here and that he has
 * personally seen the OEM firmware raise its vibration warning, so this is a
 * driver for real hardware, not speculation. What has been ruled out, each on
 * the machine rather than on paper (2026-09-05):
 *
 *   - Wrong pins. The AT24C02 answers at 0x50 on these same two wires.
 *   - Wrong address. 0x1D is the OEM's own 0x3A/0x3B, and an address-only
 *     scan of 0x08..0x77 finds nothing but the EEPROM.
 *   - A bad probe. The scan was rewritten from a register read to a bare
 *     address ACK so it cannot report "absent" for a device that merely
 *     dislikes a register.
 *   - Electrical config. Open-drain with pull-ups at ~100 kHz, which is what
 *     the EEPROM is working over.
 *   - PB12 as a power/enable line. It is the one output the OEM configures
 *     and we never touch. Driven HIGH and driven LOW: no change.
 *   - Power only while the spindle turns. Tested with the motor confirmed
 *     running (actual 399 rpm against a 400 target, verified either side of
 *     the scan): still only 0x50.
 *   - PA8 as an enable. The OEM configures PA8 as output push-pull inside its
 *     I2C init (FUN_080184f0, which also enables the GPIOA/GPIOC clocks) while
 *     we use PA8 as the buzzer and leave it driven LOW after every beep
 *     (buzzer.c pwm_stop) — a genuinely suspicious collision. Driven HIGH:
 *     no change. So PA8 really is just the buzzer and FUN_080184f0 groups
 *     unrelated pin setup rather than gating the bus.
 *
 * The RTC the OEM also addresses (DS1307 @ 0x68) is equally silent, which
 * suggests whatever is missing is shared rather than specific to this part.
 * Also eliminated, after the operator reaffirmed the sensor is fitted and
 * working:
 *   - Bus speed. Swept from the normal ~200 kHz down to ~3 kHz (factor 64).
 *     A part that NAKs fast and answers slow would look identical to an absent
 *     one; it does not.
 *   - Slow bus AND spindle running, together.
 *   - Different pins entirely. Bit-banged address scans on PB6/PB7 and
 *     PB8/PB9 (the two STM32 I2C1 pairs, both unused by us and by the OEM)
 *     found nothing either.
 *
 * A theory that the operator's remembered warning was a FALSE POSITIVE from
 * this floating bus has also been tested and does not hold. The OEM never
 * checks the I2C ACK, so with no sensor its arithmetic is fed whatever the bus
 * produces; reading it the same way (see i2c_read_device_reg_noack and the
 * VIBRAW command) gives a stable X=-5 Y=245 Z=-5, peak 245 against the
 * MAX-sensitivity trip of 251. Six counts short — tantalising, and it fits the
 * operator's report that it only ever fired on MAX. But the mechanism required
 * motor noise to shift those reads, and it does not: sampled at 400, 1500 and
 * 3001 rpm (actual rpm verified at each), all six registers read 0xFF every
 * time and the peak never moved off 245. So the near-miss is a coincidence of
 * the +250 Y offset, not an explanation.
 *
 * Ten separate variables have now been tested and killed on the machine. A device that does not ACK its own address is not a software
 * problem: an I2C slave with power and a connection answers regardless of how
 * it is configured. The next useful step is a multimeter on the
 * accelerometer's supply pin, not more firmware.
 */
#include "vibration.h"
#include "eeprom.h"

/* Axis registers. Each axis is two registers combined into a 10-bit value;
 * the OEM reads X from 1,2 — Y from 3,4 — Z from 5,6. */
#define REG_X_HI  1
#define REG_X_LO  2
#define REG_Y_HI  3
#define REG_Y_LO  4
#define REG_Z_HI  5
#define REG_Z_LO  6

/* The OEM adds 250 to the Y axis before comparing. Reproduced rather than
 * removed: the thresholds below were chosen against a Y that carries it, so
 * dropping it would silently shift one axis' trip point. Presumably it
 * compensates for the sensor's mounting orientation (gravity on one axis). */
#define Y_AXIS_OFFSET  250

/**
 * @brief Combine two registers into one signed axis value.
 *
 * OEM: `hi * 4 + ((int)lo >> 6)` with both bytes treated as SIGNED chars, so
 * the high register supplies bits 2..9 and the top two bits of the low
 * register supply bits 0..1. The decompiler also emits a multiply-by-64 /
 * divide-by-64 dance after this when hi < 0; that is the compiler's way of
 * sign-extending the 10-bit result and is a no-op for in-range values, so it
 * is not reproduced.
 */
static int16_t combine_axis(uint8_t hi_raw, uint8_t lo_raw) {
    const int8_t hi = (int8_t)hi_raw;
    const int8_t lo = (int8_t)lo_raw;
    return (int16_t)((int)hi * 4 + ((int)lo >> 6));
}

bool vibration_read_axes(vibration_axes_t* axes) {
    if (!axes) return false;
    axes->valid = false;

    uint8_t xh, xl, yh, yl, zh, zl;
    if (!i2c_read_device_reg(VIBRATION_I2C_ADDR, REG_X_HI, &xh)) return false;
    if (!i2c_read_device_reg(VIBRATION_I2C_ADDR, REG_X_LO, &xl)) return false;
    if (!i2c_read_device_reg(VIBRATION_I2C_ADDR, REG_Y_HI, &yh)) return false;
    if (!i2c_read_device_reg(VIBRATION_I2C_ADDR, REG_Y_LO, &yl)) return false;
    if (!i2c_read_device_reg(VIBRATION_I2C_ADDR, REG_Z_HI, &zh)) return false;
    if (!i2c_read_device_reg(VIBRATION_I2C_ADDR, REG_Z_LO, &zl)) return false;

    axes->x = combine_axis(xh, xl);
    axes->y = (int16_t)(combine_axis(yh, yl) + Y_AXIS_OFFSET);
    axes->z = combine_axis(zh, zl);
    axes->valid = true;
    return true;
}

/* OEM thresholds, indexed by sensitivity 1..3. `excess` is the outer band and
 * returns VIBRATION_EXCESS; anything past `elevated` but inside `excess`
 * returns VIBRATION_ELEVATED. Both are applied as +/- to every axis. */
static const struct { int16_t excess; int16_t elevated; } k_thresholds[] = {
    { 0,   0   },   /* [0] unused — sensitivity 0 is "disabled" */
    { 650, 601 },   /* [1] LOW    (OEM 0x28a / 0x259) */
    { 501, 451 },   /* [2] MEDIUM (OEM 0x1f5 / 0x1c3) */
    { 301, 251 },   /* [3] HIGH   (OEM 0x12d / 0xfb)  */
};

static bool any_axis_beyond(const vibration_axes_t* a, int16_t limit) {
    return (a->x >  limit) || (a->y >  limit) || (a->z >  limit) ||
           (a->x < -limit) || (a->y < -limit) || (a->z < -limit);
}

vibration_level_t vibration_evaluate(uint8_t sensitivity) {
    /* 0 is DISABLED, and it is the factory default. Match the OEM: no read,
     * no result — not "read it and ignore the answer". */
    if (sensitivity == 0 || sensitivity >= (sizeof(k_thresholds)/sizeof(k_thresholds[0]))) {
        return VIBRATION_OK;
    }

    vibration_axes_t axes;
    if (!vibration_read_axes(&axes)) {
        /* Device silent. Report OK rather than inventing a fault: a missing
         * accelerometer must not stop the spindle. The caller can use
         * vibration_present() if it wants to know. */
        return VIBRATION_OK;
    }

    if (any_axis_beyond(&axes, k_thresholds[sensitivity].excess))   return VIBRATION_EXCESS;
    if (any_axis_beyond(&axes, k_thresholds[sensitivity].elevated)) return VIBRATION_ELEVATED;
    return VIBRATION_OK;
}

bool vibration_present(void) {
    /* WHO_AM_I, not a bare address ACK. Reading a data register only proves
     * SOMETHING lives at 0x1D; this proves it is the expected part. The OEM
     * makes the same check (FUN_0800754e: read 0x0D, require 0x5A) and
     * disables the vibration feature when it fails — which is why a unit
     * without the sensor fitted behaves sanely rather than reporting garbage
     * axes as motion. */
    uint8_t id = 0;
    if (!i2c_read_device_reg(VIBRATION_I2C_ADDR, VIBRATION_REG_WHOAMI, &id)) {
        return false;
    }
    return id == VIBRATION_WHOAMI_VALUE;
}
