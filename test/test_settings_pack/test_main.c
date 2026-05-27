/**
 * @file test_main.c
 * @brief Round-trip tests for the REAL EEPROM settings serialiser.
 *
 * Includes the shipping include/settings_pack.h, not a copy.
 *
 * The bug this suite exists for: v0.1.0's serialiser silently omitted 39
 * settings. `SET sensor.low_load_thresh 15` + `SAVE` printed "Settings saved",
 * the menu showed "Settings Saved!", and the next boot fed the jam detector
 * the default 5% again. No test could see it, because the only thing that
 * could have noticed is a test that names each field and checks it survives —
 * which is what this is.
 *
 * If you add a field to settings_pack.h, add it to `mutate_all()` and the
 * per-field list below, or this suite will keep passing while the field is
 * dropped.
 */

#include <unity.h>
#include <string.h>
#include "settings_pack.h"

static settings_t src;
static settings_t dst;
static eeprom_custom_t blob;

/* A value distinguishable from both 0 and any plausible default. */
static void mutate_all(settings_t* s) {
    memset(s, 0, sizeof(*s));

    /* Positive: the menu offers 0..9999 for all six and the MCB's own values
     * are positive (ir_gain reads 28835 on the machine, which is why the
     * loader's ceiling is 32000 and not the menu's 9999). A negative PID gain
     * would invert the loop and is not reachable from any UI — the loader
     * clamps it away, so the round-trip fixture must not use one. */
    s->motor.speed_kprop  =  1234;
    s->motor.speed_kint   =  4321;
    s->motor.voltage_kp   =   777;
    s->motor.voltage_ki   =   888;
    s->motor.ir_gain      = 28835;
    s->motor.ir_offset    =  1357;
    s->motor.speed_ramp   =  1900;
    s->motor.torque_ramp  =   650;
    s->motor.current_limit =   93;

    s->sensor.overload_threshold    = 77;
    s->sensor.spike_thresh          = 90;    /* in range: the loader clamps 20..100 */
    s->sensor.vibration_sensitivity = 3;
    s->sensor.step_thresh           = 24;
    s->sensor.low_load_thresh       = 15;
    s->sensor.stall_sensitivity     = 61;
    s->sensor.stall_time_ms         = 4500;
    s->sensor.jam_detect            = true;
    s->sensor.spike_detect          = true;
    s->sensor.guard_check_enabled   = true;
    s->sensor.pedal_enabled         = true;
    s->sensor.low_load_detect       = true;

    s->power.power_output   = 2;
    s->power.temp_threshold = 66;

    s->depth.mode    = 2;
    s->depth.action  = 1;
    s->depth.target  = 1234;
    s->depth.offset  = 1321;   /* a raw 12-bit ADC reading; the loader clamps 0..4095 */
    s->depth.enabled = true;

    s->tapping.depth_trigger_enabled = true;
    s->tapping.load_increase_enabled = false;
    s->tapping.load_slip_enabled     = true;
    s->tapping.clutch_slip_enabled   = false;
    s->tapping.quill_trigger_enabled = true;
    s->tapping.peck_trigger_enabled  = false;
    s->tapping.pedal_enabled         = true;
    s->tapping.speed_rpm              = 420;
    s->tapping.peck_fwd_ms            = 1750;
    s->tapping.peck_rev_ms            = 950;
    s->tapping.peck_cycles            = 17;
    s->tapping.brake_delay_ms         = 250;  /* 10 ms grid — see the quantise test */
    s->tapping.depth_completion_action = 1;
    s->tapping.peck_completion_action = 2;
    s->tapping.peck_reverse_out_ms    = 3300;

    s->step_drill.enabled            = true;
    s->step_drill.start_diameter     = 13;
    s->step_drill.diameter_increment = 4;
    s->step_drill.step_depth_x2      = 27;
    s->step_drill.base_rpm           = 2750;
    s->step_drill.target_diameter    = 38;

    s->speed.slow_start = 850;
    s->display.units    = UNITS_IMPERIAL_FRACTION;
}

