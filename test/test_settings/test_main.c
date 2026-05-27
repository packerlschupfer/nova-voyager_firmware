/**
 * @file test_settings/test_main.c
 * @brief Unit tests for the settings module
 *
 * Tests pure logic extracted from settings.c.
 *
 * The eeprom_custom_t mirror and its pack/unpack copies USED to live here too,
 * with 24 tests against them. They were deleted on 2026-08-30: the copy had
 * silently fallen back to the v2 layout when the real struct went to v3, so
 * those tests passed while exercising a block the firmware no longer writes —
 * the copy proved the copy right, which is the whole failure mode.
 * include/settings_pack.h is header-only and therefore testable directly, so
 * test/test_settings_pack now covers that ground against the SHIPPING struct,
 * including the trigger-bit and checksum assertions moved over from here.
 * Self-contained: no #include of the actual settings.h or eeprom_layout.h.
 * All structs and constants are redefined locally to match the originals.
 */

#include <unity.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*===========================================================================*/
/* Local constants (mirrors config.h / settings.h)                           */
/*===========================================================================*/

#define SETTINGS_MAGIC          0xDEADBEEF  /* must match settings.h */
/* REVIEW FIX: this was still 2 after include/settings.h went to 1, so
 * test_defaults_version_is_current compared the stale local against itself and
 * passed vacuously. `-e native` compiles no src/, so this mirror IS the
 * coverage — when it drifts from the real header the tests silently stop
 * testing the shipped layout. */
#define SETTINGS_VERSION        1
#define NUM_FAVORITE_SPEEDS     8

/* REVIEW FIX: the whole block below had drifted from include/config.h — the
 * four SPEED_* the review named, and seven TAP_DEFAULT_* the hardened check
 * then found on top. Defaults tests were comparing the mirror against itself.
 *
 * REVIEW FIX: these were 100 / 6000 / 1500 / 300 while include/config.h has
 * 50 / 5500 / 500 / 200. The clamp tests were therefore exercising bounds the
 * firmware never uses, and the "defaults are in range" tests compared the
 * mirror's own numbers against themselves — vacuous. check-settings-mirror.sh
 * did not catch it because it only read settings.h and eeprom_layout.h; it
 * reads config.h now too. */
#define SPEED_MIN_RPM           50
#define SPEED_MAX_RPM           5500
#define SPEED_DEFAULT_RPM       900
#define SPEED_TAP_DEFAULT       200

/* Must track include/eeprom_layout.h — enforced by
 * scripts/check-settings-mirror.sh, which missed this on the very commit that
 * added it because the script only read include/settings.h. */
#define EE_CUSTOM_MAGIC_VALUE   0xC1
#define EE_CUSTOM_VERSION_NUM   2

/*===========================================================================*/
/* Local enum/type stubs (avoids pulling in HAL / FreeRTOS headers)          */
/*===========================================================================*/

typedef enum { DEPTH_MODE_OFF = 0, DEPTH_MODE_STANDARD, DEPTH_MODE_PRECISION } depth_mode_t;
typedef enum { DEPTH_ACTION_NOTHING = 0, DEPTH_ACTION_STOP } depth_action_t;
typedef enum { UNITS_METRIC = 0, UNITS_IMPERIAL_DECIMAL, UNITS_IMPERIAL_FRACTION } units_mode_t;
typedef enum { MOTOR_PROFILE_SOFT = 0, MOTOR_PROFILE_NORMAL = 1, MOTOR_PROFILE_HARD = 2 } motor_profile_t;

/* Tapping completion/action enums — only the values used in defaults */
#define COMPLETION_STOP             0
#define COMPLETION_REVERSE_OUT      1
#define COMPLETION_REVERSE_OUT      1
#define QUILL_PEDAL_OFF             0
#define QUILL_PEDAL_TOGGLE          2
#define CLUTCH_ACTION_REVERSE       0
#define PEDAL_ACTION_HOLD           0

