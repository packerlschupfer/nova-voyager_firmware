/**
 * @file display.c
 * @brief Display Formatting and Status Screen
 *
 * High-level display functions for the Nova Voyager's 16x4 LCD.
 */

#include "display.h"
#include "lcd.h"
#include <string.h>
#include "shared.h"
#include "settings.h"
#include "config.h"
#include "events.h"
#include "temperature.h"
#include "motor_load.h"

#define ROW3_LOAD_BAR  0
#define ROW3_TEMP      1
#define ROW3_SPEED     2
#define ROW3_CLEAN     3
#define ROW3_MODE_COUNT 4

// F0 fault code decode strings (16 chars max for LCD row)
static const char* fault_decode(uint8_t code) {
    switch (code) {
        case 0:  return "Control Board   ";
        case 1:  return "SRM Not Rotate  ";
        case 2:  return "RPS Error 0     ";
        case 3:  return "RPS Error 1     ";
        case 4:  return "Hardware Fault  ";
        case 5:  return "Unexpected Err  ";
        case 13: return "Low Voltage     ";
        case 14: return "PFC Fault       ";
        case 15: return "(no fault)      ";
        case 50: return "Fault Code 50   ";
        case 55: return "EEPROM Fault    ";
        case 56: return "Fault Code 56   ";
        default: return "Unknown Fault   ";
    }
}

// ST7920 CGRAM split load bar — dynamic per-frame rebuild.
//
// Row 3 is 128 px wide × 16 px tall, drawn as 8 chars × (16×16 px CGRAM slots).
// The bar is split horizontally into two halves sharing the same 0-100% axis:
//   top 8 rows  = cutting effort = max(0, raw_load - learned_idle_baseline)
//   bot 8 rows  = total raw load
// At any frame, at most one char position is "transitional" per half. CGRAM
// is rebuilt only when one of those partial widths changes.
//
// Slot assignment (per frame):
//   slot 0  always: top full + bottom full           — chars fully under both bars
//   slot 1  always: top empty + bottom full          — chars between transitions
//   slot 2  when cut transitions:  top partial + bottom full
//   slot 3  when total transitions: top empty + bottom partial
//   slot 2 (special: when both transitions overlap): top partial + bottom partial
//
// Displayed via 2-byte DDRAM codes 0x00,0x00 / 0x00,0x02 / 0x00,0x04 / 0x00,0x06.
#define BAR_FULL_HI   0x00
#define BAR_FULL_LO   0x00   // slot 0
#define BAR_BOT_HI    0x00
#define BAR_BOT_LO    0x02   // slot 1
#define BAR_CUT_HI    0x00
#define BAR_CUT_LO    0x04   // slot 2
#define BAR_TOT_HI    0x00
#define BAR_TOT_LO    0x06   // slot 3
#define BAR_EMPTY_HI  ' '
#define BAR_EMPTY_LO  ' '

/*===========================================================================*/
/* Row Buffer — build row content then flush via dirty tracking              */
/*===========================================================================*/

static char row_buf[LCD_COLS];
static uint8_t row_pos;

static void buf_reset(void) {
    memset(row_buf, ' ', LCD_COLS);
    row_pos = 0;
}

static void buf_char(char c) {
    if (row_pos < LCD_COLS) row_buf[row_pos++] = c;
}

static void buf_str(const char* s) {
    while (*s && row_pos < LCD_COLS) row_buf[row_pos++] = *s++;
}

static void buf_pad(uint8_t target) {
    while (row_pos < target && row_pos < LCD_COLS) row_buf[row_pos++] = ' ';
}

/* AUDIT FIX (MEDIUM, display.c:104): a value too wide for the field used to be
 * TRUNCATED rather than clamped — the loop stopped when it ran out of columns
 * and dropped the most significant digits. At width 2, a motor load of 100 %
 * rendered as "00", i.e. the display showed no load at the exact moment the
 * load was highest. Clamping to the widest value the field can hold shows 99,
 * which is off by one but stays monotonic and never inverts the reading. */
static void buf_num(uint16_t val, uint8_t width) {
    char tmp[8];
    uint16_t limit = 0;
    for (uint8_t d = 0; d < width && d < 5; d++) {
        limit = (uint16_t)(limit * 10 + 9);
    }
    if (val > limit) val = limit;
    int i = width - 1;
    bool started = false;
    do {
        tmp[i] = '0' + (val % 10);
        val /= 10;
        if (tmp[i] != '0') started = true;
        i--;
    } while (val > 0 && i >= 0);
    while (i >= 0) tmp[i--] = ' ';
    if (!started && width > 0) tmp[width - 1] = '0';
    for (uint8_t j = 0; j < width && row_pos < LCD_COLS; j++)
        row_buf[row_pos++] = tmp[j];
}