void setUp(void) {
    mutate_all(&src);
    memset(&dst, 0, sizeof(dst));
    memset(&blob, 0, sizeof(blob));
}

void tearDown(void) {}

static void roundtrip(void) {
    settings_pack_custom(&src, &blob);
    TEST_ASSERT_TRUE_MESSAGE(settings_unpack_custom(&blob, &dst),
                             "a blob we just packed must validate");
}

/*--- the whole point: every persisted field survives -----------------------*/

static void test_motor_fields_survive(void) {
    roundtrip();
    TEST_ASSERT_EQUAL_INT16(src.motor.speed_kprop,   dst.motor.speed_kprop);
    TEST_ASSERT_EQUAL_INT16(src.motor.speed_kint,    dst.motor.speed_kint);
    TEST_ASSERT_EQUAL_INT16(src.motor.voltage_kp,    dst.motor.voltage_kp);
    TEST_ASSERT_EQUAL_INT16(src.motor.voltage_ki,    dst.motor.voltage_ki);
    TEST_ASSERT_EQUAL_INT16(src.motor.ir_gain,       dst.motor.ir_gain);
    TEST_ASSERT_EQUAL_INT16(src.motor.ir_offset,     dst.motor.ir_offset);
    TEST_ASSERT_EQUAL_UINT16(src.motor.speed_ramp,   dst.motor.speed_ramp);
    TEST_ASSERT_EQUAL_UINT16(src.motor.torque_ramp,  dst.motor.torque_ramp);
    TEST_ASSERT_EQUAL_UINT16(src.motor.current_limit, dst.motor.current_limit);
}

/* These five are the ones the audit called out by name: the live jam and
 * belt-break thresholds, silently lost on every power cycle in v0.1.0. */
static void test_jam_and_belt_break_thresholds_survive(void) {
    roundtrip();
    TEST_ASSERT_EQUAL_UINT8(src.sensor.step_thresh,       dst.sensor.step_thresh);
    TEST_ASSERT_EQUAL_UINT8(src.sensor.low_load_thresh,   dst.sensor.low_load_thresh);
    TEST_ASSERT_EQUAL(src.sensor.low_load_detect,         dst.sensor.low_load_detect);
    TEST_ASSERT_EQUAL_UINT8(src.sensor.stall_sensitivity, dst.sensor.stall_sensitivity);
    TEST_ASSERT_EQUAL_UINT16(src.sensor.stall_time_ms,    dst.sensor.stall_time_ms);
}

/* REVIEW FIX (HIGH): settings_unpack_custom() used to apply every field with
 * no range check at all — an additive byte-sum checksum is order-insensitive
 * and blind to compensating errors, so a corrupt-but-summing blob could put
 * spike_thresh = 0 into live settings, which fires an emergency stop seconds
 * into every cut. These assert the clamp, so it cannot quietly rot. */
static void test_out_of_range_blob_is_clamped_not_applied(void) {
    mutate_all(&src);
    settings_pack_custom(&src, &blob);

    /* Corrupt the blob past every bound, then re-stamp the checksum so it
     * passes validation exactly as a compensating-error corruption would. */
    blob.spike_thresh    = 0;      /* below the 20 floor: emergency-stop value */
    blob.stall_time_ms20 = 255;    /* 255*20 = 5100, above the 5000 ceiling */
    blob.temp_threshold  = 5;      /* below the 40 floor */
    blob.depth_offset    = -9000;  /* not a possible ADC reading */
    /* power_output can no longer BE out of range: v2 stores it in two bits of
     * tap_misc, so 0-3 is all that fits. Set the maximum and check it survives
     * rather than pretending an impossible value can be stored. */
    blob.tap_misc |= (uint8_t)(3u << EE_TM_POWER_OUTPUT_SHIFT);
    blob.units           = 200;    /* enum has three */
    blob.checksum = settings_custom_checksum(&blob);

    memset(&dst, 0, sizeof(dst));
    TEST_ASSERT_TRUE(settings_unpack_custom(&blob, &dst));

    TEST_ASSERT_EQUAL_UINT16(20,   dst.sensor.spike_thresh);
    TEST_ASSERT_EQUAL_UINT16(5000, dst.sensor.stall_time_ms);
    TEST_ASSERT_EQUAL_UINT8(40,    dst.power.temp_threshold);
    TEST_ASSERT_EQUAL_INT16(0,     dst.depth.offset);
    TEST_ASSERT_EQUAL_UINT8(3,     dst.power.power_output);
    TEST_ASSERT_EQUAL(UNITS_METRIC, dst.display.units);
}

