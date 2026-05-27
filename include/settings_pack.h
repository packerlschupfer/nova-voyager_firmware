/**
 * @file settings_pack.h
 * @brief Serialise settings_t to and from the 0xB0 EEPROM block.
 *
 * Header-only and hardware-free so the real serialiser can be round-tripped in
 * test/test_settings_pack instead of only being exercised by power-cycling the
 * machine. Same reasoning as include/safety.h and include/menu_format.h.
 *
 * WHY IT EXISTS
 * -------------
 * The pack/unpack pair used to be two long, independent lists of assignments
 * buried in settings.c. Nothing checked that they agreed with each other, or
 * that a field the menu can edit appeared in them at all — and 39 fields did
 * not. settings_save() cleared `dirty` and returned true regardless, the
 * console printed "Settings saved" and the menu showed "Settings Saved!", and
 * the next boot silently restored defaults. Among the casualties were the live
 * jam and belt-break thresholds, so machine-specific tuning was lost on every
 * power cycle with no indication at all.
 *
 * WHAT IS AND IS NOT PERSISTED
 * ----------------------------
 * The block is 61 bytes (see eeprom_layout.h) and settings_t is 192, so this
 * is a subset by necessity, not an oversight — and eeprom_custom_t is now
 * exactly 61 bytes, i.e. the block is FULL. Nothing more can be persisted on
 * an EEPROM unit without taking space from the crash-dump record at 0xED.
 *
 * What it holds is everything that changes how the machine behaves while
 * cutting: motor tuning, all four jam and belt-break detectors, depth
 * mode/target/offset/action, which tapping triggers are armed plus the tapping
 * speed and peck timings, step drill, power output and the temperature trip.
 *
 * Deliberately NOT persisted on EEPROM units, and defaulted at every boot:
 * interface.* (key sound, F-key assignments, menu lock/password), display.*
 * beyond units, speed.material/bit_type/bit_diameter, and power.* apart from
 * power_output and temp_threshold.
 *
 * MEASURED CORRECTION (2026-09-05): the claim below that these fields "do NOT
 * survive a power cycle" is WRONG, and was wrong when written. settings_init()
 * layers its sources — defaults, then the FULL settings_t mirrored to internal
 * flash by the last idle SAVE, then the OEM EEPROM fields, then this block —
 * and the flash mirror carries every field, including these.
 *
 * Proven on target rather than argued: two of them were set, saved, the ENTIRE
 * EEPROM was blanked to 0xFF and verified blank, and after a reset both came
 * back. Nothing but the mirror could have supplied them.
 *
 * What IS true, and is the reason they were moved into this block for v2: the
 * mirror is written only by an IDLE save. settings_save() returns
 * SETTINGS_SAVE_DEFERRED while the motor runs and writes the EEPROM block
 * alone. So before v2 a mid-cut SAVE persisted none of these, and losing power
 * before the next idle SAVE lost them. Now they are EEPROM-native and a
 * mid-cut save keeps them. That is a narrower benefit than "they were lost
 * every boot", which is what this comment previously implied and what the v2
 * work was justified with.
 *
 * The twelve fields this block did not hold before v2:
 *
 *   quill_pedal_mode, quill_completion_action, load_increase_threshold,
 *   load_increase_reverse_ms, load_completion_action, load_slip_cv_percent,
 *   load_slip_completion_action, clutch_plateau_ms, clutch_action,
 *   peck_depth_stop, pedal_action, pedal_chip_break_ms
 *
 * (It was thirteen until depth_completion_action replaced the legacy
 * two-value depth_action and inherited its persisted byte — the encodings
 * coincide, so no migration was needed. This list is the kind that rots
 * silently the moment a field moves; it rotted on exactly that change and was
 * corrected on 2026-09-05. If you move a field in or out of the blob, fix this
 * list in the same commit.)
 *
 * (They did NOT revert to defaults at the next boot — see the measured
 * correction above. An earlier version of this comment said they did.)
 *
 * This now bites harder than when it was written: three of the twelve
 * (quill/load/load_slip completion_action) went from stored-but-unread to
 * deciding how a tapping cycle ENDS, so their defaults are what the machine
 * actually behaves like on every power-up.
 *
 * Units WITHOUT an EEPROM store the whole struct in flash and keep all of it,
 * these thirteen included — see settings_save().
 *
 * If you add a field here, add it to test/test_settings_pack too. The test
 * round-trips every field individually; that is what stops this list rotting
 * again.
 */