static void buf_depth(int16_t depth_01mm, uint8_t width) {
    char tmp[8];
    bool negative = depth_01mm < 0;
    if (negative) depth_01mm = (depth_01mm == INT16_MIN) ? INT16_MAX : -depth_01mm;
    uint16_t mm = depth_01mm / 10;
    uint8_t frac = depth_01mm % 10;
    int pos = width - 1;
    tmp[pos--] = '0' + frac;
    tmp[pos--] = '.';
    do { tmp[pos--] = '0' + (mm % 10); mm /= 10; } while (mm > 0 && pos >= 0);
    if (negative && pos >= 0) tmp[pos--] = '-';
    while (pos >= 0) tmp[pos--] = ' ';
    for (uint8_t j = 0; j < width && row_pos < LCD_COLS; j++)
        row_buf[row_pos++] = tmp[j];
}

static void buf_flush(uint8_t row) {
    buf_pad(LCD_COLS);
    lcd_update_row(row, row_buf);
}

/*===========================================================================*/
/* Legacy Helper Functions (for non-buffered callers)                         */
/*===========================================================================*/

void display_write_num(uint16_t val, uint8_t width) {
    char buf[8];
    int i = width - 1;
    buf[width] = '\0';

    /* REVIEW FIX: this truncated to the low `width` digits instead of
     * saturating — the fix applied to buf_num() was never carried over. An MCB
     * fault code of 100 rendered as "F0=00", which fault_decode() then names as
     * a DIFFERENT fault: the operator reads a confident wrong diagnosis off the
     * error screen. Saturate like buf_num() does. */
    uint16_t limit = 0;
    for (uint8_t d = 0; d < width && d < 5; d++) {
        limit = (uint16_t)(limit * 10 + 9);
    }
    if (val > limit) val = limit;

    // Fill with digits from right
    bool started = false;
    do {
        buf[i] = '0' + (val % 10);
        val /= 10;
        if (buf[i] != '0') started = true;
        i--;
    } while (val > 0 && i >= 0);

    // Fill remaining with spaces
    while (i >= 0) {
        buf[i--] = ' ';
    }

    // Ensure at least "0" shows for zero value
    if (!started && width > 0) {
        buf[width - 1] = '0';
    }

    lcd_print(buf);
}

void display_write_depth(int16_t depth_01mm, uint8_t width) {
    char buf[8];
    bool negative = depth_01mm < 0;
    if (negative) depth_01mm = -depth_01mm;

    uint16_t mm = depth_01mm / 10;
    uint8_t frac = depth_01mm % 10;

    int pos = width - 1;
    buf[width] = '\0';

    // Fractional digit
    buf[pos--] = '0' + frac;
    buf[pos--] = '.';

    // Integer part
    do {
        buf[pos--] = '0' + (mm % 10);
        mm /= 10;
    } while (mm > 0 && pos >= 0);

    // Sign if negative
    if (negative && pos >= 0) {
        buf[pos--] = '-';
    }

    // Fill remaining with spaces
    while (pos >= 0) {
        buf[pos--] = ' ';
    }

    lcd_print(buf);
}

// Left-justified pixel-mask in an 8-bit byte. px=0→0x00, px=4→0xF0, px=8→0xFF.
static uint8_t bar_mask8(uint8_t px) {
    if (px == 0) return 0x00;
    if (px >= 8) return 0xFF;
    return (uint8_t)(0xFF << (8 - px));
}

// Write 32 bytes to one CGRAM slot. Top half (rows 0-7) shows `top_px` cols
// of left-justified fill; bottom half (rows 8-15) shows `bottom_px` cols.
// Both 0..16. Caller must already be in basic instruction set (lcd_cmd 0x30).
static void bar_build_slot(uint8_t slot_idx, uint8_t top_px, uint8_t bottom_px) {
    uint8_t top_hi = bar_mask8(top_px > 8 ? 8 : top_px);
    uint8_t top_lo = bar_mask8(top_px > 8 ? (uint8_t)(top_px - 8) : 0);
    uint8_t bot_hi = bar_mask8(bottom_px > 8 ? 8 : bottom_px);
    uint8_t bot_lo = bar_mask8(bottom_px > 8 ? (uint8_t)(bottom_px - 8) : 0);
    lcd_cmd((uint8_t)(0x40 | (slot_idx << 4)));
    for (int r = 0; r < 8; r++) { lcd_data(top_hi); lcd_data(top_lo); }
    for (int r = 0; r < 8; r++) { lcd_data(bot_hi); lcd_data(bot_lo); }
}