/* A byte transposition inside a 16-bit field is invisible to an additive
 * checksum. voltage_kp = 2000 is D0 07 little-endian; swapped it reads 07 D0 =
 * 2000... so use a pair whose swap lands negative, which is the shape that
 * reaches the MCB via motor_sync_settings() at every boot. */
static void test_transposed_pid_gain_is_clamped(void) {
    mutate_all(&src);
    settings_pack_custom(&src, &blob);
    blob.voltage_kp = (int16_t)0xD007;      /* what a swapped 0x07D0 looks like */
    blob.checksum = settings_custom_checksum(&blob);

    memset(&dst, 0, sizeof(dst));
    TEST_ASSERT_TRUE(settings_unpack_custom(&blob, &dst));
    TEST_ASSERT_TRUE(dst.motor.voltage_kp >= 0);
}

/* The four tapping fields that decide how long the spindle actually runs
 * BACKWARDS. They are not in the custom blob (they are among the thirteen
 * defaulted at every boot on an EEPROM unit), so drive settings_clamp_loaded()
 * directly — the flash-unit path restores them verbatim. Until trigger
 * reverses carried a duration none of these were read, so an out-of-range
 * value was inert; now a stored 0 in load_increase_reverse_ms would mean
 * "reverse until the 30 s backstop". */
static void test_tapping_reverse_fields_are_clamped(void) {
    settings_t s;
    memset(&s, 0, sizeof(s));
    s.tapping.load_increase_reverse_ms = 0;      /* 0 = open-ended reverse */
    s.tapping.clutch_action            = 200;    /* enum has two values */
    s.tapping.pedal_action             = 200;    /* enum has two values */
    s.tapping.pedal_chip_break_ms      = 60000;  /* a minute of reverse */
    settings_clamp_loaded(&s);
    TEST_ASSERT_EQUAL_UINT16(50,  s.tapping.load_increase_reverse_ms);
    TEST_ASSERT_EQUAL_UINT8(1,    s.tapping.clutch_action);
    TEST_ASSERT_EQUAL_UINT8(1,    s.tapping.pedal_action);
    TEST_ASSERT_EQUAL_UINT16(500, s.tapping.pedal_chip_break_ms);

    /* Over the top end too, and in-range values untouched. */
    s.tapping.load_increase_reverse_ms = 9000;
    s.tapping.pedal_chip_break_ms      = 10;
    settings_clamp_loaded(&s);
    TEST_ASSERT_EQUAL_UINT16(2000, s.tapping.load_increase_reverse_ms);
    TEST_ASSERT_EQUAL_UINT16(50,   s.tapping.pedal_chip_break_ms);

    s.tapping.load_increase_reverse_ms = 200;
    s.tapping.clutch_action            = 0;
    s.tapping.pedal_action             = 1;
    s.tapping.pedal_chip_break_ms      = 250;
    settings_clamp_loaded(&s);
    TEST_ASSERT_EQUAL_UINT16(200, s.tapping.load_increase_reverse_ms);
    TEST_ASSERT_EQUAL_UINT8(0,    s.tapping.clutch_action);
    TEST_ASSERT_EQUAL_UINT8(1,    s.tapping.pedal_action);
    TEST_ASSERT_EQUAL_UINT16(250, s.tapping.pedal_chip_break_ms);
}

/* The five completion actions decide how a tapping cycle ENDS. Depth is the
 * odd one out: COMPLETION_RESUME (3) is a valid choice for quill, load and
 * load-slip, but for depth it would re-trigger on the next poll (depth >=
 * target is still true after a back-off) and chatter, so the loader caps it at
 * REVERSE_TIMED. */