#ifndef SETTINGS_PACK_H
#define SETTINGS_PACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "settings.h"
#include "jam.h"
#include "eeprom_layout.h"

/** Sum of every byte before the checksum field. */
static inline uint16_t settings_custom_checksum(const eeprom_custom_t* c) {
    uint16_t ck = 0;
    const uint8_t* p = (const uint8_t*)c;
    for (size_t i = 0; i < offsetof(eeprom_custom_t, checksum); i++) {
        ck = (uint16_t)(ck + p[i]);
    }
    return ck;
}

/** Serialise the persistable subset of @p s into @p c, checksum included. */
static inline void settings_pack_custom(const settings_t* s, eeprom_custom_t* c) {
    c->magic   = EE_CUSTOM_MAGIC_VALUE;
    c->version = EE_CUSTOM_VERSION_NUM;

    c->speed_kprop  = s->motor.speed_kprop;
    c->speed_kint   = s->motor.speed_kint;
    c->voltage_kp   = s->motor.voltage_kp;
    c->voltage_ki   = s->motor.voltage_ki;
    c->ir_gain      = s->motor.ir_gain;
    c->ir_offset    = s->motor.ir_offset;
    c->speed_ramp   = s->motor.speed_ramp;
    c->torque_ramp  = s->motor.torque_ramp;
    /* current_limit is a percentage; the wider type in settings_t is historic. */
    c->current_limit = (uint8_t)(s->motor.current_limit > 255 ? 255
                                                             : s->motor.current_limit);

    c->overload_threshold    = s->sensor.overload_threshold;
    c->spike_thresh          = (uint8_t)(s->sensor.spike_thresh > 255 ? 255
                                                                     : s->sensor.spike_thresh);
    c->vibration_sensitivity = s->sensor.vibration_sensitivity;
    c->step_thresh           = s->sensor.step_thresh;
    c->low_load_thresh       = s->sensor.low_load_thresh;
    c->stall_sensitivity     = s->sensor.stall_sensitivity;
    c->stall_time_ms20       = (uint8_t)(s->sensor.stall_time_ms / 20);

    c->temp_threshold = s->power.temp_threshold;

    c->depth_target = s->depth.target;
    c->depth_offset = s->depth.offset;

    c->tap_triggers =
        (uint8_t)((s->tapping.depth_trigger_enabled ? 0x01 : 0) |
                  (s->tapping.load_increase_enabled ? 0x02 : 0) |
                  (s->tapping.load_slip_enabled     ? 0x04 : 0) |
                  (s->tapping.clutch_slip_enabled   ? 0x08 : 0) |
                  (s->tapping.quill_trigger_enabled ? 0x10 : 0) |
                  (s->tapping.peck_trigger_enabled  ? 0x20 : 0) |
                  (s->tapping.pedal_enabled         ? 0x40 : 0));
    c->tap_speed_rpm           = s->tapping.speed_rpm;
    /* Scaled to a byte. Lossless for every menu-reachable value: the rows step
     * by 50 (FwdMs/RevMs), 100 (RevTime) and 10 (ChipMs). */
    c->peck_fwd_ms50           = (uint8_t)(s->tapping.peck_fwd_ms / 50);
    c->peck_rev_ms10           = (uint8_t)(s->tapping.peck_rev_ms / 10);
    c->peck_cycles             = s->tapping.peck_cycles;
    c->brake_delay_ms10        = (uint8_t)(s->tapping.brake_delay_ms / 10);
    c->tap_depth_completion_action = s->tapping.depth_completion_action;
    c->peck_completion_action  = s->tapping.peck_completion_action;
    c->peck_rev_out_ms50       = (uint8_t)(s->tapping.peck_reverse_out_ms / 50);

    /* The twelve that used to be defaulted at every boot. */
    c->load_inc_threshold   = s->tapping.load_increase_threshold;
    c->load_slip_cv         = (uint8_t)(s->tapping.load_slip_cv_percent > 255 ? 255
                                        : s->tapping.load_slip_cv_percent);
    c->load_inc_rev_ms10    = (uint8_t)(s->tapping.load_increase_reverse_ms / 10);
    c->clutch_plateau_ms10  = (uint8_t)(s->tapping.clutch_plateau_ms / 10);
    c->pedal_chip_ms10      = (uint8_t)(s->tapping.pedal_chip_break_ms / 10);

    c->tap_actions = (uint8_t)(
        ((s->tapping.quill_pedal_mode           & EE_TA_MASK2) << EE_TA_QUILL_PEDAL_SHIFT) |
        ((s->tapping.quill_completion_action    & EE_TA_MASK2) << EE_TA_QUILL_COMP_SHIFT)  |
        ((s->tapping.load_completion_action     & EE_TA_MASK2) << EE_TA_LOAD_COMP_SHIFT)   |
        ((s->tapping.load_slip_completion_action & EE_TA_MASK2) << EE_TA_SLIP_COMP_SHIFT));

    c->tap_misc = (uint8_t)(
        (s->tapping.clutch_action  ? EE_TM_CLUTCH_ACTION   : 0) |
        (s->tapping.peck_depth_stop ? EE_TM_PECK_DEPTH_STOP : 0) |
        (s->tapping.pedal_action   ? EE_TM_PEDAL_ACTION    : 0) |
        (((uint8_t)s->depth.mode   & EE_TA_MASK2) << EE_TM_DEPTH_MODE_SHIFT)   |
        (s->depth.action           ? EE_TM_DEPTH_ACTION    : 0) |
        (((uint8_t)s->power.power_output & EE_TA_MASK2) << EE_TM_POWER_OUTPUT_SHIFT));

    c->step_start_dia  = s->step_drill.start_diameter;
    c->step_dia_inc    = s->step_drill.diameter_increment;
    c->step_depth_x2   = s->step_drill.step_depth_x2;
    c->step_base_rpm   = s->step_drill.base_rpm;
    c->step_target_dia = s->step_drill.target_diameter;

    c->slow_start = s->speed.slow_start;

    c->flags = (uint8_t)((s->sensor.jam_detect           ? EE_CF_JAM_DETECT      : 0) |
                         (s->sensor.spike_detect         ? EE_CF_SPIKE_DETECT    : 0) |
                         (s->sensor.guard_check_enabled  ? EE_CF_GUARD_CHECK     : 0) |
                         (s->sensor.pedal_enabled        ? EE_CF_PEDAL_ENABLED   : 0) |
                         (s->step_drill.enabled          ? EE_CF_STEP_ENABLED    : 0) |
                         (s->sensor.low_load_detect      ? EE_CF_LOW_LOAD_DETECT : 0) |
                         (s->depth.enabled               ? EE_CF_DEPTH_ENABLED   : 0));
    c->units = (uint8_t)s->display.units;

    c->checksum = settings_custom_checksum(c);
}