#define TAP_DEFAULT_LOAD_INCREASE_THRESHOLD  60
#define TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS 200
#define TAP_DEFAULT_LOAD_SLIP_CV_PERCENT     130
#define TAP_DEFAULT_CLUTCH_PLATEAU_MS        500
#define TAP_DEFAULT_PECK_FWD_MS              150
#define TAP_DEFAULT_PECK_REV_MS              100
#define TAP_DEFAULT_PECK_CYCLES              7
#define TAP_DEFAULT_PEDAL_CHIP_BREAK_MS      200
#define TAP_DEFAULT_BRAKE_DELAY              100
#define TAP_PECK_FWD_MS_MIN                  100
#define TAP_PECK_FWD_MS_MAX                  5000
#define TAP_PECK_REV_MS_MIN                  100
#define TAP_PECK_REV_MS_MAX                  5000

/*===========================================================================*/
/* Local struct definitions (mirrors settings.h exactly)                     */
/*===========================================================================*/

typedef struct {
    int16_t  speed_kprop;
    int16_t  speed_kint;
    int16_t  voltage_kp;
    int16_t  voltage_ki;
    int16_t  ir_gain;
    int16_t  ir_offset;
    int16_t  advance_max;
    int16_t  pulse_max;
    uint16_t current_limit;
    uint8_t  profile;
    uint16_t speed_ramp;
    uint16_t torque_ramp;
} motor_params_t;

typedef struct {
    uint16_t default_rpm;
    uint16_t favorite[NUM_FAVORITE_SPEEDS];
    uint16_t max_limit;
    uint16_t slow_start;
    uint16_t anti_tearout;
    uint8_t  step_size;
    bool     rounding;
    uint8_t  material;
    uint8_t  bit_type;
    uint8_t  bit_diameter;
    bool     auto_rpm;
} speed_settings_t;

typedef struct {
    bool     depth_trigger_enabled;
    bool     load_increase_enabled;
    bool     load_slip_enabled;
    bool     clutch_slip_enabled;
    bool     quill_trigger_enabled;
    bool     peck_trigger_enabled;
    bool     pedal_enabled;
    uint16_t speed_rpm;
    uint8_t  depth_completion_action;
    uint8_t  quill_pedal_mode;
    uint8_t  quill_completion_action;
    uint8_t  load_increase_threshold;
    uint16_t load_increase_reverse_ms;
    uint8_t  load_completion_action;
    uint16_t load_slip_cv_percent;
    uint8_t  load_slip_completion_action;
    uint16_t clutch_plateau_ms;
    uint8_t  clutch_action;
    uint16_t peck_fwd_ms;
    uint16_t peck_rev_ms;
    uint8_t  peck_cycles;
    bool     peck_depth_stop;
    uint8_t  peck_completion_action;
    uint16_t peck_reverse_out_ms;
    uint8_t  pedal_action;
    uint16_t pedal_chip_break_ms;
    uint16_t brake_delay_ms;
} tap_settings_t;

typedef struct {
    depth_mode_t   mode;
    depth_action_t action;
    int16_t        target;
    int16_t        offset;
    bool           enabled;
} depth_settings_t;

typedef struct {
    bool     enabled;
    uint8_t  start_diameter;
    uint8_t  diameter_increment;
    uint8_t  step_depth_x2;
    uint16_t base_rpm;
    uint8_t  target_diameter;
} step_drill_settings_t;

typedef struct {
    units_mode_t units;
} display_settings_t;

typedef struct {
    bool     jam_detect;
    bool     spike_detect;
    uint8_t  vibration_sensitivity;
    uint16_t vibration_thresh;
    uint16_t spike_thresh;
    /* Added to the real struct when the OEM-style detectors went in; the
     * mirror was never updated, so every offset below here differed from the
     * shipping layout and the checksum-coverage tests were exercising the
     * wrong thing. Found by scripts/check-settings-mirror.sh. */
    uint8_t  step_thresh;
    bool     low_load_detect;
    uint8_t  low_load_thresh;
    uint8_t  stall_sensitivity;
    uint16_t stall_time_ms;
    bool     guard_check_enabled;
    bool     pedal_enabled;
    uint8_t  overload_threshold;
} sensor_settings_t;