static void test_completion_actions_are_clamped(void) {
    settings_t s;
    memset(&s, 0, sizeof(s));
    s.tapping.depth_completion_action     = 3;   /* RESUME: not valid for depth */
    s.tapping.quill_completion_action     = 9;
    s.tapping.load_completion_action      = 9;
    s.tapping.load_slip_completion_action = 9;
    settings_clamp_loaded(&s);
    TEST_ASSERT_EQUAL_UINT8(2, s.tapping.depth_completion_action);
    TEST_ASSERT_EQUAL_UINT8(3, s.tapping.quill_completion_action);
    TEST_ASSERT_EQUAL_UINT8(3, s.tapping.load_completion_action);
    TEST_ASSERT_EQUAL_UINT8(3, s.tapping.load_slip_completion_action);

    /* RESUME survives on the three triggers that accept it. */
    s.tapping.quill_completion_action     = 3;
    s.tapping.load_completion_action      = 3;
    s.tapping.load_slip_completion_action = 3;
    s.tapping.depth_completion_action     = 1;
    settings_clamp_loaded(&s);
    TEST_ASSERT_EQUAL_UINT8(3, s.tapping.quill_completion_action);
    TEST_ASSERT_EQUAL_UINT8(3, s.tapping.load_completion_action);
    TEST_ASSERT_EQUAL_UINT8(3, s.tapping.load_slip_completion_action);
    TEST_ASSERT_EQUAL_UINT8(1, s.tapping.depth_completion_action);
}

/* In-range values must pass through the clamp untouched. */
static void test_in_range_blob_is_untouched_by_clamp(void) {
    roundtrip();
    TEST_ASSERT_EQUAL_UINT16(src.sensor.spike_thresh,  dst.sensor.spike_thresh);
    TEST_ASSERT_EQUAL_UINT16(src.sensor.stall_time_ms, dst.sensor.stall_time_ms);
    TEST_ASSERT_EQUAL_UINT8(src.power.temp_threshold,  dst.power.temp_threshold);
    TEST_ASSERT_EQUAL_INT16(src.depth.offset,          dst.depth.offset);
}

static void test_remaining_sensor_fields_survive(void) {
    roundtrip();
    TEST_ASSERT_EQUAL_UINT8(src.sensor.overload_threshold,    dst.sensor.overload_threshold);
    TEST_ASSERT_EQUAL_UINT16(src.sensor.spike_thresh,         dst.sensor.spike_thresh);
    TEST_ASSERT_EQUAL_UINT8(src.sensor.vibration_sensitivity, dst.sensor.vibration_sensitivity);
    TEST_ASSERT_EQUAL(src.sensor.jam_detect,          dst.sensor.jam_detect);
    TEST_ASSERT_EQUAL(src.sensor.spike_detect,        dst.sensor.spike_detect);
    TEST_ASSERT_EQUAL(src.sensor.guard_check_enabled, dst.sensor.guard_check_enabled);
    TEST_ASSERT_EQUAL(src.sensor.pedal_enabled,       dst.sensor.pedal_enabled);
}

static void test_depth_fields_survive(void) {
    roundtrip();
    TEST_ASSERT_EQUAL(src.depth.mode,          dst.depth.mode);
    TEST_ASSERT_EQUAL(src.depth.action,        dst.depth.action);
    TEST_ASSERT_EQUAL_INT16(src.depth.target,  dst.depth.target);
    TEST_ASSERT_EQUAL_INT16(src.depth.offset,  dst.depth.offset);
    TEST_ASSERT_EQUAL(src.depth.enabled,       dst.depth.enabled);
}

