# CGRAM Custom Characters - Implementation Guide

**Date:** 2026-01-30
**Status:** Ready to implement
**Priority:** HIGH
**Estimated effort:** 3-4 hours

---

## Discovery Summary

Binary analysis of original firmware (r2p06k) revealed that the "graphics" capability is **CGRAM custom characters**, not pixel-level graphics. This is a standard HD44780/ST7920 feature that allows defining 8 custom 5×8 character patterns.

**Key finding:** Original firmware uses text mode + custom characters, not ST7920 extended graphics mode.

---

## CGRAM Basics

### What is CGRAM?

**Character Generator RAM** - User-definable character patterns
- **8 custom characters** (codes 0-7)
- **5×8 pixel** resolution per character
- **8 bytes** per character definition
- **Standard HD44780** feature (all compatible controllers support it)

### How It Works

```
Character Code 0: Address 0x40-0x47 (8 bytes)
Character Code 1: Address 0x48-0x4F (8 bytes)
...
Character Code 7: Address 0x78-0x7F (8 bytes)
```

Each byte represents one row (5 bits used, MSB ignored):
```
Byte 0: xxxABCDE  (row 0)
Byte 1: xxxABCDE  (row 1)
...
Byte 7: xxxABCDE  (row 7)
```

---

## Implementation

### 1. Add CGRAM Functions to `lcd.c`

```c
/**
 * Define a custom character in CGRAM
 * @param code Character code (0-7)
 * @param pattern Pointer to 8-byte pattern (5 bits per row)
 */
void lcd_define_char(uint8_t code, const uint8_t *pattern) {
    if (code > 7) return;  // Only 8 custom characters

    // Set CGRAM address
    lcd_cmd(0x40 | (code << 3));

    // Write 8 bytes of pattern data
    for (int i = 0; i < 8; i++) {
        lcd_data(pattern[i] & 0x1F);  // Only use lower 5 bits
    }

    // Return to DDRAM (text) mode
    lcd_cmd(0x80);
}

/**
 * Display a custom character
 * @param code Character code (0-7)
 */
void lcd_putchar_custom(uint8_t code) {
    if (code > 7) return;
    lcd_data(code);
}
```

### 2. Create Icon Library (`include/lcd_icons.h`)

```c
#ifndef LCD_ICONS_H
#define LCD_ICONS_H

#include <stdint.h>

// Icon patterns (8 bytes each, 5×8 pixels)

// Arrow Right (→)
static const uint8_t ICON_ARROW_RIGHT[8] = {
    0b00000,
    0b00100,
    0b00010,
    0b11111,
    0b00010,
    0b00100,
    0b00000,
    0b00000
};

// Check Mark (✓)
static const uint8_t ICON_CHECK[8] = {
    0b00000,
    0b00001,
    0b00011,
    0b10110,
    0b11100,
    0b01000,
    0b00000,
    0b00000
};

// Warning (!)
static const uint8_t ICON_WARNING[8] = {
    0b00100,
    0b01110,
    0b01010,
    0b01010,
    0b00100,
    0b00000,
    0b00100,
    0b00000
};

// Wrench (🔧)
static const uint8_t ICON_WRENCH[8] = {
    0b00010,
    0b00111,
    0b00110,
    0b01110,
    0b11100,
    0b11000,
    0b10000,
    0b00000
};

// Up Arrow (↑)
static const uint8_t ICON_ARROW_UP[8] = {
    0b00100,
    0b01110,
    0b10101,
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b00000
};

// Down Arrow (↓)
static const uint8_t ICON_ARROW_DOWN[8] = {
    0b00100,
    0b00100,
    0b00100,
    0b00100,
    0b10101,
    0b01110,
    0b00100,
    0b00000
};

// Spinner / Running indicator (rotating bars)
static const uint8_t ICON_SPINNER_1[8] = {
    0b00100,
    0b01000,
    0b10000,
    0b00000,
    0b00000,
    0b00001,
    0b00010,
    0b00100
};

// Degree symbol (°)
static const uint8_t ICON_DEGREE[8] = {
    0b01110,
    0b01010,
    0b01110,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000
};

// Icon codes (character positions 0-7)
#define ICON_CODE_ARROW_RIGHT   0
#define ICON_CODE_CHECK         1
#define ICON_CODE_WARNING       2
#define ICON_CODE_WRENCH        3
#define ICON_CODE_ARROW_UP      4
#define ICON_CODE_ARROW_DOWN    5
#define ICON_CODE_SPINNER       6
#define ICON_CODE_DEGREE        7

// Initialize all icons
static inline void lcd_init_icons(void) {
    extern void lcd_define_char(uint8_t code, const uint8_t *pattern);

    lcd_define_char(ICON_CODE_ARROW_RIGHT, ICON_ARROW_RIGHT);
    lcd_define_char(ICON_CODE_CHECK, ICON_CHECK);
    lcd_define_char(ICON_CODE_WARNING, ICON_WARNING);
    lcd_define_char(ICON_CODE_WRENCH, ICON_WRENCH);
    lcd_define_char(ICON_CODE_ARROW_UP, ICON_ARROW_UP);
    lcd_define_char(ICON_CODE_ARROW_DOWN, ICON_ARROW_DOWN);
    lcd_define_char(ICON_CODE_SPINNER, ICON_SPINNER_1);
    lcd_define_char(ICON_CODE_DEGREE, ICON_DEGREE);
}

#endif // LCD_ICONS_H
```