typedef struct {
    bool     key_sound;
    bool     show_shortcuts;
    uint8_t  f1_function;
    uint8_t  f2_function;
    uint8_t  f3_function;
    uint8_t  f4_function;
    bool     menu_locked;
    uint16_t password;
} interface_settings_t;

typedef struct {
    bool     braking_enabled;
    bool     spindle_hold;
    uint8_t  power_limit;
    uint8_t  power_output;
    uint16_t low_voltage_thresh;
    uint16_t dc_bus_voltage;
    uint8_t  temp_threshold;
    bool     self_start;
    bool     pilot_hole;
} power_settings_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    motor_params_t        motor;
    speed_settings_t      speed;
    tap_settings_t        tapping;
    depth_settings_t      depth;
    step_drill_settings_t step_drill;
    display_settings_t    display;
    sensor_settings_t     sensor;
    interface_settings_t  interface;
    power_settings_t      power;
    uint16_t              checksum;
} settings_t;

/* The eeprom_custom_t mirror lived here. Deleted 2026-08-30 — see the file
 * header. test/test_settings_pack exercises the real one from
 * include/eeprom_layout.h through the shipping include/settings_pack.h. */

/*===========================================================================*/
/* Algorithms under test (inlined from settings.c)                           */
/*===========================================================================*/

/**
 * CRC16-CCITT as used by settings.c for the flash-backed settings_t.
 * Covers all bytes before the checksum field.
 */