static void test_tapping_fields_survive(void) {
    roundtrip();
    TEST_ASSERT_EQUAL(src.tapping.depth_trigger_enabled, dst.tapping.depth_trigger_enabled);
    TEST_ASSERT_EQUAL(src.tapping.load_increase_enabled, dst.tapping.load_increase_enabled);
    TEST_ASSERT_EQUAL(src.tapping.load_slip_enabled,     dst.tapping.load_slip_enabled);
    TEST_ASSERT_EQUAL(src.tapping.clutch_slip_enabled,   dst.tapping.clutch_slip_enabled);
    TEST_ASSERT_EQUAL(src.tapping.quill_trigger_enabled, dst.tapping.quill_trigger_enabled);
    TEST_ASSERT_EQUAL(src.tapping.peck_trigger_enabled,  dst.tapping.peck_trigger_enabled);
    TEST_ASSERT_EQUAL(src.tapping.pedal_enabled,         dst.tapping.pedal_enabled);
    TEST_ASSERT_EQUAL_UINT16(src.tapping.speed_rpm,              dst.tapping.speed_rpm);
    TEST_ASSERT_EQUAL_UINT16(src.tapping.peck_fwd_ms,            dst.tapping.peck_fwd_ms);
    TEST_ASSERT_EQUAL_UINT16(src.tapping.peck_rev_ms,            dst.tapping.peck_rev_ms);
    TEST_ASSERT_EQUAL_UINT8(src.tapping.peck_cycles,             dst.tapping.peck_cycles);
    TEST_ASSERT_EQUAL_UINT16(src.tapping.brake_delay_ms,         dst.tapping.brake_delay_ms);
    TEST_ASSERT_EQUAL_UINT8(src.tapping.depth_completion_action, dst.tapping.depth_completion_action);
    TEST_ASSERT_EQUAL_UINT8(src.tapping.peck_completion_action,  dst.tapping.peck_completion_action);
    TEST_ASSERT_EQUAL_UINT16(src.tapping.peck_reverse_out_ms,    dst.tapping.peck_reverse_out_ms);
}

static void test_step_drill_and_speed_and_power_survive(void) {
    roundtrip();
    TEST_ASSERT_EQUAL(src.step_drill.enabled,                    dst.step_drill.enabled);
    TEST_ASSERT_EQUAL_UINT8(src.step_drill.start_diameter,       dst.step_drill.start_diameter);
    TEST_ASSERT_EQUAL_UINT8(src.step_drill.diameter_increment,   dst.step_drill.diameter_increment);
    TEST_ASSERT_EQUAL_UINT8(src.step_drill.step_depth_x2,        dst.step_drill.step_depth_x2);
    TEST_ASSERT_EQUAL_UINT16(src.step_drill.base_rpm,            dst.step_drill.base_rpm);
    TEST_ASSERT_EQUAL_UINT8(src.step_drill.target_diameter,      dst.step_drill.target_diameter);
    TEST_ASSERT_EQUAL_UINT16(src.speed.slow_start,               dst.speed.slow_start);
    TEST_ASSERT_EQUAL_UINT8(src.power.power_output,              dst.power.power_output);
    TEST_ASSERT_EQUAL_UINT8(src.power.temp_threshold,            dst.power.temp_threshold);
}

/* units_mode_t has three values. It gets a whole byte for exactly this reason:
 * a single flag bit would silently turn IMPERIAL_FRACTION into something else. */
static void test_all_three_unit_modes_survive(void) {
    const units_mode_t modes[] = { UNITS_METRIC, UNITS_IMPERIAL_DECIMAL,
                                   UNITS_IMPERIAL_FRACTION };
    for (int i = 0; i < 3; i++) {
        src.display.units = modes[i];
        roundtrip();
        TEST_ASSERT_EQUAL(modes[i], dst.display.units);
    }
}

/* Booleans share one byte; a wrong mask would alias two of them together. */
static void test_each_flag_is_independent(void) {
    bool* const flags[] = {
        &src.sensor.jam_detect, &src.sensor.spike_detect,
        &src.sensor.guard_check_enabled, &src.sensor.pedal_enabled,
        &src.step_drill.enabled, &src.sensor.low_load_detect,
        &src.depth.enabled,
    };
    bool* const out[] = {
        &dst.sensor.jam_detect, &dst.sensor.spike_detect,
        &dst.sensor.guard_check_enabled, &dst.sensor.pedal_enabled,
        &dst.step_drill.enabled, &dst.sensor.low_load_detect,
        &dst.depth.enabled,
    };
    const int n = (int)(sizeof(flags) / sizeof(flags[0]));

    for (int set = 0; set < n; set++) {
        for (int i = 0; i < n; i++) *flags[i] = (i == set);
        roundtrip();
        for (int i = 0; i < n; i++) {
            TEST_ASSERT_EQUAL_MESSAGE((i == set), *out[i],
                                      "packed booleans alias each other");
        }
    }
}