/**
 * @brief Validate @p c and apply it to @p s.
 * @return false if the blob is absent, of a different layout version, or
 *         corrupt — in which case @p s is left untouched.
 */
/* Clamp every field the loader restores into the range its setter enforces.
 *
 * REVIEW FIX (HIGH): settings_unpack_custom() applied ~35 fields straight into
 * live settings behind nothing but an additive byte-sum checksum — which is
 * order-insensitive and blind to compensating errors — while the OEM loader
 * beside it range-checks everything it reads. So a blob that is corrupt but
 * still sums correctly put out-of-range values where they matter:
 * sensor.spike_thresh restored as 0 fires an emergency stop seconds into every
 * cut (settings.c documents exactly this), motor.voltage_kp/ki go to the MCB
 * via motor_sync_settings(), which validates the ramps but not the gains, and
 * depth.target lands directly in g_state.target_depth.
 *
 * Clamping rather than rejecting the whole blob is deliberate: one bad field
 * should not throw away a machine's other tuning. The bounds are the ones the
 * settings_set_* functions apply, so a loaded value can never be something the
 * operator could not have dialled in.
 */
#define SETTINGS_CLAMP(field, lo, hi) \
    do { \
        if ((field) < (lo)) (field) = (lo); \
        if ((field) > (hi)) (field) = (hi); \
    } while (0)