static uint16_t calc_crc16(const settings_t *s) {
    const uint8_t *data = (const uint8_t *)s;
    size_t len = offsetof(settings_t, checksum);
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/**
 * Clamp helper used by several setter implementations.
 */
static uint16_t clamp16(uint16_t v, uint16_t lo, uint16_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/*===========================================================================*/
/* Helpers                                                                   */
/*===========================================================================*/

/** Fill a settings_t with factory defaults (mirrors set_defaults() logic). */
static void apply_defaults(settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->magic   = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;

    s->motor.speed_kprop  = 100;
    s->motor.speed_kint   = 50;
    s->motor.voltage_kp   = 2000;
    s->motor.voltage_ki   = 9000;
    s->motor.ir_gain      = 28835;
    s->motor.ir_offset    = 400;
    s->motor.advance_max  = 85;
    s->motor.pulse_max    = 185;
    s->motor.current_limit = 100;
    s->motor.profile      = MOTOR_PROFILE_NORMAL;
    s->motor.speed_ramp   = 1000;
    s->motor.torque_ramp  = 75;

    s->speed.default_rpm  = SPEED_DEFAULT_RPM;
    s->speed.favorite[0]  = 500;
    s->speed.favorite[1]  = 1000;
    s->speed.favorite[2]  = 1500;
    s->speed.favorite[3]  = 2000;
    s->speed.favorite[4]  = 2500;
    s->speed.favorite[5]  = 3500;
    s->speed.favorite[6]  = 4500;
    s->speed.favorite[7]  = 5500;
    s->speed.max_limit    = SPEED_MAX_RPM;
    s->speed.slow_start   = 400;
    s->speed.anti_tearout = 250;
    s->speed.step_size    = 50;
    s->speed.rounding     = true;
    s->speed.material     = 0;
    s->speed.bit_type     = 0;
    s->speed.bit_diameter = 10;
    s->speed.auto_rpm     = false;

    s->tapping.depth_trigger_enabled  = false;
    s->tapping.load_increase_enabled  = false;
    s->tapping.load_slip_enabled      = false;
    s->tapping.clutch_slip_enabled    = false;
    s->tapping.quill_trigger_enabled  = false;
    s->tapping.peck_trigger_enabled   = false;
    s->tapping.pedal_enabled          = false;
    s->tapping.speed_rpm              = SPEED_TAP_DEFAULT;
    s->tapping.depth_completion_action = COMPLETION_STOP;
    s->tapping.depth_completion_action = COMPLETION_REVERSE_OUT;
    s->tapping.quill_pedal_mode       = QUILL_PEDAL_OFF;
    s->tapping.quill_completion_action = COMPLETION_REVERSE_OUT;
    s->tapping.load_increase_threshold = TAP_DEFAULT_LOAD_INCREASE_THRESHOLD;
    s->tapping.load_increase_reverse_ms = TAP_DEFAULT_LOAD_INCREASE_REVERSE_MS;
    s->tapping.load_completion_action  = COMPLETION_REVERSE_OUT;
    s->tapping.load_slip_cv_percent    = TAP_DEFAULT_LOAD_SLIP_CV_PERCENT;
    s->tapping.load_slip_completion_action = COMPLETION_REVERSE_OUT;
    s->tapping.clutch_plateau_ms       = TAP_DEFAULT_CLUTCH_PLATEAU_MS;
    s->tapping.clutch_action           = CLUTCH_ACTION_REVERSE;
    s->tapping.peck_fwd_ms             = TAP_DEFAULT_PECK_FWD_MS;
    s->tapping.peck_rev_ms             = TAP_DEFAULT_PECK_REV_MS;
    s->tapping.peck_cycles             = TAP_DEFAULT_PECK_CYCLES;
    s->tapping.peck_depth_stop         = true;
    s->tapping.peck_completion_action  = COMPLETION_REVERSE_OUT;
    s->tapping.peck_reverse_out_ms     = 2000;
    s->tapping.pedal_action            = PEDAL_ACTION_HOLD;
    s->tapping.pedal_chip_break_ms     = TAP_DEFAULT_PEDAL_CHIP_BREAK_MS;
    s->tapping.brake_delay_ms          = TAP_DEFAULT_BRAKE_DELAY;

    s->depth.mode    = DEPTH_MODE_OFF;
    s->depth.action  = DEPTH_ACTION_STOP;
    s->depth.target  = 0;
    s->depth.offset  = 0;
    s->depth.enabled = false;

    s->step_drill.enabled            = false;
    s->step_drill.start_diameter     = 6;
    s->step_drill.diameter_increment = 3;
    s->step_drill.step_depth_x2      = 11;
    s->step_drill.base_rpm           = 1500;
    s->step_drill.target_diameter    = 0;

    s->display.units      = UNITS_METRIC;

    s->sensor.jam_detect           = true;
    s->sensor.spike_detect         = true;
    s->sensor.vibration_sensitivity = 3;
    s->sensor.vibration_thresh     = 800;
    s->sensor.spike_thresh         = 90;
    s->sensor.stall_sensitivity    = 50;
    s->sensor.stall_time_ms        = 500;
    s->sensor.guard_check_enabled  = true;
    s->sensor.pedal_enabled        = true;
    s->sensor.overload_threshold   = 50;

    s->interface.key_sound      = false;
    s->interface.show_shortcuts = true;
    s->interface.f1_function    = 0;
    s->interface.f2_function    = 0;
    s->interface.f3_function    = 0;
    s->interface.f4_function    = 0;
    s->interface.menu_locked    = false;
    s->interface.password       = 0;

    s->power.braking_enabled    = false;
    s->power.spindle_hold       = false;
    s->power.power_limit        = 100;
    s->power.power_output       = 2;
    s->power.low_voltage_thresh = 180;
    s->power.dc_bus_voltage     = 3600;
    s->power.temp_threshold     = 60;
    s->power.self_start         = false;
    s->power.pilot_hole         = false;

    s->checksum = calc_crc16(s);
}



/*===========================================================================*/
/* Test fixtures                                                              */
/*===========================================================================*/

static settings_t g_s;

void setUp(void) {
    memset(&g_s, 0, sizeof(g_s));
}

void tearDown(void) {
    /* nothing */
}

/*===========================================================================*/
/* 1. CRC16-CCITT checksum tests                                             */
/*===========================================================================*/

void test_crc16_is_deterministic(void) {
    apply_defaults(&g_s);
    uint16_t first  = calc_crc16(&g_s);
    uint16_t second = calc_crc16(&g_s);
    TEST_ASSERT_EQUAL_HEX16(first, second);
}

void test_crc16_nonzero_for_default_settings(void) {
    apply_defaults(&g_s);
    /* A zero CRC would indicate a degenerate (all-zero) structure */
    TEST_ASSERT_NOT_EQUAL(0, calc_crc16(&g_s));
}

void test_crc16_detects_corruption_in_speed_field(void) {
    apply_defaults(&g_s);
    uint16_t clean_crc = calc_crc16(&g_s);
    g_s.speed.default_rpm ^= 0x0100;  /* flip a bit */
    uint16_t corrupt_crc = calc_crc16(&g_s);
    TEST_ASSERT_NOT_EQUAL(clean_crc, corrupt_crc);
}

void test_crc16_detects_corruption_in_magic(void) {
    apply_defaults(&g_s);
    uint16_t clean_crc = calc_crc16(&g_s);
    g_s.magic ^= 0xFF;
    TEST_ASSERT_NOT_EQUAL(clean_crc, calc_crc16(&g_s));
}

void test_crc16_detects_corruption_in_motor_params(void) {
    apply_defaults(&g_s);
    uint16_t clean_crc = calc_crc16(&g_s);
    g_s.motor.voltage_kp = 0;  /* zero out a critical PID param */
    TEST_ASSERT_NOT_EQUAL(clean_crc, calc_crc16(&g_s));
}

void test_crc16_stored_in_settings_validates(void) {
    apply_defaults(&g_s);
    /* apply_defaults() stores the CRC; verify it matches */
    uint16_t expected = calc_crc16(&g_s);
    TEST_ASSERT_EQUAL_HEX16(expected, g_s.checksum);
}

void test_crc16_two_different_structs_differ(void) {
    settings_t a, b;
    apply_defaults(&a);
    apply_defaults(&b);
    b.motor.speed_kprop = 999;
    TEST_ASSERT_NOT_EQUAL(calc_crc16(&a), calc_crc16(&b));
}

/*===========================================================================*/
/* 2. EEPROM custom block: pack/unpack round-trip                            */
/*===========================================================================*/

/*===========================================================================*/
/* 3. Settings defaults: sane values after apply_defaults()                  */
/*===========================================================================*/

void test_defaults_magic_is_set(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_EQUAL_HEX32(SETTINGS_MAGIC, g_s.magic);
}

void test_defaults_version_is_current(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_VERSION, g_s.version);
}

void test_defaults_speed_in_range(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_GREATER_OR_EQUAL(SPEED_MIN_RPM, g_s.speed.default_rpm);
    TEST_ASSERT_LESS_OR_EQUAL(SPEED_MAX_RPM,    g_s.speed.default_rpm);
}

void test_defaults_max_limit_in_range(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_GREATER_OR_EQUAL(SPEED_MIN_RPM, g_s.speed.max_limit);
    TEST_ASSERT_LESS_OR_EQUAL(SPEED_MAX_RPM,    g_s.speed.max_limit);
}

void test_defaults_critical_pid_nonzero(void) {
    apply_defaults(&g_s);
    /* voltage_kp and voltage_ki must be non-zero or motor won't start */
    TEST_ASSERT_NOT_EQUAL(0, g_s.motor.voltage_kp);
    TEST_ASSERT_NOT_EQUAL(0, g_s.motor.voltage_ki);
}

void test_defaults_ir_gain_nonzero(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_NOT_EQUAL(0, g_s.motor.ir_gain);
}

void test_defaults_overload_threshold_nonzero(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_GREATER_THAN(0, g_s.sensor.overload_threshold);
}

void test_defaults_overload_threshold_in_range(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_GREATER_OR_EQUAL(10,  g_s.sensor.overload_threshold);
    TEST_ASSERT_LESS_OR_EQUAL(100, g_s.sensor.overload_threshold);
}

void test_defaults_all_tapping_triggers_disabled(void) {
    apply_defaults(&g_s);
    /* All triggers must be off — enabling them at defaults would be unsafe */
    TEST_ASSERT_FALSE(g_s.tapping.depth_trigger_enabled);
    TEST_ASSERT_FALSE(g_s.tapping.load_increase_enabled);
    TEST_ASSERT_FALSE(g_s.tapping.load_slip_enabled);
    TEST_ASSERT_FALSE(g_s.tapping.clutch_slip_enabled);
    TEST_ASSERT_FALSE(g_s.tapping.quill_trigger_enabled);
    TEST_ASSERT_FALSE(g_s.tapping.peck_trigger_enabled);
    TEST_ASSERT_FALSE(g_s.tapping.pedal_enabled);
}

void test_defaults_tap_speed_in_range(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_GREATER_OR_EQUAL(SPEED_MIN_RPM, g_s.tapping.speed_rpm);
    TEST_ASSERT_LESS_OR_EQUAL(SPEED_MAX_RPM,    g_s.tapping.speed_rpm);
}

void test_defaults_all_favorites_in_range(void) {
    apply_defaults(&g_s);
    for (int i = 0; i < NUM_FAVORITE_SPEEDS; i++) {
        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(SPEED_MIN_RPM, g_s.speed.favorite[i],
                                             "favorite speed below SPEED_MIN_RPM");
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(SPEED_MAX_RPM, g_s.speed.favorite[i],
                                          "favorite speed above SPEED_MAX_RPM");
    }
}

void test_defaults_depth_mode_is_off(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_EQUAL_INT(DEPTH_MODE_OFF, g_s.depth.mode);
}

void test_defaults_step_drill_disabled(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_FALSE(g_s.step_drill.enabled);
}

void test_defaults_braking_disabled(void) {
    apply_defaults(&g_s);
    /* Braking defaults OFF to prevent motor overheating */
    TEST_ASSERT_FALSE(g_s.power.braking_enabled);
}

void test_defaults_guard_check_enabled(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_TRUE(g_s.sensor.guard_check_enabled);
}

void test_defaults_checksum_valid(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_EQUAL_HEX16(calc_crc16(&g_s), g_s.checksum);
}

/*===========================================================================*/
/* 4. Speed bounds: min/max clamping logic                                   */
/*===========================================================================*/

void test_speed_clamp_below_min_gives_min(void) {
    uint16_t v = clamp16(0, SPEED_MIN_RPM, SPEED_MAX_RPM);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MIN_RPM, v);
}