/* The seven tapping triggers share one byte. Moved here from test_settings,
 * which asserted the same thing against its own COPY of eeprom_custom_t — a
 * copy that had silently fallen back to the v2 layout, so those tests were
 * passing against a struct the firmware no longer writes. Same assertions,
 * now against the shipping header. */
static void test_each_tap_trigger_bit_is_independent(void) {
    bool* const in[] = {
        &src.tapping.depth_trigger_enabled, &src.tapping.load_increase_enabled,
        &src.tapping.load_slip_enabled,     &src.tapping.clutch_slip_enabled,
        &src.tapping.quill_trigger_enabled, &src.tapping.peck_trigger_enabled,
        &src.tapping.pedal_enabled,
    };
    bool* const out[] = {
        &dst.tapping.depth_trigger_enabled, &dst.tapping.load_increase_enabled,
        &dst.tapping.load_slip_enabled,     &dst.tapping.clutch_slip_enabled,
        &dst.tapping.quill_trigger_enabled, &dst.tapping.peck_trigger_enabled,
        &dst.tapping.pedal_enabled,
    };
    const int n = (int)(sizeof(in) / sizeof(in[0]));

    for (int set = 0; set < n; set++) {
        for (int i = 0; i < n; i++) *in[i] = (i == set);
        roundtrip();
        for (int i = 0; i < n; i++) {
            TEST_ASSERT_EQUAL_MESSAGE((i == set), *out[i],
                                      "packed tapping triggers alias each other");
        }
    }
}

static void test_all_tap_triggers_off_and_all_on(void) {
    bool* const in[] = {
        &src.tapping.depth_trigger_enabled, &src.tapping.load_increase_enabled,
        &src.tapping.load_slip_enabled,     &src.tapping.clutch_slip_enabled,
        &src.tapping.quill_trigger_enabled, &src.tapping.peck_trigger_enabled,
        &src.tapping.pedal_enabled,
    };
    for (int on = 0; on < 2; on++) {
        for (int i = 0; i < 7; i++) *in[i] = (on != 0);
        roundtrip();
        TEST_ASSERT_EQUAL(on != 0, dst.tapping.depth_trigger_enabled);
        TEST_ASSERT_EQUAL(on != 0, dst.tapping.pedal_enabled);
        TEST_ASSERT_EQUAL_UINT8(on ? 0x7F : 0x00, blob.tap_triggers);
    }
}

/* The magic and version are what stop a foreign or stale block being parsed;
 * assert the shipped values reach the wire, not just that a round trip works. */
static void test_packed_blob_carries_the_shipping_magic_and_version(void) {
    settings_pack_custom(&src, &blob);
    TEST_ASSERT_EQUAL_HEX8(EE_CUSTOM_MAGIC_VALUE, blob.magic);
    TEST_ASSERT_EQUAL_UINT8(EE_CUSTOM_VERSION_NUM, blob.version);
}

static void test_checksum_is_deterministic(void) {
    settings_pack_custom(&src, &blob);
    const uint16_t first = blob.checksum;
    eeprom_custom_t again;
    settings_pack_custom(&src, &again);
    TEST_ASSERT_EQUAL_UINT16(first, again.checksum);
    TEST_ASSERT_EQUAL_UINT16(settings_custom_checksum(&blob), blob.checksum);
}

/*--- validation ------------------------------------------------------------*/

static void test_corrupt_checksum_is_rejected(void) {
    settings_pack_custom(&src, &blob);
    blob.checksum ^= 0x0001;
    memset(&dst, 0xA5, sizeof(dst));
    TEST_ASSERT_FALSE(settings_unpack_custom(&blob, &dst));
}

