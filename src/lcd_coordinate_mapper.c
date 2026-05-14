/**
 * @file lcd_coordinate_mapper.c
 * @brief ST7920 Coordinate Mapping Tool
 *
 * Systematically tests graphics RAM positions to build coordinate map.
 * User reports where each pattern appears, we document the mapping.
 */

#include "lcd.h"
#include "shared.h"
#include <stdio.h>

extern void uart_puts(const char* s);
extern void print_num(int32_t n);

// Watchdog refresh
#define REFRESH_WATCHDOG() do { \
    volatile uint32_t* iwdg_kr = (volatile uint32_t*)0x40003000; \
    *iwdg_kr = 0xAAAA; \
} while(0)

/**
 * @brief Interactive coordinate mapper
 * Tests one position at a time, waits for user confirmation
 */
void lcd_map_coordinates_interactive(void) {
    uart_puts("\r\n╔══════════════════════════════════════╗\r\n");
    uart_puts("║  ST7920 COORDINATE MAPPING TOOL      ║\r\n");
    uart_puts("╚══════════════════════════════════════╝\r\n\r\n");

    uart_puts("This will test each graphics position.\r\n");
    uart_puts("For each test, report WHERE you see the pattern.\r\n\r\n");

    // Enable graphics mode
    lcd_cmd(0x34);  // Extended instruction
    lcd_delay_ms(1);
    lcd_cmd(0x36);  // Graphics ON
    lcd_delay_ms(1);

    uart_puts("Graphics mode enabled.\r\n");
    uart_puts("Starting coordinate scan...\r\n\r\n");

    // Test grid: Y=0-7, X=0-3 (subset for speed)
    for (uint8_t y = 0; y < 8; y++) {
        for (uint8_t x = 0; x < 4; x++) {
            REFRESH_WATCHDOG();

            // Clear previous
            lcd_cmd(0x30);  // Text mode
            lcd_delay_ms(1);
            lcd_cmd(0x34);  // Back to extended
            lcd_delay_ms(1);
            lcd_cmd(0x36);  // Graphics ON
            lcd_delay_ms(1);

            // Set position
            lcd_cmd(0x80 | y);
            lcd_cmd(0x80 | x);

            // Write test pattern (2 bytes = 16 pixels)
            lcd_data(0xFF);  // All ON
            lcd_data(0x00);  // Half ON

            uart_puts("Test Y=");
            print_num(y);
            uart_puts(" X=");
            print_num(x);
            uart_puts(" -> Check LCD, press ENTER\r\n");

            // Wait for user (they press ENTER in console)
            delay_ms(5000);  // 5 second view time
        }
    }

    // Done
    lcd_cmd(0x30);  // Back to text
    lcd_delay_ms(1);
    lcd_clear();
    lcd_print_at(0, 0, "Mapping complete");

    uart_puts("\r\n╔══════════════════════════════════════╗\r\n");
    uart_puts("║  COORDINATE MAPPING COMPLETE         ║\r\n");
    uart_puts("╚══════════════════════════════════════╝\r\n");
}

/**
 * @brief Quick coordinate probe - tests specific positions
 */
void lcd_probe_position(uint8_t y, uint8_t x, uint8_t pattern) {
    uart_puts("\r\n=== POSITION PROBE ===\r\n");
    uart_puts("Y=");
    print_num(y);
    uart_puts(" X=");
    print_num(x);
    uart_puts(" Pattern=0x");

    // Print hex
    char hex[3];
    hex[0] = (pattern >> 4) > 9 ? 'A' + (pattern >> 4) - 10 : '0' + (pattern >> 4);
    hex[1] = (pattern & 0xF) > 9 ? 'A' + (pattern & 0xF) - 10 : '0' + (pattern & 0xF);
    hex[2] = '\0';
    uart_puts(hex);
    uart_puts("\r\n\r\n");

    // Enable graphics
    lcd_cmd(0x34);
    lcd_delay_ms(1);
    lcd_cmd(0x36);
    lcd_delay_ms(1);

    // Set position and write
    lcd_cmd(0x80 | y);
    lcd_cmd(0x80 | x);
    lcd_data(pattern);
    lcd_data(0x00);

    uart_puts("Pattern drawn. Check LCD for 2 seconds...\r\n");
    delay_ms(2000);

    // Back to text
    lcd_cmd(0x30);
    lcd_delay_ms(1);
    lcd_clear();
    lcd_print_at(0, 0, "Probe done");
    lcd_print_at(1, 0, "Where was it?");

    uart_puts("Report: Where did pattern appear?\r\n");
}

/**
 * @brief Test byte orientation (vertical vs horizontal pixels)
 */
void lcd_test_byte_orientation(void) {
    uart_puts("\r\n=== BYTE ORIENTATION TEST ===\r\n\r\n");

    lcd_cmd(0x34);
    lcd_cmd(0x36);
    lcd_delay_ms(1);
    REFRESH_WATCHDOG();

    // Test 1: Single pixel patterns
    uart_puts("Test 1: Top pixel only (0x01)\r\n");
    lcd_cmd(0x80);  // Y=0
    lcd_cmd(0x80);  // X=0
    lcd_data(0x01);  // Bit 0 only
    lcd_data(0x00);
    uart_puts("-> Check LCD: Where is ONE pixel?\r\n");
    delay_ms(1500);
    REFRESH_WATCHDOG();

    // Test 2: Bottom pixel
    uart_puts("Test 2: Bottom pixel (0x80)\r\n");
    lcd_cmd(0x80);
    lcd_cmd(0x81);  // X=1 (next position)
    lcd_data(0x80);  // Bit 7 only
    lcd_data(0x00);
    uart_puts("-> Check LCD: Where is this pixel relative to first?\r\n");
    delay_ms(1500);
    REFRESH_WATCHDOG();

    // Test 3: Full byte
    uart_puts("Test 3: Full byte (0xFF)\r\n");
    lcd_cmd(0x80);
    lcd_cmd(0x82);  // X=2
    lcd_data(0xFF);
    lcd_data(0x00);
    uart_puts("-> Check LCD: 8 pixels vertical or horizontal?\r\n");
    delay_ms(2000);

    lcd_cmd(0x30);
    lcd_delay_ms(1);
    lcd_clear();
    lcd_print_at(0, 0, "Orientation test");
    lcd_print_at(1, 0, "done. Report:");
    lcd_print_at(2, 0, "Vert or horiz?");

    uart_puts("\r\n=== ORIENTATION TEST COMPLETE ===\r\n");
    uart_puts("Report: Were pixels VERTICAL or HORIZONTAL?\r\n");
}