static void display_row3_load_bar(uint16_t load) {
    if (load > 100) load = 100;

    // Compute cutting effort = max(0, raw_load - baseline). When the baseline
    // hasn't been learned yet (just-started, mid-speed-change), show 0 for
    // cutting — the bar degenerates to bottom-only, signaling "still learning".
    uint8_t baseline_pct = 0;
    uint8_t cutting_pct = 0;
    if (motor_load_get_baseline(&baseline_pct) && (uint8_t)load > baseline_pct) {
        cutting_pct = (uint8_t)load - baseline_pct;
    }

    uint8_t total_px   = (uint16_t)load        * 128 / 100;
    uint8_t cutting_px = (uint16_t)cutting_pct * 128 / 100;
    if (cutting_px > total_px) cutting_px = total_px;

    // Transitional char index for each bar (-1 = no partial — falls on a boundary).
    int8_t  cut_trans = -1;
    uint8_t cut_partial = 0;
    if (cutting_px > 0 && cutting_px < 128 && (cutting_px & 0x0F) != 0) {
        cut_trans = (int8_t)(cutting_px >> 4);
        cut_partial = (uint8_t)(cutting_px & 0x0F);
    }
    int8_t  tot_trans = -1;
    uint8_t tot_partial = 0;
    if (total_px > 0 && total_px < 128 && (total_px & 0x0F) != 0) {
        tot_trans = (int8_t)(total_px >> 4);
        tot_partial = (uint8_t)(total_px & 0x0F);
    }

    // Skip CGRAM rewrite when the pixel geometry hasn't moved across a
    // sub-character boundary. Pixel-level cache, not percent-level.
    static uint8_t prev_cut_px = 0xFF;
    static uint8_t prev_tot_px = 0xFF;
    if (cutting_px != prev_cut_px || total_px != prev_tot_px) {
        lcd_cmd(0x30);
        lcd_delay_ms(1);

        bar_build_slot(0, 16, 16);                 // all full
        bar_build_slot(1,  0, 16);                 // bottom-only full
        if (cut_trans >= 0 && cut_trans == tot_trans) {
            // Transitions overlap: one char carries both partials.
            bar_build_slot(2, cut_partial, tot_partial);
        } else {
            if (cut_trans >= 0) bar_build_slot(2, cut_partial, 16);
            if (tot_trans >= 0) bar_build_slot(3, 0, tot_partial);
        }

        prev_cut_px = cutting_px;
        prev_tot_px = total_px;
        // CGRAM bytes changed but DDRAM codes may match the shadow — force
        // the next row write to push all bytes through, otherwise the dirty-
        // word check sees "no change" and the visible pixels stay stale.
        lcd_shadow_invalidate();
    }

    uint8_t bar_buf[LCD_COLS];
    for (int i = 0; i < 8; i++) {
        uint8_t hi = BAR_EMPTY_HI, lo = BAR_EMPTY_LO;
        uint16_t char_start = (uint16_t)(i * 16);
        uint16_t char_end   = (uint16_t)(char_start + 16);

        if (total_px <= char_start) {
            hi = BAR_EMPTY_HI; lo = BAR_EMPTY_LO;
        } else if (cutting_px >= char_end) {
            hi = BAR_FULL_HI;  lo = BAR_FULL_LO;     // slot 0
        } else if (i == cut_trans) {
            // Slot 2 is either (cut_partial, 16) or, when transitions overlap,
            // (cut_partial, tot_partial) — built above accordingly.
            hi = BAR_CUT_HI;   lo = BAR_CUT_LO;      // slot 2
        } else if (i == tot_trans) {
            hi = BAR_TOT_HI;   lo = BAR_TOT_LO;      // slot 3
        } else if (cutting_px <= char_start && total_px >= char_end) {
            hi = BAR_BOT_HI;   lo = BAR_BOT_LO;      // slot 1
        }

        bar_buf[i * 2]     = hi;
        bar_buf[i * 2 + 1] = lo;
    }
    lcd_update_row_2byte(3, bar_buf);
}