static void test_single_flipped_data_bit_is_rejected(void) {
    settings_pack_custom(&src, &blob);
    blob.stall_time_ms20 ^= 0x10;
    TEST_ASSERT_FALSE(settings_unpack_custom(&blob, &dst));
}

static void test_wrong_magic_is_rejected(void) {
    settings_pack_custom(&src, &blob);
    blob.magic = 0x00;
    TEST_ASSERT_FALSE(settings_unpack_custom(&blob, &dst));
}

static void test_older_layout_version_is_rejected(void) {
    settings_pack_custom(&src, &blob);
    blob.version = EE_CUSTOM_VERSION_NUM - 1;
    blob.checksum = settings_custom_checksum(&blob);  /* otherwise checksum catches it */
    TEST_ASSERT_FALSE(settings_unpack_custom(&blob, &dst));
}

static void test_blank_eeprom_is_rejected(void) {
    memset(&blob, 0xFF, sizeof(blob));
    TEST_ASSERT_FALSE(settings_unpack_custom(&blob, &dst));
}

/* A rejected blob must not leave the caller's settings half-written. */
static void test_rejected_blob_leaves_settings_untouched(void) {
    settings_t before;
    mutate_all(&before);
    memcpy(&dst, &before, sizeof(dst));

    memset(&blob, 0xFF, sizeof(blob));
    TEST_ASSERT_FALSE(settings_unpack_custom(&blob, &dst));
    TEST_ASSERT_EQUAL_MEMORY(&before, &dst, sizeof(before));
}

/*--- the budget ------------------------------------------------------------*/

/* The static assertions in eeprom_layout.h enforce this at build time; stating
 * it here too means the numbers appear in the test log where a reader sees
 * them. 0xB0..0xEC settings, 0xED..0xFF crash dump, nothing overlapping. */
/* v2 stores five ms fields scaled into a byte. That is a real tradeoff and it
 * is pinned here so nobody "fixes" the rounding later without seeing the cost:
 * values on the menu grid survive exactly, off-grid values round DOWN to the
 * scale. Every menu row steps at or above its field's scale (FwdMs/RevMs 50,
 * RevTime 100, ChipMs 10, RevTim 50), so nothing reachable from the panel
 * loses a digit — only a console-typed odd number does. */
static void test_scaled_ms_fields_quantise_to_their_scale(void) {
    mutate_all(&src);
    src.tapping.brake_delay_ms         = 275;   /* off-grid */
    src.tapping.peck_rev_ms            = 1005;  /* off-grid */
    src.tapping.peck_fwd_ms            = 1000;  /* on the 50 grid */
    src.tapping.peck_reverse_out_ms    = 1000;  /* on the 50 grid */
    src.tapping.pedal_chip_break_ms    = 205;   /* off-grid */
    roundtrip();
    TEST_ASSERT_EQUAL_UINT16(270,  dst.tapping.brake_delay_ms);
    TEST_ASSERT_EQUAL_UINT16(1000, dst.tapping.peck_rev_ms);
    TEST_ASSERT_EQUAL_UINT16(1000, dst.tapping.peck_fwd_ms);
    TEST_ASSERT_EQUAL_UINT16(1000, dst.tapping.peck_reverse_out_ms);
    TEST_ASSERT_EQUAL_UINT16(200,  dst.tapping.pedal_chip_break_ms);
}

/* The twelve tapping fields that v1 could not hold must now survive a
 * power cycle. This is the whole point of the v2 repack. */
