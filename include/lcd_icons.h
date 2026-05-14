/**
 * @file lcd_icons.h
 * @brief CGRAM Custom Character Icon Library for Nova Voyager
 *
 * Defines 8 custom 5×8 pixel icons for use with CGRAM.
 * Based on analysis of original firmware - uses text mode + custom characters
 * rather than pixel-level graphics.
 *
 * Icons are designed to enhance the UI with visual indicators:
 * - Status indicators (running, ready, error)
 * - Direction arrows (up/down for FWD/REV)
 * - Mode indicators (service, tapping)
 * - Progress/activity indicators
 */

#ifndef LCD_ICONS_H
#define LCD_ICONS_H

#include <stdint.h>

/*===========================================================================*/
/* Icon Patterns (8 bytes each, 5×8 pixels)                                  */
/*===========================================================================*/

/**
 * Icon 0: Arrow Right (→)
 * Use for: Forward direction, running status, next item
 */
static const uint8_t ICON_ARROW_RIGHT[8] = {
    0b00000,  // ·····
    0b00100,  // ··█··
    0b00010,  // ···█·
    0b11111,  // █████
    0b00010,  // ···█·
    0b00100,  // ··█··
    0b00000,  // ·····
    0b00000   // ·····
};

/**
 * Icon 1: Check Mark (✓)
 * Use for: Ready status, operation complete, confirmation
 */
static const uint8_t ICON_CHECK[8] = {
    0b00000,  // ·····
    0b00001,  // ····█
    0b00011,  // ···██
    0b10110,  // █·██·
    0b11100,  // ███··
    0b01000,  // ·█···
    0b00000,  // ·····
    0b00000   // ·····
};

/**
 * Icon 2: Warning/Alert (!)
 * Use for: Warnings, errors, attention needed
 */
static const uint8_t ICON_WARNING[8] = {
    0b00100,  // ··█··
    0b01110,  // ·███·
    0b01010,  // ·█·█·
    0b01010,  // ·█·█·
    0b00100,  // ··█··
    0b00000,  // ·····
    0b00100,  // ··█··
    0b00000   // ·····
};

/**
 * Icon 3: Wrench/Tool (🔧)
 * Use for: Service mode, settings, configuration
 */
static const uint8_t ICON_WRENCH[8] = {
    0b00010,  // ···█·
    0b00111,  // ··███
    0b00110,  // ··██·
    0b01110,  // ·███·
    0b11100,  // ███··
    0b11000,  // ██···
    0b10000,  // █····
    0b00000   // ·····
};

/**
 * Icon 4: Arrow Up (↑)
 * Use for: Forward direction, increase, ascending
 */
static const uint8_t ICON_ARROW_UP[8] = {
    0b00100,  // ··█··
    0b01110,  // ·███·
    0b10101,  // █·█·█
    0b00100,  // ··█··
    0b00100,  // ··█··
    0b00100,  // ··█··
    0b00100,  // ··█··
    0b00000   // ·····
};

/**
 * Icon 5: Arrow Down (↓)
 * Use for: Reverse direction, decrease, descending
 */
static const uint8_t ICON_ARROW_DOWN[8] = {
    0b00100,  // ··█··
    0b00100,  // ··█··
    0b00100,  // ··█··
    0b00100,  // ··█··
    0b10101,  // █·█·█
    0b01110,  // ·███·
    0b00100,  // ··█··
    0b00000   // ·····
};

/**
 * Icon 6: Spinner Frame 1 (rotating activity indicator)
 * Use for: Running status, processing, activity
 * Animate by cycling through frames
 */
static const uint8_t ICON_SPINNER_1[8] = {
    0b00100,  // ··█··
    0b01000,  // ·█···
    0b10000,  // █····
    0b00000,  // ·····
    0b00000,  // ·····
    0b00001,  // ····█
    0b00010,  // ···█·
    0b00100   // ··█··
};

/**
 * Icon 7: Degree Symbol (°)
 * Use for: Temperature display, angular measurements
 */
static const uint8_t ICON_DEGREE[8] = {
    0b01110,  // ·███·
    0b01010,  // ·█·█·
    0b01110,  // ·███·
    0b00000,  // ·····
    0b00000,  // ·····
    0b00000,  // ·····
    0b00000,  // ·····
    0b00000   // ·····
};

/*===========================================================================*/
/* Icon Character Codes (0-7)                                                */
/*===========================================================================*/

#define ICON_CODE_ARROW_RIGHT   0
#define ICON_CODE_CHECK         1
#define ICON_CODE_WARNING       2
#define ICON_CODE_WRENCH        3
#define ICON_CODE_ARROW_UP      4
#define ICON_CODE_ARROW_DOWN    5
#define ICON_CODE_SPINNER       6
#define ICON_CODE_DEGREE        7

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

/**
 * @brief Initialize all 8 custom icons in CGRAM
 *
 * Loads all icon patterns into CGRAM locations 0-7.
 * Call once during initialization, after lcd_init().
 * Icons can then be displayed using lcd_data(ICON_CODE_xxx).
 */
static inline void lcd_init_icons(void) {
    extern void lcd_create_char(uint8_t location, const uint8_t* data);

    lcd_create_char(ICON_CODE_ARROW_RIGHT, ICON_ARROW_RIGHT);
    lcd_create_char(ICON_CODE_CHECK, ICON_CHECK);
    lcd_create_char(ICON_CODE_WARNING, ICON_WARNING);
    lcd_create_char(ICON_CODE_WRENCH, ICON_WRENCH);
    lcd_create_char(ICON_CODE_ARROW_UP, ICON_ARROW_UP);
    lcd_create_char(ICON_CODE_ARROW_DOWN, ICON_ARROW_DOWN);
    lcd_create_char(ICON_CODE_SPINNER, ICON_SPINNER_1);
    lcd_create_char(ICON_CODE_DEGREE, ICON_DEGREE);
}

/**
 * @brief Display icon at current cursor position
 * @param icon_code Icon code (0-7) or use ICON_CODE_xxx constants
 *
 * Example:
 *   lcd_set_cursor(0, 0);
 *   lcd_put_icon(ICON_CODE_CHECK);
 *   lcd_print(" Ready");
 */
static inline void lcd_put_icon(uint8_t icon_code) {
    extern void lcd_data(uint8_t data);
    lcd_data(icon_code & 0x07);
}

/*===========================================================================*/
/* Alternative Spinner Frames (for animation)                                */
/*===========================================================================*/

// Additional spinner frames for smooth rotation animation
// Use lcd_create_char(ICON_CODE_SPINNER, ICON_SPINNER_x) to update

static const uint8_t ICON_SPINNER_2[8] = {
    0b00010,  // ···█·
    0b00100,  // ··█··
    0b00100,  // ··█··
    0b01000,  // ·█···
    0b10000,  // █····
    0b00000,  // ·····
    0b00000,  // ·····
    0b00001   // ····█
};

static const uint8_t ICON_SPINNER_3[8] = {
    0b00001,  // ····█
    0b00010,  // ···█·
    0b00100,  // ··█··
    0b00100,  // ··█··
    0b01000,  // ·█···
    0b10000,  // █····
    0b00000,  // ·····
    0b00000   // ·····
};

static const uint8_t ICON_SPINNER_4[8] = {
    0b00000,  // ·····
    0b00001,  // ····█
    0b00010,  // ···█·
    0b00100,  // ··█··
    0b00100,  // ··█··
    0b01000,  // ·█···
    0b10000,  // █····
    0b00000   // ·····
};

#endif /* LCD_ICONS_H */