### 3. Update `lcd.h`

```c
// Add to include/lcd.h

/**
 * Define custom character in CGRAM
 * @param code Character code (0-7)
 * @param pattern 8-byte pattern array
 */
void lcd_define_char(uint8_t code, const uint8_t *pattern);

/**
 * Display custom character
 * @param code Character code (0-7)
 */
void lcd_putchar_custom(uint8_t code);
```

### 4. Initialize Icons at Startup

In `src/main.c` or `src/lcd.c` initialization:

```c
#include "lcd_icons.h"

// After lcd_init():
lcd_init_icons();
```

### 5. Use Icons in UI

In `src/task_ui.c`:

```c
#include "lcd_icons.h"

// Example: Show status with icon
void ui_show_status(void) {
    lcd_set_cursor(0, 0);
    lcd_puts("Status:");

    if (motor_running) {
        lcd_putchar_custom(ICON_CODE_ARROW_RIGHT);
        lcd_puts(" Running");
    } else {
        lcd_putchar_custom(ICON_CODE_CHECK);
        lcd_puts(" Ready  ");
    }
}

// Example: Show direction
void ui_show_direction(bool forward) {
    if (forward) {
        lcd_putchar_custom(ICON_CODE_ARROW_UP);
    } else {
        lcd_putchar_custom(ICON_CODE_ARROW_DOWN);
    }
}

// Example: Service mode indicator
void ui_service_mode(void) {
    lcd_putchar_custom(ICON_CODE_WRENCH);
    lcd_puts(" Service Mode");
}
```

---

## Advanced Features

### Progress Bar Using Custom Characters

Define 5 custom characters for different fill levels:

```c
// Empty block
static const uint8_t BAR_EMPTY[8] = {
    0b11111,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b10001,
    0b11111
};

// 25% filled
static const uint8_t BAR_25[8] = {
    0b11111,
    0b10001,
    0b11001,
    0b11001,
    0b11001,
    0b11001,
    0b10001,
    0b11111
};

// 50% filled
static const uint8_t BAR_50[8] = {
    0b11111,
    0b10001,
    0b11101,
    0b11101,
    0b11101,
    0b11101,
    0b10001,
    0b11111
};

// 75% filled
static const uint8_t BAR_75[8] = {
    0b11111,
    0b10001,
    0b11111,
    0b11111,
    0b11111,
    0b11111,
    0b10001,
    0b11111
};

// Full block
static const uint8_t BAR_FULL[8] = {
    0b11111,
    0b11111,
    0b11111,
    0b11111,
    0b11111,
    0b11111,
    0b11111,
    0b11111
};

// Display progress bar (0-100%)
void lcd_progress_bar(uint8_t row, uint8_t percent) {
    // Map to 16 character positions
    int filled = (percent * 16) / 100;

    lcd_set_cursor(row, 0);
    for (int i = 0; i < 16; i++) {
        if (i < filled) {
            lcd_putchar_custom(4);  // Full block
        } else {
            lcd_putchar_custom(0);  // Empty block
        }
    }
}
```

### Animated Spinner

```c
// Define 4 spinner frames
static const uint8_t SPINNER_FRAMES[4][8] = {
    { 0b00100, 0b01000, 0b10000, 0b00000, 0b00000, 0b00001, 0b00010, 0b00100 },
    { 0b00100, 0b00100, 0b01000, 0b10000, 0b00000, 0b00000, 0b00001, 0b00010 },
    { 0b00010, 0b00100, 0b00100, 0b01000, 0b10000, 0b00000, 0b00000, 0b00001 },
    { 0b00001, 0b00010, 0b00100, 0b00100, 0b01000, 0b10000, 0b00000, 0b00000 }
};

// Animate spinner (call periodically)
void lcd_update_spinner(uint8_t frame) {
    lcd_define_char(ICON_CODE_SPINNER, SPINNER_FRAMES[frame % 4]);
    // Display with lcd_putchar_custom(ICON_CODE_SPINNER);
}
```