static inline void settings_clamp_loaded(settings_t* s) {
    /* REVIEW FIX (MEDIUM): the block comment above names motor.voltage_kp/ki
     * as a reason this function exists, and they were not actually clamped —
     * only the ramps and the current limit were. The checksum is an additive
     * byte sum, so it is order-insensitive: a two-byte transposition on the
     * bit-banged I2C read turns voltage_kp = 2000 (D0 07) into -12281, passes
     * magic/version/checksum, and motor_sync_settings() pushes it to the MCB at
     * every boot. Bounds are the MCB parameter ranges these are written into. */
    SETTINGS_CLAMP(s->motor.speed_kprop, 0, 32000);
    SETTINGS_CLAMP(s->motor.speed_kint,  0, 32000);
    SETTINGS_CLAMP(s->motor.voltage_kp,  0, 32000);
    SETTINGS_CLAMP(s->motor.voltage_ki,  0, 32000);
    SETTINGS_CLAMP(s->motor.ir_gain,     0, 32000);
    SETTINGS_CLAMP(s->motor.ir_offset,   0, 32000);

    SETTINGS_CLAMP(s->motor.speed_ramp,   50, 2000);
    SETTINGS_CLAMP(s->motor.torque_ramp,  50, 2000);
    SETTINGS_CLAMP(s->motor.current_limit, 0, 500);

    SETTINGS_CLAMP(s->sensor.overload_threshold,   10, 100);
    SETTINGS_CLAMP(s->sensor.spike_thresh,         20, 100);
    SETTINGS_CLAMP(s->sensor.vibration_sensitivity, 0, 3);
    /* 0 still means "step detector off"; anything above 0 must clear the
     * floor — see JAM_STEP_MIN_THRESH in jam.h. */
    if (s->sensor.step_thresh > 0 && s->sensor.step_thresh < JAM_STEP_MIN_THRESH) {
        s->sensor.step_thresh = JAM_STEP_MIN_THRESH;
    }
    SETTINGS_CLAMP(s->sensor.step_thresh,           0, 100);
    SETTINGS_CLAMP(s->sensor.low_load_thresh,       0, 100);
    SETTINGS_CLAMP(s->sensor.stall_sensitivity,     0, 100);
    SETTINGS_CLAMP(s->sensor.stall_time_ms,       100, 5000);

    SETTINGS_CLAMP(s->power.power_output,    0, 3);
    SETTINGS_CLAMP(s->power.temp_threshold, 40, 100);

    if ((int)s->depth.mode   > (int)DEPTH_MODE_PRECISION)  s->depth.mode   = DEPTH_MODE_OFF;
    if ((int)s->depth.action > (int)DEPTH_ACTION_STOP_REV_TOP) s->depth.action = DEPTH_ACTION_STOP;
    /* 0..1500 in 0.1 mm — the range the Depth > Target menu row offers
     * (menu.c). The setter itself is unclamped, so this is the only bound. */
    SETTINGS_CLAMP(s->depth.target, 0, 1500);
    /* depth.offset is a raw 12-bit ADC reading (task_depth.c). */
    SETTINGS_CLAMP(s->depth.offset, 0, 4095);

    SETTINGS_CLAMP(s->tapping.speed_rpm,         SPEED_MIN_RPM, SPEED_MAX_RPM);
    /* TAP_PECK_FWD_MS_MIN/MAX and TAP_PECK_REV_MS_MIN/MAX from tapping.h,
     * inlined as literals so this header stays free of tapping.h (it is
     * compiled by the native tests, which build no src/). */
    SETTINGS_CLAMP(s->tapping.peck_fwd_ms, 50, 5000);
    SETTINGS_CLAMP(s->tapping.peck_rev_ms, 50, 2000);
    SETTINGS_CLAMP(s->tapping.peck_cycles,       0, 99);
    SETTINGS_CLAMP(s->tapping.brake_delay_ms,    50, 500);
    /* 0..2, not 0..3: COMPLETION_RESUME would re-trigger on the next poll
     * because depth >= target is still true after a back-off. */
    SETTINGS_CLAMP(s->tapping.depth_completion_action, 0, 2);
    SETTINGS_CLAMP(s->tapping.quill_completion_action, 0, 3);
    SETTINGS_CLAMP(s->tapping.load_completion_action,  0, 3);
    SETTINGS_CLAMP(s->tapping.load_slip_completion_action, 0, 3);
    SETTINGS_CLAMP(s->tapping.peck_completion_action, 0, 2);
    SETTINGS_CLAMP(s->tapping.peck_reverse_out_ms, 100, 10000);
    /* These four became load-bearing when trigger reverses gained a duration:
     * they now decide how long the spindle actually runs backwards and whether
     * a clutch plateau reverses at all. An out-of-range value used to be inert
     * because nothing read them. Bounds match the setters and the menu rows. */
    SETTINGS_CLAMP(s->tapping.load_increase_reverse_ms, 50, 2000);
    SETTINGS_CLAMP(s->tapping.clutch_action,     0, 1);
    SETTINGS_CLAMP(s->tapping.pedal_action,      0, 1);
    SETTINGS_CLAMP(s->tapping.pedal_chip_break_ms, 50, 500);

    SETTINGS_CLAMP(s->step_drill.start_diameter,     5, 50);
    SETTINGS_CLAMP(s->step_drill.diameter_increment, 1, 10);
    SETTINGS_CLAMP(s->step_drill.step_depth_x2,     10, 40);
    SETTINGS_CLAMP(s->step_drill.base_rpm,  SPEED_MIN_RPM, SPEED_MAX_RPM);
    SETTINGS_CLAMP(s->step_drill.target_diameter,    0, 50);

    SETTINGS_CLAMP(s->speed.slow_start, 100, 1000);

    if ((int)s->display.units > (int)UNITS_IMPERIAL_FRACTION) s->display.units = UNITS_METRIC;
}

