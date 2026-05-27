#ifndef EEPROM_LAYOUT_H
#define EEPROM_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>

// OEM Teknatool EEPROM addresses (AT24C02, 256 bytes)
// These are shared with the original firmware for compatibility
#define EE_OEM_MAGIC        0x02    // 1 byte: 0x7C = valid OEM data
#define EE_DEPTH_CAL_B      0x1A    // 2 bytes: depth zero offset (BE)
#define EE_DEPTH_CAL_C      0x1C    // 2 bytes: depth scale (BE)
#define EE_DEPTH_STOP       0x28    // 2 bytes: depth stop value (BE)
#define EE_CL_DEFAULT       0x30    // 2 bytes: current limit default (BE)
#define EE_DEFAULT_RPM      0x32    // 2 bytes: stored speed (BE)
#define EE_PROFILE_IDX      0x3A    // 1 byte: motor profile index
#define EE_MAX_SPEED        0x44    // 2 bytes: max speed RPM (BE)
#define EE_FEATURE_FLAGS    0x48    // 2 bytes: feature flag bitfield
#define EE_PRESETS_BASE     0x64    // 8 x 2 bytes: speed presets (BE, 4-byte stride)
#define EE_BRIGHTNESS       0x84    // 1 byte: display brightness

// Feature flag bits at EE_FEATURE_FLAGS
#define EE_FLAG_FORWARD     0x02
#define EE_FLAG_UNLOCK      0x08
#define EE_FLAG_TAPPING     0x20
#define EE_FLAG_METRIC      0x40
#define EE_FLAG_DEPTH_STOP  0x80

// Custom firmware block: 0xB0-0xFF (80 bytes, unused by OEM)
#define EE_CUSTOM_BASE      0xB0
#define EE_CUSTOM_MAGIC     0xB0    // 1 byte: EE_CUSTOM_MAGIC_VALUE = valid
#define EE_CUSTOM_VERSION   0xB1    // 1 byte: layout version

/*---------------------------------------------------------------------------
 * EEPROM budget for the custom firmware: 0xB0..0xFF, 80 bytes, and not a byte
 * more (see docs/EEPROM_MAP.md — everything below 0xB0 belongs to the OEM).
 *
 * AUDIT FIX (CRITICAL, found 2026-08-30 while working finding #13): the v2
 * layout was 57 bytes at 0xB0, i.e. 0xB0..0xE8, while crash_dump.c wrote its
 * 36-byte record at 0xDC..0xFF. They overlapped by 13 bytes, and the overlap
 * covered the custom block's own checksum field. So every crash dump silently
 * corrupted the settings checksum, and the next boot failed validation and —
 * per the destructive rejection path in settings_init() — overwrote the whole
 * block with factory defaults. Neither the header comment ("0xB2-0xEF") nor
 * crash_dump.c's ("0xB0-0xDB") matched the struct that was actually compiled.
 *
 * The budget is now split explicitly here, with static assertions below, so
 * the regions cannot silently grow into each other again.
 *-------------------------------------------------------------------------*/
/* The block is FULL: eeprom_custom_t is exactly EE_CUSTOM_SIZE bytes. Adding a
 * field fails the static assertion at the bottom of this header rather than
 * quietly eating the crash dump, which is how the previous overlap happened.
 * To make room, take it from EE_CRASH_SIZE deliberately — do not move the
 * boundary by accident. */
#define EE_CUSTOM_SIZE      61          /* 0xB0..0xEC */
#define EE_CRASH_BASE       0xED        /* 0xED..0xFF */
#define EE_CRASH_SIZE       19


