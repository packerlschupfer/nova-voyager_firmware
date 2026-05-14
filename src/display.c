/**
 * @file display.c
 * @brief Display Formatting and Status Screen
 *
 * High-level display functions for the Nova Voyager's 16x4 LCD.
 */

#include "display.h"
#include "lcd.h"
#include "shared.h"
#include "settings.h"
#include "config.h"

/*===========================================================================*/
/* Helper Functions                                                           */
/*===========================================================================*/

void display_write_num(uint16_t val, uint8_t width) {
    char buf[8];
    int i = width - 1;
    buf[width] = '\0';

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

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

void display_boot_message(const char* line1, const char* line2) {
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print(line1);
    lcd_set_cursor(1, 0);
    lcd_print(line2);
    lcd_delay_ms(300);
}

void display_update(void) {
    // Check for temporary error message
    STATE_LOCK();
    uint32_t error_until = g_state.error_until;
    const char* error_line1 = g_state.error_line1;
    const char* error_line2 = g_state.error_line2;
    STATE_UNLOCK();

    if (error_until > 0 && HAL_GetTick() < error_until) {
        // Show error message
        lcd_set_cursor(0, 0); lcd_print("                ");
        lcd_set_cursor(1, 0); lcd_print(error_line1 ? error_line1 : "");
        lcd_set_cursor(2, 0); lcd_print(error_line2 ? error_line2 : "");
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
    bool guard_closed = g_state.guard_closed;
    STATE_UNLOCK();

    // Check if step drill is enabled
    const settings_t* s = settings_get();
    bool step_drill_active = s->step_drill.enabled;

    // Handle error state with full-screen message
    if (state == APP_STATE_ERROR) {
        if (estop) {
            lcd_set_cursor(0, 0); lcd_print("!!!!!!!!!!!!!!!!");
            lcd_set_cursor(1, 0); lcd_print(" EMERGENCY STOP ");
            lcd_set_cursor(2, 0); lcd_print(" Press RESET btn");
            lcd_set_cursor(3, 0); lcd_print("!!!!!!!!!!!!!!!!");
        } else if (motor_fault) {
            lcd_set_cursor(0, 0); lcd_print("! MOTOR FAULT ! ");
            lcd_set_cursor(1, 0); lcd_print("Check motor     ");
            lcd_set_cursor(2, 0); lcd_print("connection      ");
            lcd_set_cursor(3, 0); lcd_print("                ");
        } else {
            lcd_set_cursor(0, 0); lcd_print("!! DRILL JAM !! ");
            lcd_set_cursor(1, 0); lcd_print("Release pressure");
            lcd_set_cursor(2, 0); lcd_print("Press ON to cont");
            lcd_set_cursor(3, 0); lcd_print("                ");
        }
        return;
    }

    // 16x4 LCD Layout:
    // Row 0: Actual RPM (8) | Target RPM (8)
    // Row 1: Load% State (8) | Tap Dir (8)
    // Row 2: Target (8) | Depth (8)
    // Row 3: F-key labels

    // Row 0: "XXXX        XXXX" (16 chars) - actual left, target right
    lcd_set_cursor(0, 0);
    // Show 0 RPM when motor stopped (IDLE state)
    if (state == APP_STATE_IDLE || !motor_running) {
        display_write_num(0, 4);  // Show 0 when stopped
    } else {
        display_write_num(actual_rpm, 4);  // Show actual when running
    }
    lcd_print("        ");
    display_write_num(target_rpm, 4);

    // Row 1: "XX% SSSS TTT DDDD"
    lcd_set_cursor(1, 0);
    display_write_num(load, 2);
    lcd_print("% ");
    // Show STEP when step drill mode is active and drilling
    if (step_drill_active && state == APP_STATE_DRILLING) {
        lcd_print("STEP");
    } else {
        switch (state) {
            case APP_STATE_IDLE:     lcd_print("IDL "); break;
            case APP_STATE_DRILLING: lcd_print("DRL "); break;
            case APP_STATE_TAPPING:  lcd_print("TAP "); break;
            case APP_STATE_MENU:     lcd_print("MENU"); break;
            default:                 lcd_print("    "); break;
        }
    }
    // Show active triggers (NEW: combinable trigger display)
    const settings_t* tap_settings = settings_get();
    char triggers[8] = "---";
    int idx = 0;

    // Build trigger abbreviation string (D-I-S-C-Q-K-P)
    if (tap_settings->tapping.depth_trigger_enabled) triggers[idx++] = 'D';
    if (tap_settings->tapping.load_increase_enabled) triggers[idx++] = 'I';
    if (tap_settings->tapping.load_slip_enabled) triggers[idx++] = 'S';
    if (tap_settings->tapping.clutch_slip_enabled) triggers[idx++] = 'C';
    if (tap_settings->tapping.quill_trigger_enabled) triggers[idx++] = 'Q';
    if (tap_settings->tapping.peck_trigger_enabled) triggers[idx++] = 'K';
    if (tap_settings->tapping.pedal_enabled) triggers[idx++] = 'P';
    triggers[idx] = '\0';

    // Show triggers if any enabled
    if (idx > 0) {
        lcd_print(triggers);
        lcd_print(" ");  // Pad to 4 chars
    } else {
        lcd_print("--- ");  // No triggers enabled
    }
    if (!motor_running) {
        lcd_print(" -- ");
    } else if (motor_forward) {
        lcd_print(" FWD");
    } else {
        lcd_print(" REV");
    }

    // Row 2: "T:XXXXXXD:XXXXXX" (16 chars) - target left, depth right
    lcd_set_cursor(2, 0);
    lcd_print("T:");
    if (depth_mode > 0) {
        display_write_depth(target_depth, 6);
    } else {
        lcd_print("      ");
    }
    lcd_print("D:");
    display_write_depth(depth, 6);

    // Row 3: F-key labels (16 chars)
    lcd_set_cursor(3, 0);
    lcd_print("SPD TAP DEP MOD ");
}
