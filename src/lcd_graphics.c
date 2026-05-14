/**
 * @file lcd_graphics.c
 * @brief ST7920 Graphics Mode Implementation
 *
 * Implements basic graphics primitives for ST7920 128×64 display
 * Used for icons, bar graphs, and visual feedback
 */

#include "lcd.h"
#include "shared.h"
#include <string.h>

// External debug
extern void uart_puts(const char* s);

/*===========================================================================*/
/* ST7920 Graphics Mode Control                                              */
/*===========================================================================*/

static bool graphics_mode_active = false;

// Frame buffer for double buffering (128x64 pixels = 1024 bytes)
static uint8_t framebuffer[1024];

void lcd_graphics_enable(void) {
    lcd_cmd(0x34);  // Extended instruction set
    lcd_delay_ms(1);
    lcd_cmd(0x36);  // Graphics ON
    lcd_delay_ms(1);
    graphics_mode_active = true;
}

void lcd_graphics_disable(void) {
    lcd_cmd(0x30);  // Basic instruction set (back to text mode)
    lcd_delay_ms(1);
    graphics_mode_active = false;
}

/**
 * @brief Clear frame buffer (for games using framebuffer)
 */
void lcd_graphics_clear(void) {
    // Clear framebuffer (used by games)
    memset(framebuffer, 0, sizeof(framebuffer));
}

/*===========================================================================*/
/* Icon Definitions (8×8 pixel bitmaps)                                      */
/*===========================================================================*/

// Play/Forward icon (right-pointing triangle)
static const uint8_t icon_play[8] = {
    0b00010000,  // ...#....
    0b00011000,  // ...##...
    0b00011100,  // ...###..
    0b00011110,  // ...####.
    0b00011100,  // ...###..
    0b00011000,  // ...##...
    0b00010000,  // ...#....
    0b00000000   // ........
};

// Stop icon (filled square)
static const uint8_t icon_stop[8] = {
    0b00000000,  // ........
    0b01111110,  // .######.
    0b01111110,  // .######.
    0b01111110,  // .######.
    0b01111110,  // .######.
    0b01111110,  // .######.
    0b00000000,  // ........
    0b00000000   // ........
};

// Down arrow
static const uint8_t icon_down[8] = {
    0b00011000,  // ...##...
    0b00011000,  // ...##...
    0b00011000,  // ...##...
    0b01111110,  // .######.
    0b00111100,  // ..####..
    0b00011000,  // ...##...
    0b00000000,  // ........
    0b00000000   // ........
};

// Up arrow
static const uint8_t icon_up[8] = {
    0b00011000,  // ...##...
    0b00111100,  // ..####..
    0b01111110,  // .######.
    0b00011000,  // ...##...
    0b00011000,  // ...##...
    0b00011000,  // ...##...
    0b00000000,  // ........
    0b00000000   // ........
};

// Warning triangle
static const uint8_t icon_warning[8] = {
    0b00011000,  // ...##...
    0b00111100,  // ..####..
    0b01100110,  // .##..##.
    0b01000010,  // .#....#.
    0b01011010,  // .#.##.#.
    0b01111110,  // .######.
    0b00000000,  // ........
    0b00000000   // ........
};

// Checkmark/OK
static const uint8_t icon_ok[8] = {
    0b00000000,  // ........
    0b00000001,  // .......#
    0b00000011,  // ......##
    0b01000110,  // .#...##.
    0b01101100,  // .##.##..
    0b00111000,  // ..###...
    0b00010000,  // ...#....
    0b00000000   // ........
};

/*===========================================================================*/
/* Icon Drawing Function                                                      */
/*===========================================================================*/

/**
 * @brief Draw 8×8 icon in graphics mode
 * @param x X position in pixels (0-120)
 * @param y Y position in pixels (0-56)
 * @param icon Pointer to 8-byte icon data
 */
void lcd_draw_icon_8x8(uint8_t x, uint8_t y, const uint8_t* icon) {
    // ST7920 graphics coordinates:
    // Y: 0-31 (each unit = 2 pixel rows for 64 total rows)
    // X: 0-15 (each unit = 8 pixels for 128 total pixels)

    if (!graphics_mode_active) {
        uart_puts("Error: Graphics mode not enabled\r\n");
        return;
    }

    uint8_t y_addr = y / 2;  // Convert pixel row to Y address (0-31)
    uint8_t x_addr = x / 8;  // Convert pixel column to X address (0-15)

    // Draw 8 rows of the icon
    for (int row = 0; row < 8; row++) {
        uint8_t y_pos = y_addr + (row / 2);  // Each Y unit = 2 pixel rows

        // Set position
        lcd_cmd(0x80 | y_pos);       // Set Y address
        lcd_cmd(0x80 | x_addr);      // Set X address

        // Write icon byte
        if (row % 2 == 0) {
            lcd_data(icon[row]);      // Upper row
        } else {
            lcd_data(icon[row]);      // Lower row
        }
    }
}

/*===========================================================================*/
/* Icon Test Function                                                         */
/*===========================================================================*/