static void test_the_twelve_formerly_lost_fields_survive(void) {
    mutate_all(&src);
    src.tapping.quill_pedal_mode            = 2;
    src.tapping.quill_completion_action     = 3;
    src.tapping.load_completion_action      = 3;
    src.tapping.load_slip_completion_action = 1;
    src.tapping.clutch_action               = 1;
    src.tapping.peck_depth_stop             = false;
    src.tapping.pedal_action                = 1;
    src.tapping.load_increase_threshold     = 45;
    src.tapping.load_slip_cv_percent        = 150;
    src.tapping.load_increase_reverse_ms    = 300;
    src.tapping.clutch_plateau_ms           = 250;
    src.tapping.pedal_chip_break_ms         = 150;
    roundtrip();
    TEST_ASSERT_EQUAL_UINT8(2,   dst.tapping.quill_pedal_mode);
    TEST_ASSERT_EQUAL_UINT8(3,   dst.tapping.quill_completion_action);
    TEST_ASSERT_EQUAL_UINT8(3,   dst.tapping.load_completion_action);
    TEST_ASSERT_EQUAL_UINT8(1,   dst.tapping.load_slip_completion_action);
    TEST_ASSERT_EQUAL_UINT8(1,   dst.tapping.clutch_action);
    TEST_ASSERT_EQUAL(false,     dst.tapping.peck_depth_stop);
    TEST_ASSERT_EQUAL_UINT8(1,   dst.tapping.pedal_action);
    TEST_ASSERT_EQUAL_UINT8(45,  dst.tapping.load_increase_threshold);
    TEST_ASSERT_EQUAL_UINT16(150, dst.tapping.load_slip_cv_percent);
    TEST_ASSERT_EQUAL_UINT16(300, dst.tapping.load_increase_reverse_ms);
    TEST_ASSERT_EQUAL_UINT16(250, dst.tapping.clutch_plateau_ms);
    TEST_ASSERT_EQUAL_UINT16(150, dst.tapping.pedal_chip_break_ms);
}

static void test_block_fits_its_eeprom_budget(void) {
    /* Was "exactly EE_CUSTOM_SIZE" when the block was full. v2 is one byte
     * smaller; assert the bound and the headroom separately so a future field
     * that overruns still fails here rather than eating the crash dump. */
    TEST_ASSERT_TRUE(sizeof(eeprom_custom_t) <= EE_CUSTOM_SIZE);
    TEST_ASSERT_EQUAL_size_t(60, sizeof(eeprom_custom_t));
    TEST_ASSERT_EQUAL_INT(EE_CRASH_BASE, EE_CUSTOM_BASE + EE_CUSTOM_SIZE);
    TEST_ASSERT_EQUAL_INT(0x100, EE_CRASH_BASE + EE_CRASH_SIZE);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_motor_fields_survive);
    RUN_TEST(test_jam_and_belt_break_thresholds_survive);
    RUN_TEST(test_out_of_range_blob_is_clamped_not_applied);
    RUN_TEST(test_transposed_pid_gain_is_clamped);
    RUN_TEST(test_tapping_reverse_fields_are_clamped);
    RUN_TEST(test_completion_actions_are_clamped);
    RUN_TEST(test_in_range_blob_is_untouched_by_clamp);
    RUN_TEST(test_remaining_sensor_fields_survive);
    RUN_TEST(test_depth_fields_survive);
    RUN_TEST(test_tapping_fields_survive);
    RUN_TEST(test_step_drill_and_speed_and_power_survive);
    RUN_TEST(test_all_three_unit_modes_survive);
    RUN_TEST(test_each_flag_is_independent);
    RUN_TEST(test_each_tap_trigger_bit_is_independent);
    RUN_TEST(test_all_tap_triggers_off_and_all_on);
    RUN_TEST(test_packed_blob_carries_the_shipping_magic_and_version);
    RUN_TEST(test_checksum_is_deterministic);
    RUN_TEST(test_corrupt_checksum_is_rejected);
    RUN_TEST(test_single_flipped_data_bit_is_rejected);
    RUN_TEST(test_wrong_magic_is_rejected);
    RUN_TEST(test_older_layout_version_is_rejected);
    RUN_TEST(test_blank_eeprom_is_rejected);
    RUN_TEST(test_rejected_blob_leaves_settings_untouched);
    RUN_TEST(test_scaled_ms_fields_quantise_to_their_scale);
    RUN_TEST(test_the_twelve_formerly_lost_fields_survive);
    RUN_TEST(test_block_fits_its_eeprom_budget);
    return UNITY_END();
}