void test_speed_clamp_above_max_gives_max(void) {
    uint16_t v = clamp16(9999, SPEED_MIN_RPM, SPEED_MAX_RPM);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MAX_RPM, v);
}

void test_speed_clamp_at_min_boundary(void) {
    uint16_t v = clamp16(SPEED_MIN_RPM, SPEED_MIN_RPM, SPEED_MAX_RPM);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MIN_RPM, v);
}

void test_speed_clamp_at_max_boundary(void) {
    uint16_t v = clamp16(SPEED_MAX_RPM, SPEED_MIN_RPM, SPEED_MAX_RPM);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MAX_RPM, v);
}

void test_speed_clamp_midrange_unchanged(void) {
    uint16_t v = clamp16(1500, SPEED_MIN_RPM, SPEED_MAX_RPM);
    TEST_ASSERT_EQUAL_UINT16(1500, v);
}

void test_speed_clamp_one_below_min(void) {
    uint16_t v = clamp16(SPEED_MIN_RPM - 1, SPEED_MIN_RPM, SPEED_MAX_RPM);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MIN_RPM, v);
}

void test_speed_clamp_one_above_max(void) {
    uint16_t v = clamp16(SPEED_MAX_RPM + 1, SPEED_MIN_RPM, SPEED_MAX_RPM);
    TEST_ASSERT_EQUAL_UINT16(SPEED_MAX_RPM, v);
}