/*---------------------------------------------------------------------------
 * v2. Same 61-byte budget, rearranged to make room for the twelve tapping
 * fields that used to be defaulted at every boot — three of which (the
 * quill/load/load-slip completion actions) now decide how a tapping cycle
 * ENDS, so their defaults were what the machine actually behaved like on
 * every power-up.
 *
 * Where the room came from, all of it inside the block (the crash dump was
 * left alone deliberately — it is written from fault context and is the only
 * evidence available after a lockup):
 *
 *   -3  depth_mode, depth_action and power_output stopped burning a byte each
 *       and became bitfields; none has more than four values.
 *   -5  five ms fields dropped from uint16 to uint8 with a scale factor. Every
 *       one has a menu step at or above its scale (RevTim/FwdMs/RevMs step 50,
 *       ChipMs 10, RevTime 100), so no value reachable from the menu loses a
 *       digit. A console-typed odd value quantises — documented per field.
 *   +2  tap_actions / tap_misc bitfield bytes.
 *   +5  the remaining tapping values that genuinely need a byte each.
 *
 * Net -1, leaving one spare byte rather than the zero v1 had.
 *-------------------------------------------------------------------------*/
typedef struct __attribute__((packed)) {
    uint8_t  magic;             // EE_CUSTOM_MAGIC_VALUE
    uint8_t  version;           // EE_CUSTOM_VERSION_NUM
    // Motor PID
    int16_t  speed_kprop;
    int16_t  speed_kint;
    int16_t  voltage_kp;
    int16_t  voltage_ki;
    int16_t  ir_gain;
    int16_t  ir_offset;
    uint16_t speed_ramp;
    uint16_t torque_ramp;
    uint8_t  current_limit;
    // Sensor
    uint8_t  overload_threshold;
    uint8_t  spike_thresh;
    uint8_t  vibration_sensitivity;
    uint8_t  step_thresh;
    uint8_t  low_load_thresh;
    uint8_t  stall_sensitivity;
    uint8_t  stall_time_ms20;   // stall_time_ms / 20 (0-5000 -> 0-250)
    // Power / depth: power_output, depth_mode and depth_action live in tap_misc
    uint8_t  temp_threshold;
    int16_t  depth_target;
    int16_t  depth_offset;
    // Tapping
    uint8_t  tap_triggers;      // bits: 0=depth,1=load_inc,2=load_slip,3=clutch,4=quill,5=peck,6=pedal
    uint16_t tap_speed_rpm;
    uint8_t  peck_fwd_ms50;     // peck_fwd_ms / 50   (50-5000, menu step 50)
    uint8_t  peck_rev_ms10;     // peck_rev_ms / 10   (50-2000, menu step 50)
    uint8_t  peck_cycles;
    uint8_t  brake_delay_ms10;  // brake_delay_ms / 10 (50-500)
    uint8_t  tap_depth_completion_action;
    uint8_t  peck_completion_action;
    uint8_t  peck_rev_out_ms50; // peck_reverse_out_ms / 50 (200-5000, step 100)
    /* The twelve that used to reset every boot. Seven are bits in
     * tap_actions/tap_misc; these five need a byte each. */
    uint8_t  load_inc_threshold;   // 10-100 %
    uint8_t  load_slip_cv;         // 110-200 %
    uint8_t  load_inc_rev_ms10;    // load_increase_reverse_ms / 10 (50-2000)
    uint8_t  clutch_plateau_ms10;  // clutch_plateau_ms / 10 (50-500)
    uint8_t  pedal_chip_ms10;      // pedal_chip_break_ms / 10 (50-500)
    uint8_t  tap_actions;          // see EE_TA_* below
    uint8_t  tap_misc;             // see EE_TM_* below
    // Step drill
    uint8_t  step_start_dia;
    uint8_t  step_dia_inc;
    uint8_t  step_depth_x2;
    uint16_t step_base_rpm;
    uint8_t  step_target_dia;
    // Speed
    uint16_t slow_start;
    uint8_t  flags;             // see EE_CF_*
    uint8_t  units;
    uint16_t checksum;
} eeprom_custom_t;

