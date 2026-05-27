# ST7920 Graphics Memory Layout Investigation

**Date:** 2026-01-30
**Status:** Graphics mode confirmed working, addressing needs correction

## Test Results Summary

### What Worked
✅ **TESTGFX (Simple pixels):** Pixel pattern visible
- Basic graphics mode activation works
- Writing to graphics RAM works
- Simple 0xFF bytes show pixels

### What Failed
❌ **TESTICONS (8×8 icons):** Garbled display
- Icons appeared as garbage
- Indicates addressing calculation is wrong
- Graphics mode works but coordinate system incorrect

## Problem Analysis

### Current Implementation (WRONG)

```c
void lcd_draw_icon_8x8(uint8_t x, uint8_t y, const uint8_t* icon) {
    uint8_t y_addr = y / 2;    // Assuming Y unit = 2 pixel rows
    uint8_t x_addr = x / 8;    // Assuming X unit = 8 pixels

    for (int row = 0; row < 8; row++) {
        lcd_cmd(0x80 | y_pos);  // Set Y
        lcd_cmd(0x80 | x_addr); // Set X
        lcd_data(icon[row]);    // Write data
    }
}
```

**Issue:** Garbled output suggests wrong addressing scheme

### ST7920 Graphics RAM Layout (Theory)

**Display:** 128×64 pixels
**RAM Organization:** Need to determine

**Possibilities:**
1. **Horizontal addressing:** Rows are contiguous (0-127 pixels per row, 64 rows)
2. **Vertical addressing:** Columns are contiguous
3. **Bank-based:** Display split into banks/pages
4. **Tile-based:** 8×8 or 16×16 pixel tiles

## Investigation Plan

### Test 1: Horizontal Line Test

Draw horizontal line at Y=0 by writing sequential bytes:
```c
lcd_cmd(0x80);  // Y=0
lcd_cmd(0x80);  // X=0
for (int i = 0; i < 16; i++) {
    lcd_data(0xFF);  // 16 bytes = 128 pixels
}
```

**Expected if horizontal:** Solid line across top
**Expected if vertical:** Vertical lines or tiles

### Test 2: Vertical Line Test

Draw vertical line by incrementing Y:
```c
for (int y = 0; y < 32; y++) {
    lcd_cmd(0x80 | y);  // Set Y
    lcd_cmd(0x80);      // X=0
    lcd_data(0x80);     // Left-most pixel of first byte
}
```

**Expected:** Vertical line on left edge

### Test 3: Checkerboard Pattern

Alternating 0xAA and 0x55:
```c
for (int y = 0; y < 32; y++) {
    lcd_cmd(0x80 | y);
    lcd_cmd(0x80);
    for (int x = 0; x < 16; x++) {
        lcd_data((y + x) % 2 ? 0xAA : 0x55);
    }
}
```

**Expected:** Checkerboard pattern
**Result:** Reveals memory organization

## ST7920 Datasheet Notes

### Standard ST7920 Graphics RAM

**Resolution:** 128×64 pixels

**Graphics RAM Addressing:**
- **Vertical coordinate (Y):** 0-31 (0x00-0x1F)
- **Horizontal coordinate (X):** 0-7 (0x00-0x07)

**Each Y position = 2 pixel rows**
**Each X position = 16 pixels (2 bytes)**

**Correct sequence:**
```c
lcd_cmd(0x80 | y);   // Y address (0-31)
lcd_cmd(0x80 | x);   // X address (0-7)
lcd_data(byte1);     // Upper 8 pixels
lcd_data(byte2);     // Lower 8 pixels
```

**IMPORTANT:** Must write 2 bytes per position (16 pixels)!

## Likely Issue in Our Code

**Problem:** Writing only 1 byte per position
```c
lcd_data(icon[row]);  // Only 1 byte!
```

**Should be:** Write 2 bytes
```c
lcd_data(icon[row]);      // Upper 8 pixels
lcd_data(icon[row]);      // Lower 8 pixels (or 0x00)
```

## Corrected Icon Drawing

```c
void lcd_draw_icon_8x8_FIXED(uint8_t x, uint8_t y, const uint8_t* icon) {
    // ST7920: Y=0-31 (each = 2 pixel rows), X=0-7 (each = 16 pixels/2 bytes)
    uint8_t y_start = y / 2;     // Which Y address (0-31)
    uint8_t x_pos = x / 16;      // Which X address (0-7)
    bool left_half = (x % 16) < 8;  // Left or right half of 16-pixel block

    for (int row = 0; row < 8; row += 2) {
        lcd_cmd(0x80 | (y_start + row/2));  // Y address
        lcd_cmd(0x80 | x_pos);               // X address

        if (left_half) {
            lcd_data(icon[row]);     // Upper row, left 8 pixels
            lcd_data(0x00);          // Upper row, right 8 pixels (blank)
        } else {
            lcd_data(0x00);          // Upper row, left 8 pixels (blank)
            lcd_data(icon[row]);     // Upper row, right 8 pixels
        }
    }
}
```

## Recommended Next Step

**Create systematic test command:**

```c
void cmd_testgfxmap(void) {
    // Test 1: Full screen fill
    lcd_graphics_enable();
    for (int y = 0; y < 32; y++) {
        lcd_cmd(0x80 | y);
        lcd_cmd(0x80);
        for (int x = 0; x < 8; x++) {
            lcd_data(0xFF);  // Write 2 bytes per X
            lcd_data(0xFF);
        }
    }
    delay_ms(2000);
    // Should fill entire screen with pixels

    // Test 2: Horizontal lines
    lcd_graphics_enable();
    for (int y = 0; y < 32; y += 4) {
        lcd_cmd(0x80 | y);
        lcd_cmd(0x80);
        for (int x = 0; x < 8; x++) {
            lcd_data(0xFF);
            lcd_data(0xFF);
        }
    }
    delay_ms(2000);
    // Should show horizontal lines every 8 pixels
}
```

## Conclusion

**Graphics mode works** but addressing is incorrect.

**Fix needed:**
1. Write 2 bytes per X position (not 1)
2. Account for 16-pixel X granularity (not 8)
3. Test systematically to find correct layout

**Next:** Implement corrected addressing and retest icons.