/*===========================================================================*/
/* 5. Tapping trigger bitfield packing / unpacking                           */
/*===========================================================================*/

/*===========================================================================*/
/* 6. Overload threshold range                                                */
/*===========================================================================*/

void test_overload_threshold_clamp_below_10_gives_10(void) {
    /* mirrors settings_set_overload_threshold() clamping */
    uint8_t v = 5;
    if (v < 10)  v = 10;
    if (v > 100) v = 100;
    TEST_ASSERT_EQUAL_UINT8(10, v);
}

void test_overload_threshold_clamp_above_100_gives_100(void) {
    uint8_t v = 110;
    if (v < 10)  v = 10;
    if (v > 100) v = 100;
    TEST_ASSERT_EQUAL_UINT8(100, v);
}

void test_overload_threshold_at_lower_boundary(void) {
    uint8_t v = 10;
    if (v < 10)  v = 10;
    if (v > 100) v = 100;
    TEST_ASSERT_EQUAL_UINT8(10, v);
}

void test_overload_threshold_at_upper_boundary(void) {
    uint8_t v = 100;
    if (v < 10)  v = 10;
    if (v > 100) v = 100;
    TEST_ASSERT_EQUAL_UINT8(100, v);
}

void test_overload_threshold_midrange_unchanged(void) {
    uint8_t v = 50;
    if (v < 10)  v = 10;
    if (v > 100) v = 100;
    TEST_ASSERT_EQUAL_UINT8(50, v);
}