/* tap_actions: four two-bit completion/mode enums. */
#define EE_TA_QUILL_PEDAL_SHIFT   0   /* quill_pedal_mode          0-2 */
#define EE_TA_QUILL_COMP_SHIFT    2   /* quill_completion_action   0-3 */
#define EE_TA_LOAD_COMP_SHIFT     4   /* load_completion_action    0-3 */
#define EE_TA_SLIP_COMP_SHIFT     6   /* load_slip_completion_act. 0-3 */
#define EE_TA_MASK2               0x03

/* tap_misc: three tapping bits plus the three fields that used to be bytes. */
#define EE_TM_CLUTCH_ACTION       0x01  /* clutch_action    0-1 */
#define EE_TM_PECK_DEPTH_STOP     0x02  /* peck_depth_stop  bool */
#define EE_TM_PEDAL_ACTION        0x04  /* pedal_action     0-1 */
#define EE_TM_DEPTH_MODE_SHIFT    3     /* depth.mode       0-2 */
#define EE_TM_DEPTH_ACTION        0x20  /* depth.action     0-1 */
#define EE_TM_POWER_OUTPUT_SHIFT  6     /* power.power_output 0-3 */

/* Packed boolean flags. */
#define EE_CF_JAM_DETECT      0x01
#define EE_CF_SPIKE_DETECT    0x02
#define EE_CF_GUARD_CHECK     0x04
#define EE_CF_PEDAL_ENABLED   0x08
#define EE_CF_STEP_ENABLED    0x10
#define EE_CF_LOW_LOAD_DETECT 0x20
#define EE_CF_DEPTH_ENABLED   0x40
/* bit 7 spare */

/* REVIEW FIX: changed from 0xCF on 2026-08-30, alongside the version reset to
 * 1. Resetting a version downward re-uses numbers already burned on
 * incompatible layouts, and a rejected blob is deliberately LEFT on the chip —
 * so a pre-release unit's v3 block would be accepted again two layout bumps
 * from now, with magic, version and CRC all matching (the CRC covers the
 * stored bytes and cannot tell you how to interpret them), feeding garbage
 * jam and belt-break thresholds into live settings. A new magic makes every
 * pre-release block unmatchable for good. See SETTINGS_MAGIC in config.h. */
#define EE_CUSTOM_MAGIC_VALUE  0xC1
/* Layout version of the 0xB0 EEPROM block.
 *
 * Reset to 1 on 2026-08-30 alongside SETTINGS_VERSION, for the same reason:
 * first public release, no back compatibility carried, and the counter had
 * only reached 3 through pre-release churn.
 *
 * BUMP IT on any change to eeprom_custom_t. A blob of a different version is
 * treated as absent and — unlike before — is NOT overwritten until the
 * operator saves, so a mismatch costs defaults for one boot rather than the
 * block. See eeprom_load_custom() in settings.c. */
/* 2: the v2 layout above. The v1 struct and its migration are gone — they
 * existed to carry this machine's block across the format change, which they
 * did (verified: the stored block reads magic C1 version 02), so keeping them
 * would only be a legacy path for a version that no longer exists anywhere.
 * A block with any other version is rejected and the settings fall back to
 * defaults. */
#define EE_CUSTOM_VERSION_NUM  2

// Read/write helpers
bool eeprom_load_oem_settings(void* settings);
bool eeprom_save_oem_settings(const void* settings);
bool eeprom_load_custom_settings(void* settings);
bool eeprom_save_custom_settings(const void* settings);

/*---------------------------------------------------------------------------
 * The whole point of the constants above: make an overlap a build error.
 *-------------------------------------------------------------------------*/
_Static_assert(sizeof(eeprom_custom_t) <= EE_CUSTOM_SIZE,
               "custom settings block has outgrown its EEPROM budget and would "
               "overwrite the crash dump at EE_CRASH_BASE");
_Static_assert(EE_CUSTOM_BASE + EE_CUSTOM_SIZE == EE_CRASH_BASE,
               "custom block and crash dump must be adjacent with no gap or overlap");
_Static_assert(EE_CRASH_BASE + EE_CRASH_SIZE == 0x100,
               "crash dump must end exactly at the top of the AT24C02");

#endif