void lcd_test_icons(void) {
    uart_puts("\r\n=== ICON TEST (ST7920 Graphics) ===\r\n\r\n");

    // Enable graphics mode
    lcd_graphics_enable();
    uart_puts("Graphics mode enabled\r\n");

    // Clear graphics RAM (optional - fill with 0)
    uart_puts("Drawing icons...\r\n");

    // Draw icons at different positions
    lcd_draw_icon_8x8(0, 0, icon_play);      // Top-left
    delay_ms(200);

    lcd_draw_icon_8x8(16, 0, icon_stop);     // Next to play
    delay_ms(200);

    lcd_draw_icon_8x8(32, 0, icon_warning);  // Warning
    delay_ms(200);

    lcd_draw_icon_8x8(48, 0, icon_ok);       // OK/check
    delay_ms(200);

    lcd_draw_icon_8x8(0, 16, icon_up);       // Second row
    delay_ms(200);

    lcd_draw_icon_8x8(16, 16, icon_down);    // Down arrow
    delay_ms(200);

    uart_puts("Icons drawn - displaying for 3 seconds...\r\n");
    delay_ms(3000);  // Show icons (FreeRTOS delay, safe)

    // Return to text mode
    lcd_graphics_disable();
    lcd_clear();
    lcd_print_at(0, 0, "Icon test done");
    lcd_print_at(1, 0, "Did you see:");
    lcd_print_at(2, 0, "Play Stop Warn");
    lcd_print_at(3, 0, "OK Up Down");

    uart_puts("\r\n=== ICON TEST COMPLETE ===\r\n");
    uart_puts("If you saw 6 icons -> Graphics icons work!\r\n");
    uart_puts("Next: Can implement full graphics UI\r\n");
}

/**
 * @brief Systematic graphics memory layout test
 * Draws simple patterns to understand ST7920 addressing
 */
void lcd_test_graphics_layout(void) {
    // Watchdog refresh (prevent reset during test)
    extern void IWDG_KR_Write(uint16_t val);
    #define REFRESH_WATCHDOG() do { \
        volatile uint32_t* iwdg_kr = (volatile uint32_t*)0x40003000; \
        *iwdg_kr = 0xAAAA; \
    } while(0)

    uart_puts("\r\n=== ST7920 MEMORY LAYOUT TEST ===\r\n\r\n");

    lcd_graphics_enable();
    REFRESH_WATCHDOG();

    // Test 1: Horizontal line at top (QUICK TEST)
    uart_puts("Test 1: Horizontal line top\r\n");
    lcd_cmd(0x80);  // Y=0
    lcd_cmd(0x80);  // X=0
    for (int i = 0; i < 8; i++) {
        lcd_data(0xFF);  // First byte
        lcd_data(0xFF);  // Second byte (16 pixels per X)
    }
    uart_puts("-> Should see line across top (check NOW)\r\n");
    delay_ms(800);  // Shorter delay, safer
    REFRESH_WATCHDOG();

    // Test 2: Vertical line left edge
    uart_puts("Test 2: Vertical line left\r\n");
    for (int y = 0; y < 32; y++) {
        lcd_cmd(0x80 | y);
        lcd_cmd(0x80);
        lcd_data(0x80);  // Left pixel
        lcd_data(0x00);
    }
    uart_puts("-> Should see line down left side (check NOW)\r\n");
    delay_ms(800);
    REFRESH_WATCHDOG();

    // Test 3: Simple box (just corners)
    uart_puts("Test 3: Box corners\r\n");
    // Top-left corner
    lcd_cmd(0x80);  // Y=0
    lcd_cmd(0x80);  // X=0
    lcd_data(0xFF);
    lcd_data(0xFF);
    // Top-right corner
    lcd_cmd(0x80);  // Y=0
    lcd_cmd(0x87);  // X=7 (rightmost)
    lcd_data(0xFF);
    lcd_data(0xFF);
    uart_puts("-> Should see box corners (check NOW)\r\n");
    delay_ms(800);

    lcd_graphics_disable();
    REFRESH_WATCHDOG();
    lcd_clear();
    lcd_print_at(0, 0, "Layout test done");
    lcd_print_at(1, 0, "Patterns OK?");

    uart_puts("=== LAYOUT TEST COMPLETE ===\r\n");
    uart_puts("Total time: ~2.5 seconds (watchdog-safe)\r\n");
}

/*===========================================================================*/
/* Graphics Primitives for Games                                              */
/*===========================================================================*/

// Frame buffer for double buffering (128x64 pixels = 1024 bytes)
static uint8_t framebuffer[1024];

void lcd_graphics_mode(bool enable) {
    if (enable) {
        lcd_graphics_enable();
        memset(framebuffer, 0, sizeof(framebuffer));
    } else {
        lcd_graphics_disable();
    }
}

void lcd_graphics_pixel(int16_t x, int16_t y, bool value) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;

    uint16_t byte_idx = (y * 16) + (x / 8);
    uint8_t bit_mask = 0x80 >> (x % 8);

    if (value) {
        framebuffer[byte_idx] |= bit_mask;
    } else {
        framebuffer[byte_idx] &= ~bit_mask;
    }
}

void lcd_graphics_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bool value) {
    for (int16_t dy = 0; dy < h; dy++) {
        for (int16_t dx = 0; dx < w; dx++) {
            lcd_graphics_pixel(x + dx, y + dy, value);
        }
    }
}

void lcd_graphics_rect(int16_t x, int16_t y, int16_t w, int16_t h, bool value) {
    // Draw hollow rectangle
    for (int16_t dx = 0; dx < w; dx++) {
        lcd_graphics_pixel(x + dx, y, value);  // Top
        lcd_graphics_pixel(x + dx, y + h, value);  // Bottom
    }
    for (int16_t dy = 0; dy < h; dy++) {
        lcd_graphics_pixel(x, y + dy, value);  // Left
        lcd_graphics_pixel(x + w, y + dy, value);  // Right
    }
}

void lcd_graphics_update(void) {
    if (!graphics_mode_active) return;

    // Write framebuffer to GDRAM
    for (uint8_t y = 0; y < 32; y++) {
        lcd_cmd(0x80 | y);  // Set Y
        lcd_cmd(0x80);      // Set X=0

        for (uint8_t x = 0; x < 16; x++) {
            uint16_t fb_idx = (y * 16) + x;
            lcd_data(framebuffer[fb_idx]);
        }
    }
}

