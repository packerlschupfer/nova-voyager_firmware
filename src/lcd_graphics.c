/**
 * @file lcd_graphics.c
 * @brief ST7920 Graphics Mode — framebuffer + dirty-row flush
 *
 * From nova_voyager_games project. Dirty-row tracking avoids flushing
 * unchanged rows (~5ms per changed row vs ~74ms for full flush).
 */

#include "lcd.h"
#include "lcd_graphics.h"
#include "shared.h"
#include <string.h>

extern void uart_puts(const char* s);

/*===========================================================================*/
/* State & Framebuffer                                                        */
/*===========================================================================*/

static bool    graphics_mode_active = false;
static uint8_t framebuffer[1024];
static uint8_t prev_fb[1024];

void lcd_graphics_enable(void) {
    lcd_cmd(0x34);
    lcd_delay_ms(1);
    lcd_cmd(0x36);
    lcd_delay_ms(1);
    graphics_mode_active = true;
}

void lcd_graphics_disable(void) {
    lcd_cmd(0x34);
    lcd_delay_ms(1);
    lcd_cmd(0x30);
    lcd_delay_ms(1);
    graphics_mode_active = false;
}

void lcd_graphics_clear(void) {
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
 * @brief Draw 8x8 icon directly into GDRAM (no framebuffer)
 * @param x X position in pixels, MUST be a multiple of 16 (0,16,...,112)
 * @param y Y position in pixels (0-56)
 * @param icon Pointer to 8-byte icon data, one byte per row, MSB left
 *
 * REVIEW FIX: the previous addressing contradicted flush_row() in this same
 * file and could not have worked. It used y/2 as the Y address and advanced it
 * only every second row, so each icon row pair was written twice to the same
 * address; it used x/8 as the X address, but an ST7920 X address selects a
 * 16-pixel word, not a byte; and it wrote one byte per address, i.e. only the
 * high half of each word, leaving the low half as whatever was there.
 *
 * The real layout, which flush_row() has always used: for screen row y, the Y
 * address is y & 31 and the X address starts at 0 for the top half and 8 for
 * the bottom, with each X address covering 16 pixels written as two bytes.
 * An 8-pixel icon therefore owns the left byte of one word; the right byte is
 * written as 0 rather than left stale, which is why x must be 16-aligned.
 */
void lcd_draw_icon_8x8(uint8_t x, uint8_t y, const uint8_t* icon) {
    if (!graphics_mode_active) {
        uart_puts("Error: Graphics mode not enabled\r\n");
        return;
    }
    if (icon == NULL || x > 112 || (x % 16) != 0 || y > 56) {
        return;
    }

    const uint8_t x_word = (uint8_t)(x / 16);   // one X address per 16 pixels

    for (uint8_t row = 0; row < 8; row++) {
        const uint8_t screen_y = (uint8_t)(y + row);
        const uint8_t gram_y   = (uint8_t)(screen_y & 31);
        const uint8_t x_base   = (screen_y < 32) ? 0 : 8;

        lcd_cmd((uint8_t)(0x80U | gram_y));
        lcd_cmd((uint8_t)(0x80U | (x_base + x_word)));
        lcd_data(icon[row]);   // left 8 pixels of the word
        lcd_data(0x00);        // right 8 pixels: this word is the icon's alone
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

void lcd_graphics_mode(bool enable) {
    if (enable) {
        // Full init: clear DDRAM then enter graphics (prevents XOR artifacts)
        lcd_cmd(0x30); lcd_delay_ms(5);
        lcd_cmd(0x30); lcd_delay_ms(1);
        lcd_cmd(0x30); lcd_delay_ms(1);
        lcd_cmd(0x0C); lcd_delay_ms(2);
        lcd_cmd(0x01); lcd_delay_ms(10);
        lcd_cmd(0x06); lcd_delay_ms(2);

        for (uint8_t a = 0; a < 0x80; a++) {
            lcd_cmd(0x80 | a);
            lcd_data(0x20);
        }
        lcd_delay_ms(2);

        lcd_cmd(0x34); lcd_delay_ms(1);
        lcd_cmd(0x36); lcd_delay_ms(1);
        graphics_mode_active = true;

        memset(framebuffer, 0, sizeof(framebuffer));
        memset(prev_fb, 0, sizeof(prev_fb));

        // Clear both GRAM halves
        for (uint8_t y = 0; y < 32; y++) {
            lcd_cmd((uint8_t)(0x80U | y));
            lcd_cmd(0x80U);
            for (uint8_t x = 0; x < 16; x++) lcd_data(0x00);
        }
        for (uint8_t y = 0; y < 32; y++) {
            lcd_cmd((uint8_t)(0x80U | y));
            lcd_cmd((uint8_t)(0x80U | 8));
            for (uint8_t x = 0; x < 16; x++) lcd_data(0x00);
        }
    } else {
        lcd_graphics_disable();
    }
}

void lcd_graphics_pixel(int16_t x, int16_t y, bool value) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    uint16_t byte_idx = (uint16_t)((y * 16) + (x / 8));
    uint8_t  bit_mask = (uint8_t)(0x80U >> (x % 8));
    if (value) {
        framebuffer[byte_idx] |= bit_mask;
    } else {
        framebuffer[byte_idx] &= (uint8_t)~bit_mask;
    }
}

void lcd_graphics_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bool value) {
    for (int16_t dy = 0; dy < h; dy++)
        for (int16_t dx = 0; dx < w; dx++)
            lcd_graphics_pixel(x + dx, y + dy, value);
}

void lcd_graphics_rect(int16_t x, int16_t y, int16_t w, int16_t h, bool value) {
    for (int16_t dx = 0; dx <= w; dx++) {
        lcd_graphics_pixel(x + dx, y, value);
        lcd_graphics_pixel(x + dx, y + h, value);
    }
    for (int16_t dy = 1; dy < h; dy++) {
        lcd_graphics_pixel(x, y + dy, value);
        lcd_graphics_pixel(x + w, y + dy, value);
    }
}

static void flush_row(uint8_t screen_y) {
    uint8_t gram_y = screen_y & 31;
    uint8_t x_base = (screen_y < 32) ? 0 : 8;
    uint16_t offset = (uint16_t)screen_y * 16;
    lcd_cmd((uint8_t)(0x80U | gram_y));
    lcd_cmd((uint8_t)(0x80U | x_base));
    for (uint8_t x = 0; x < 16; x++)
        lcd_data(framebuffer[offset + x]);
    memcpy(&prev_fb[offset], &framebuffer[offset], 16);
}

void lcd_graphics_update(void) {
    if (!graphics_mode_active) return;
    for (uint8_t y = 0; y < 64; y++) {
        uint16_t offset = (uint16_t)y * 16;
        if (memcmp(&framebuffer[offset], &prev_fb[offset], 16) != 0)
            flush_row(y);
    }
}

void lcd_graphics_blit_1bit(const uint8_t *data) {
    if (!graphics_mode_active || data == NULL) return;
    memcpy(framebuffer, data, sizeof(framebuffer));
    lcd_graphics_update();
}