static void display_row3_temp(void) {
    uint16_t mcb = temp_get_mcb();
    uint16_t hmi = temperature_read_gd32();
    STATE_LOCK();
    uint16_t vbus = g_state.dc_bus_voltage;
    STATE_UNLOCK();

    buf_reset();
    /* REVIEW FIX: the field is two columns wide and buf_num() saturates, so a
     * heatsink at or above 100 C — well past the 80 C shutdown floor — read
     * "99C", a plausible number inside its own alarm band. Say HI instead. */
    if (mcb >= 100 && mcb < 150) {
        buf_str("HI");
    } else {
        buf_num(mcb > 0 && mcb < 150 ? mcb : 0, 2);
    }
    buf_char('C');  // plain C instead of 2-byte ℃ (keeps dirty tracking simple)
    buf_char(' ');
    buf_num(hmi, 2);
    buf_char('C');
    buf_char(' ');
    buf_num(vbus, 3);
    buf_str("V ");
    buf_flush(3);
}

static void display_row3_speed(void) {
    STATE_LOCK();
    bool fine = g_state.speed_fine_mode;
    uint16_t rpm = g_state.target_rpm;
    STATE_UNLOCK();

    uint16_t step = get_speed_step(rpm, !fine);
    buf_reset();
    buf_str(fine ? "Fine  " : "Coarse");
    buf_char(' ');
    buf_num(step, 4);
    buf_str("/clk ");
    buf_flush(3);
}

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

void display_init(void) {
    // Seed CGRAM with the split-bar default layout so the very first frame
    // already renders correctly. Subsequent frames rebuild on demand from
    // display_row3_load_bar() (no-op when the pixel geometry hasn't moved).
    lcd_cmd(0x30);
    lcd_delay_ms(2);
    bar_build_slot(0, 16, 16);  // all full
    bar_build_slot(1,  0, 16);  // bottom only
    bar_build_slot(2,  0, 16);  // start identical to slot 1 — overwritten on first transition
    bar_build_slot(3,  0, 16);  // ditto
    lcd_cmd(0x38);
    lcd_delay_ms(1);
    lcd_cmd(0x0C);
    lcd_delay_ms(1);
}

void display_cycle_row3(void) {
    STATE_LOCK();
    g_state.row3_mode = (g_state.row3_mode + 1) % ROW3_MODE_COUNT;
    STATE_UNLOCK();
}

void display_boot_message(const char* line1, const char* line2) {
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print(line1);
    lcd_set_cursor(1, 0);
    lcd_print(line2);
    lcd_delay_ms(300);
}

/* Print exactly LCD_COLS characters, space-padded. The error lines come from a
 * dozen producers of differing length and go out through lcd_print(), which
 * bypasses the dirty-row shadow — so a short line has to blank its own tail. */
static void lcd_print_padded(const char* text) {
    char row[LCD_COLS + 1];
    size_t i = 0;
    if (text) {
        while (i < LCD_COLS && text[i]) { row[i] = text[i]; i++; }
    }
    while (i < LCD_COLS) row[i++] = ' ';
    row[LCD_COLS] = '\0';
    lcd_print(row);
}