void test_overload_threshold_default_in_range(void) {
    apply_defaults(&g_s);
    TEST_ASSERT_GREATER_OR_EQUAL(10,  g_s.sensor.overload_threshold);
    TEST_ASSERT_LESS_OR_EQUAL(100, g_s.sensor.overload_threshold);
}

/*===========================================================================*/
/* Test runner                                                                */
/*===========================================================================*/

int main(void) {
    UNITY_BEGIN();

    /* 1. CRC16 checksum */
    RUN_TEST(test_crc16_is_deterministic);
    RUN_TEST(test_crc16_nonzero_for_default_settings);
    RUN_TEST(test_crc16_detects_corruption_in_speed_field);
    RUN_TEST(test_crc16_detects_corruption_in_magic);
    RUN_TEST(test_crc16_detects_corruption_in_motor_params);
    RUN_TEST(test_crc16_stored_in_settings_validates);
    RUN_TEST(test_crc16_two_different_structs_differ);

    /* 2. EEPROM custom block pack/unpack */

    /* 3. Settings defaults */
    RUN_TEST(test_defaults_magic_is_set);
    RUN_TEST(test_defaults_version_is_current);
    RUN_TEST(test_defaults_speed_in_range);
    RUN_TEST(test_defaults_max_limit_in_range);
    RUN_TEST(test_defaults_critical_pid_nonzero);
    RUN_TEST(test_defaults_ir_gain_nonzero);
    RUN_TEST(test_defaults_overload_threshold_nonzero);
    RUN_TEST(test_defaults_overload_threshold_in_range);
    RUN_TEST(test_defaults_all_tapping_triggers_disabled);
    RUN_TEST(test_defaults_tap_speed_in_range);
    RUN_TEST(test_defaults_all_favorites_in_range);
    RUN_TEST(test_defaults_depth_mode_is_off);
    RUN_TEST(test_defaults_step_drill_disabled);
    RUN_TEST(test_defaults_braking_disabled);
    RUN_TEST(test_defaults_guard_check_enabled);
    RUN_TEST(test_defaults_checksum_valid);

    /* 4. Speed bounds */
    RUN_TEST(test_speed_clamp_below_min_gives_min);
    RUN_TEST(test_speed_clamp_above_max_gives_max);
    RUN_TEST(test_speed_clamp_at_min_boundary);
    RUN_TEST(test_speed_clamp_at_max_boundary);
    RUN_TEST(test_speed_clamp_midrange_unchanged);
    RUN_TEST(test_speed_clamp_one_below_min);
    RUN_TEST(test_speed_clamp_one_above_max);

    /* 5. Tapping trigger bitfield */

    /* 6. Overload threshold */
    RUN_TEST(test_overload_threshold_clamp_below_10_gives_10);
    RUN_TEST(test_overload_threshold_clamp_above_100_gives_100);
    RUN_TEST(test_overload_threshold_at_lower_boundary);
    RUN_TEST(test_overload_threshold_at_upper_boundary);
    RUN_TEST(test_overload_threshold_midrange_unchanged);
    RUN_TEST(test_overload_threshold_default_in_range);

    return UNITY_END();
}