static inline bool settings_unpack_custom(const eeprom_custom_t* c, settings_t* s) {
    if (c->magic != EE_CUSTOM_MAGIC_VALUE)      return false;
    if (c->version != EE_CUSTOM_VERSION_NUM)    return false;
    if (settings_custom_checksum(c) != c->checksum) return false;

    s->motor.speed_kprop  = c->speed_kprop;
    s->motor.speed_kint   = c->speed_kint;
    s->motor.voltage_kp   = c->voltage_kp;
    s->motor.voltage_ki   = c->voltage_ki;
    s->motor.ir_gain      = c->ir_gain;
    s->motor.ir_offset    = c->ir_offset;
    s->motor.speed_ramp   = c->speed_ramp;
    s->motor.torque_ramp  = c->torque_ramp;
    s->motor.current_limit = c->current_limit;

    s->sensor.overload_threshold    = c->overload_threshold;
    s->sensor.spike_thresh          = c->spike_thresh;
    s->sensor.vibration_sensitivity = c->vibration_sensitivity;
    s->sensor.step_thresh           = c->step_thresh;
    s->sensor.low_load_thresh       = c->low_load_thresh;
    s->sensor.stall_sensitivity     = c->stall_sensitivity;
    s->sensor.stall_time_ms         = (uint16_t)c->stall_time_ms20 * 20;

    s->power.power_output   = (uint8_t)((c->tap_misc >> EE_TM_POWER_OUTPUT_SHIFT) & EE_TA_MASK2);
    s->power.temp_threshold = c->temp_threshold;

    s->depth.mode   = (depth_mode_t)((c->tap_misc >> EE_TM_DEPTH_MODE_SHIFT) & EE_TA_MASK2);
    s->depth.action = (depth_action_t)((c->tap_misc & EE_TM_DEPTH_ACTION) ? 1 : 0);
    s->depth.target = c->depth_target;
    s->depth.offset = c->depth_offset;

    s->tapping.depth_trigger_enabled = (c->tap_triggers & 0x01) != 0;
    s->tapping.load_increase_enabled = (c->tap_triggers & 0x02) != 0;
    s->tapping.load_slip_enabled     = (c->tap_triggers & 0x04) != 0;
    s->tapping.clutch_slip_enabled   = (c->tap_triggers & 0x08) != 0;
    s->tapping.quill_trigger_enabled = (c->tap_triggers & 0x10) != 0;
    s->tapping.peck_trigger_enabled  = (c->tap_triggers & 0x20) != 0;
    s->tapping.pedal_enabled         = (c->tap_triggers & 0x40) != 0;
    s->tapping.speed_rpm              = c->tap_speed_rpm;
    s->tapping.peck_fwd_ms            = (uint16_t)c->peck_fwd_ms50 * 50;
    s->tapping.peck_rev_ms            = (uint16_t)c->peck_rev_ms10 * 10;
    s->tapping.peck_cycles            = c->peck_cycles;
    s->tapping.brake_delay_ms         = (uint16_t)c->brake_delay_ms10 * 10;
    s->tapping.depth_completion_action = c->tap_depth_completion_action;
    s->tapping.peck_completion_action = c->peck_completion_action;
    s->tapping.peck_reverse_out_ms    = (uint16_t)c->peck_rev_out_ms50 * 50;

    /* The twelve. Every one of these used to be defaulted at every boot. */
    s->tapping.load_increase_threshold   = c->load_inc_threshold;
    s->tapping.load_slip_cv_percent      = c->load_slip_cv;
    s->tapping.load_increase_reverse_ms  = (uint16_t)c->load_inc_rev_ms10 * 10;
    s->tapping.clutch_plateau_ms         = (uint16_t)c->clutch_plateau_ms10 * 10;
    s->tapping.pedal_chip_break_ms       = (uint16_t)c->pedal_chip_ms10 * 10;

    s->tapping.quill_pedal_mode            = (c->tap_actions >> EE_TA_QUILL_PEDAL_SHIFT) & EE_TA_MASK2;
    s->tapping.quill_completion_action     = (c->tap_actions >> EE_TA_QUILL_COMP_SHIFT)  & EE_TA_MASK2;
    s->tapping.load_completion_action      = (c->tap_actions >> EE_TA_LOAD_COMP_SHIFT)   & EE_TA_MASK2;
    s->tapping.load_slip_completion_action = (c->tap_actions >> EE_TA_SLIP_COMP_SHIFT)   & EE_TA_MASK2;

    s->tapping.clutch_action  = (c->tap_misc & EE_TM_CLUTCH_ACTION)   ? 1 : 0;
    s->tapping.peck_depth_stop = (c->tap_misc & EE_TM_PECK_DEPTH_STOP) != 0;
    s->tapping.pedal_action   = (c->tap_misc & EE_TM_PEDAL_ACTION)    ? 1 : 0;

    s->step_drill.start_diameter     = c->step_start_dia;
    s->step_drill.diameter_increment = c->step_dia_inc;
    s->step_drill.step_depth_x2      = c->step_depth_x2;
    s->step_drill.base_rpm           = c->step_base_rpm;
    s->step_drill.target_diameter    = c->step_target_dia;

    s->speed.slow_start = c->slow_start;

    s->sensor.jam_detect          = (c->flags & EE_CF_JAM_DETECT)      != 0;
    s->sensor.spike_detect        = (c->flags & EE_CF_SPIKE_DETECT)    != 0;
    s->sensor.guard_check_enabled = (c->flags & EE_CF_GUARD_CHECK)     != 0;
    s->sensor.pedal_enabled       = (c->flags & EE_CF_PEDAL_ENABLED)   != 0;
    s->step_drill.enabled         = (c->flags & EE_CF_STEP_ENABLED)    != 0;
    s->sensor.low_load_detect     = (c->flags & EE_CF_LOW_LOAD_DETECT) != 0;
    s->depth.enabled              = (c->flags & EE_CF_DEPTH_ENABLED)   != 0;
    s->display.units              = (units_mode_t)c->units;

    /* Nothing below this line may assume the blob was sane — see above. */
    settings_clamp_loaded(s);
    return true;
}

#endif /* SETTINGS_PACK_H */