---

## Test Commands

Add test commands to verify CGRAM functionality:

```c
// In src/serial_commands.c

CMD_HANDLER(cmd_testcgram) {
    lcd_define_char(0, ICON_ARROW_RIGHT);
    lcd_define_char(1, ICON_CHECK);
    lcd_define_char(2, ICON_WARNING);

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_puts("CGRAM Test:");

    lcd_set_cursor(1, 0);
    lcd_putchar_custom(0);
    lcd_puts(" Arrow");

    lcd_set_cursor(2, 0);
    lcd_putchar_custom(1);
    lcd_puts(" Check");

    lcd_set_cursor(3, 0);
    lcd_putchar_custom(2);
    lcd_puts(" Warning");

    uart_puts("CGRAM custom characters displayed\r\n");
    return CMD_SUCCESS;
}

CMD_HANDLER(cmd_testbar) {
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_puts("Progress Test:");

    for (int i = 0; i <= 100; i += 10) {
        lcd_progress_bar(2, i);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    uart_puts("Progress bar test complete\r\n");
    return CMD_SUCCESS;
}
```

---

## Benefits

### Visual Improvements

1. **Status indicators** - Icons show motor state at a glance
2. **Direction arrows** - Clear visual for FWD/REV
3. **Mode indicators** - Service mode, tapping mode, etc.
4. **Progress feedback** - Bar graphs for depth, load, etc.

### User Experience

1. **Clearer UI** - Icons are faster to read than text
2. **Professional look** - Matches commercial firmware
3. **Language independent** - Icons work in any language
4. **Space efficient** - 1 char vs 4-6 text chars

### Implementation

1. **Simple** - Standard HD44780 feature, well-documented
2. **Fast** - No performance impact
3. **Compatible** - Works on all HD44780/ST7920 displays
4. **Tested** - Used in original firmware

---

## Migration Path

### Phase 1: Basic Icons (1-2 hours)
- Implement `lcd_define_char()`
- Create 8 basic icons
- Test with `TESTCGRAM` command

### Phase 2: UI Integration (1-2 hours)
- Add icons to status displays
- Update error messages with warning icon
- Service mode indicator

### Phase 3: Advanced Graphics (2-3 hours)
- Progress bars
- Bar graphs for RPM/load
- Animated spinner

---

## Files to Modify

| File | Change |
|------|--------|
| `include/lcd.h` | Add `lcd_define_char()`, `lcd_putchar_custom()` declarations |
| `src/lcd.c` | Implement CGRAM functions |
| `include/lcd_icons.h` | **NEW** - Icon library |
| `src/task_ui.c` | Use icons in UI displays |
| `src/serial_commands.c` | Add `TESTCGRAM`, `TESTBAR` commands |

---

## Testing Plan

1. **Unit test:** Define single character, verify display
2. **Icon test:** Display all 8 icons
3. **Update test:** Change icon definition dynamically
4. **UI test:** Integrate into main display
5. **Stress test:** Rapid icon updates (animated spinner)

---

## References

- HD44780 datasheet: Section 6 (CGRAM)
- ST7920 datasheet: Section 4.3 (Character Generator)
- Original firmware analysis: `/tmp/pattern_analysis.log`

---

## Next Session Prompt

```
I need to implement CGRAM custom characters for the Nova Voyager LCD display.

CONTEXT:
- Display: 16×4 character LCD (HD44780/ST7920 compatible)
- Current implementation: Text mode working perfectly
- Goal: Add custom character support (icons, progress bars)

FILES TO MODIFY:
- src/lcd.c: Add lcd_define_char() function
- include/lcd.h: Add function declarations
- include/lcd_icons.h: NEW - Icon library with 8 icons
- src/task_ui.c: Use icons in UI
- src/serial_commands.c: Add TESTCGRAM command

IMPLEMENTATION GUIDE:
See nova_firmware/docs/CGRAM_IMPLEMENTATION_GUIDE.md

Start with Phase 1 (basic icons), test with TESTCGRAM, then integrate into UI.
```

---

**Ready to implement. Estimated time: 3-4 hours total.** 🎯
