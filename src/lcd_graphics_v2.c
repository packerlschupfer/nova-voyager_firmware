/**
 * @file lcd_graphics_v2.c
 * @brief ST7920 Graphics - Corrected based on vertical stripe observation
 *
 * User feedback: Horizontal line test showed VERTICAL STRIPES
 * This means: Bytes represent VERTICAL pixels, or X/Y are swapped
 */

#include "lcd.h"
#include "shared.h"

// Watchdog refresh
#define REFRESH_WATCHDOG() do { \
    volatile uint32_t* iwdg_kr = (volatile uint32_t*)0x40003000; \
    *iwdg_kr = 0xAAAA; \
} while(0)

extern void uart_puts(const char* s);

void lcd_graphics_enable_v2(void) {
    lcd_cmd(0x34);  // Extended instruction set
    lcd_delay_ms(1);
    lcd_cmd(0x36);  // Graphics ON
    lcd_delay_ms(1);
}

void lcd_graphics_disable_v2(void) {
    lcd_cmd(0x30);  // Basic instruction set
    lcd_delay_ms(1);
}

/**
 * @brief Test with corrected understanding
 * Hypothesis: Each byte represents VERTICAL pixels (8 pixels tall)
 */
void cmd_testgfx_v2(void) {
    uart_puts("\r\n=== CORRECTED GRAPHICS TEST ===\r\n\r\n");
    uart_puts("Based on observation: bytes = vertical pixels\r\n\r\n");

    lcd_graphics_enable_v2();
    REFRESH_WATCHDOG();

    // Clear graphics RAM first
    uart_puts("Clearing graphics RAM...\r\n");
    for (int y = 0; y < 32; y++) {
        lcd_cmd(0x80 | y);
        lcd_cmd(0x80);
        for (int x = 0; x < 16; x++) {
            lcd_data(0x00);
        }
    }
    REFRESH_WATCHDOG();

    // Test: Single vertical stripe (should be ONE line)
    uart_puts("Test: Single byte at (0,0)\r\n");
    lcd_cmd(0x80);  // Y=0
    lcd_cmd(0x80);  // X=0
    lcd_data(0xFF);  // One byte
    uart_puts("-> Check LCD: Should see 8 vertical pixels or 8 horizontal?\r\n");
    delay_ms(1000);
    REFRESH_WATCHDOG();

    // Clear and test horizontal line (different approach)
    uart_puts("Test: Horizontal line attempt 2\r\n");
    lcd_cmd(0x80);  // Y=0
    lcd_cmd(0x80);  // X=0
    // Write same byte to multiple X positions
    for (int x = 0; x < 8; x++) {
        lcd_cmd(0x80);  // Reset Y=0
        lcd_cmd(0x80 | x);  // X position
        lcd_data(0xFF);  // Top pixel row
    }
    uart_puts("-> Check LCD: Line across top?\r\n");
    delay_ms(1000);
    REFRESH_WATCHDOG();

    lcd_graphics_disable_v2();
    lcd_clear();
    lcd_print_at(0, 0, "Test v2 complete");
    lcd_print_at(1, 0, "Report results");

    uart_puts("=== TEST COMPLETE ===\r\n");
}