void display_update(void) {
    // Clock fault outranks every other screen. The crystal never started, we
    // are running on HSI, and nothing timing-dependent can be trusted —
    // including the UART link to the MCB. Level-driven like the guard screen,
    // but g_clock_fault is latched for the life of the boot, so this stays up
    // until the board is power-cycled onto a working crystal.
    if (g_clock_fault) {
        lcd_shadow_invalidate();
        lcd_set_cursor(0, 0); lcd_print("!! CLOCK FAULT !");
        lcd_set_cursor(1, 0); lcd_print("HSE crystal dead");
        lcd_set_cursor(2, 0); lcd_print("Motor disabled  ");
        lcd_set_cursor(3, 0); lcd_print("Service required");
        return;
    }

    // Check for temporary error message
    STATE_LOCK();
    uint32_t error_until = g_state.error_until;
    const char* error_line1 = g_state.error_line1;
    const char* error_line2 = g_state.error_line2;
    bool guard_closed_early = g_state.guard_closed;
    bool guard_check_enabled = true;
    STATE_UNLOCK();
    // Only render the guard warning if the guard-check feature is enabled.
    {
        const settings_t* s_gc = settings_get();
        if (s_gc) guard_check_enabled = s_gc->sensor.guard_check_enabled;
    }

    // BUGFIX 2026-07-14: guard-open indicator is level-driven, not
    // edge-timer-driven. Old behavior: handle_btn_guard set error_until
    // for 30 s, then the screen vanished even though the guard was still
    // open. Now the warning stays for as long as the guard reads open.
    /* REVIEW FIX (MEDIUM): this returned before the APP_STATE_ERROR/E-Stop
     * screen below, so with the E-Stop latched AND the guard open the operator
     * saw only "Close to operate" — closing the guard then changed nothing and
     * nothing on screen said why. Severity ordering has to run the other way:
     * an engaged E-Stop outranks an open guard. */
    STATE_LOCK();
    const bool estop_early = g_state.estop_active;
    STATE_UNLOCK();

    if (guard_check_enabled && !guard_closed_early && !estop_early) {
        lcd_shadow_invalidate();
        lcd_set_cursor(0, 0); lcd_print("!! GUARD OPEN !!");
        lcd_set_cursor(1, 0); lcd_print("                ");
        lcd_set_cursor(2, 0); lcd_print("Close to operate");
        lcd_set_cursor(3, 0); lcd_print("!!!!!!!!!!!!!!!!");
        return;
    }

    if (error_until > 0 && (error_until - HAL_GetTick()) <= ESTOP_DISPLAY_MS) {
        /* REVIEW FIX (MEDIUM): rows 0 and 3 were blanked but 1 and 2 were
         * printed unpadded, and lcd_print() bypasses the shadow so
         * lcd_shadow_invalidate() cannot clean up after it. Several producers
         * are short — " LOW VOLTAGE! " and "!! OVERHEAT !!" are 14, "Tap:OFF"
         * is 7 — so the tail of the previous screen stayed on the line: over a
         * normal row 2 of "T:  25.0D:   3.4" an overheat rendered as
         * "Temp: 85C:   3.4". Pad to the full 16 columns. */
        lcd_shadow_invalidate();
        lcd_set_cursor(0, 0); lcd_print("                ");
        lcd_set_cursor(1, 0); lcd_print_padded(error_line1);
        lcd_set_cursor(2, 0); lcd_print_padded(error_line2);
        lcd_set_cursor(3, 0); lcd_print("                ");
        return;
    }

    // Read shared state
    STATE_LOCK();
    app_state_t state = g_state.state;
    uint16_t target_rpm = g_state.target_rpm;
    uint16_t actual_rpm = g_state.current_rpm;
    uint16_t load = g_state.motor_load;
    int16_t depth = g_state.current_depth;
    int16_t target_depth = g_state.target_depth;
    bool motor_forward = g_state.motor_forward;
    bool motor_running = g_state.motor_running;
    bool motor_fault = g_state.motor_fault;
    bool estop = g_state.estop_active;
    uint8_t depth_mode = g_state.depth_mode;
    bool tap_armed = g_state.tapping_armed;
    uint8_t row3_mode = g_state.row3_mode;
    STATE_UNLOCK();

    // Check if step drill is enabled
    const settings_t* s = settings_get();
    bool step_drill_active = s->step_drill.enabled;

    if (state == APP_STATE_ERROR) {
        lcd_shadow_invalidate();
        if (estop) {
            // BUGFIX 2026-07-14: was "Press RESET btn" — wrong instruction for
            // a physical E-Stop button. Operator releases the mushroom, they
            // don't press a separate reset. This is the only E-Stop screen
            // now (transient error_until path disabled — see handle_btn_estop).
            lcd_set_cursor(0, 0); lcd_print("!!!!!!!!!!!!!!!!");
            lcd_set_cursor(1, 0); lcd_print(" EMERGENCY STOP ");
            lcd_set_cursor(2, 0); lcd_print(" Release E-Stop ");
            lcd_set_cursor(3, 0); lcd_print("!!!!!!!!!!!!!!!!");
        } else if (motor_fault) {
            STATE_LOCK();
            uint8_t fc = g_state.fault_code;
            STATE_UNLOCK();
            lcd_set_cursor(0, 0); lcd_print("! MOTOR FAULT ! ");
            lcd_set_cursor(1, 0); lcd_print("F0=");
            display_write_num(fc, 2);
            lcd_print("           ");
            lcd_set_cursor(2, 0); lcd_print(fault_decode(fc));
            lcd_set_cursor(3, 0); lcd_print("Press ON to clr ");
        } else {
            lcd_set_cursor(0, 0); lcd_print("!! DRILL JAM !! ");
            lcd_set_cursor(1, 0); lcd_print("Release pressure");
            lcd_set_cursor(2, 0); lcd_print("Press ON to cont");
            lcd_set_cursor(3, 0); lcd_print("                ");
        }
        return;
    }

    // Row 0: "XXXX        XXXX" — actual RPM left, target right
    buf_reset();
    if (state == APP_STATE_IDLE || !motor_running)
        buf_num(0, 4);
    else
        buf_num(actual_rpm, 4);
    buf_pad(12);
    buf_num(target_rpm, 4);
    buf_flush(0);

    // Row 1: "LL SSS TTTT  DIR" — load, state, triggers, direction
    buf_reset();
    buf_num(load, 2);
    buf_char(' ');
    /* The powered spindle hold is shown as a MODIFIER, not as an app state.
     *
     * It genuinely coexists with the app state rather than replacing one: a
     * safety hold runs while the machine is in ERROR with the E-Stop engaged or
     * the guard open, and those persistent screens are rendered earlier and
     * return before this row is composed — so there is nothing to arbitrate.
     * Making it an APP_STATE_* value would instead have put it in competition
     * with them for the same slot, and would have needed considering at each of
     * the ~67 places that compare app state, several of which gate motor
     * starts. A label is a label; it does not belong in the safety enum.
     *
     * It takes precedence over IDL because "held" is the more specific fact
     * about a stopped spindle, and the operator needs to know the windings are
     * energized. */
    extern bool motor_is_spindle_hold_active(void);   /* motor.h */
    if (motor_is_spindle_hold_active()) {
        buf_str("HLD");
    } else if (step_drill_active && state == APP_STATE_DRILLING) {
        buf_str("STP");
    } else {
        switch (state) {
            case APP_STATE_IDLE:     buf_str("IDL"); break;
            case APP_STATE_DRILLING: buf_str("DRL"); break;
            case APP_STATE_TAPPING:  buf_str("TAP"); break;
            case APP_STATE_MENU:     buf_str("MNU"); break;
            default:                 buf_str("   "); break;
        }
    }
    buf_char(' ');

    const settings_t* tap_settings = settings_get();
    int trig_start = row_pos;
    if (tap_settings->tapping.depth_trigger_enabled) buf_char('D');
    if (tap_settings->tapping.load_increase_enabled) buf_char('I');
    if (tap_settings->tapping.load_slip_enabled) buf_char('S');
    if (tap_settings->tapping.clutch_slip_enabled) buf_char('C');
    if (tap_settings->tapping.quill_trigger_enabled) buf_char('Q');
    if (tap_settings->tapping.peck_trigger_enabled) buf_char('K');
    if (tap_settings->tapping.pedal_enabled) buf_char('P');
    int trig_count = row_pos - trig_start;

    if (trig_count > 0 && !tap_armed) {
        row_pos = trig_start;
        buf_str("off");
    } else if (trig_count == 0) {
        buf_str("---");
    }

    /* REVIEW FIX (MEDIUM): the trigger letters start at column 7 and there are
     * seven of them, so a fully-configured tapping setup ran to column 14 and
     * the direction field below — which needs the last four columns — was
     * truncated to " F". The operator lost FWD/REV, which on a tapping machine
     * is the one thing on the row they cannot infer. Cap the list instead and
     * mark that it was cut. */
    if (row_pos > 12) {
        row_pos = 11;
        buf_char('+');
    }
    buf_pad(12);

    if (!motor_running)     buf_str(" -- ");
    else if (motor_forward) buf_str(" FWD");
    else                    buf_str(" REV");
    buf_flush(1);

    // Row 2: "T:XXXXXXD:XXXXXX" — target depth, current depth
    buf_reset();
    buf_str("T:");
    if (depth_mode > 0)
        buf_depth(target_depth, 6);
    else
        buf_str("      ");
    buf_str("D:");
    buf_depth(depth, 6);
    buf_flush(2);

    // Row 3: dynamic (F4 cycles mode)
    switch (row3_mode) {
        case ROW3_LOAD_BAR: display_row3_load_bar(load); break;
        case ROW3_TEMP:     display_row3_temp(); break;
        case ROW3_SPEED:    display_row3_speed(); break;
        default: {
            buf_reset();
            buf_flush(3);
            break;
        }
    }

    lcd_shadow_commit();
}
