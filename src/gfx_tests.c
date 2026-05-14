/**
 * @file gfx_tests.c
 * @brief ST7920 Graphics Coordinate Mapping Tests
 *
 * 5 independent tests to systematically map graphics coordinates
 * Each test: Clear → Write ONE pattern → Show → Report
 */

#include "lcd.h"
#include "shared.h"

extern void uart_puts(const char* s);
extern void print_num(int32_t n);

#define REFRESH_WDG() do { \
    volatile uint32_t* iwdg = (volatile uint32_t*)0x40003000; \
    *iwdg = 0xAAAA; \
} while(0)

// Helper: Clear all graphics RAM
static void clear_gfx_ram(void) {
    for (uint8_t y = 0; y < 32; y++) {
        lcd_cmd(0x80 | y);
        lcd_cmd(0x80);
        for (uint8_t x = 0; x < 16; x++) {
            lcd_data(0x00);
        }
        if (y % 4 == 0) REFRESH_WDG();
    }
}

// Helper: Single graphics test
static void run_gfx_test(const char* name, uint8_t y, uint8_t x, uint8_t pattern) {
    uart_puts("\r\n═══ ");
    uart_puts(name);
    uart_puts(" ═══\r\n\r\n");

    // Enter graphics mode
    lcd_cmd(0x34);  // Extended
    lcd_delay_ms(1);
    lcd_cmd(0x36);  // Graphics ON
    lcd_delay_ms(1);
    REFRESH_WDG();

    // Clear graphics RAM
    uart_puts("Clearing graphics RAM...\r\n");
    clear_gfx_ram();
    REFRESH_WDG();

    // Write pattern
    uart_puts("Writing: Y=");
    print_num(y);
    uart_puts(" X=");
    print_num(x);
    uart_puts(" Pattern=0x");
    char h = (pattern >> 4) > 9 ? 'A' + (pattern >> 4) - 10 : '0' + (pattern >> 4);
    char l = (pattern & 0xF) > 9 ? 'A' + (pattern & 0xF) - 10 : '0' + (pattern & 0xF);
    uart_putc(h);
    uart_putc(l);
    uart_puts("\r\n\r\n");

    lcd_cmd(0x80 | y);
    lcd_cmd(0x80 | x);
    lcd_data(pattern);
    lcd_data(0x00);

    uart_puts("╔════════════════════════════════════╗\r\n");
    uart_puts("║  CHECK LCD NOW - 3 SECONDS!        ║\r\n");
    uart_puts("╚════════════════════════════════════╝\r\n\r\n");

    // Show for 3 seconds
    for (int i = 0; i < 3; i++) {
        delay_ms(1000);
        REFRESH_WDG();
    }

    // Back to text
    lcd_cmd(0x30);
    lcd_delay_ms(1);
    lcd_clear();
    lcd_print_at(0, 0, name);
    lcd_print_at(1, 0, "Where did");
    lcd_print_at(2, 0, "pattern appear?");

    uart_puts("Test complete. REPORT:\r\n");
    uart_puts("  • Where on screen? (top/middle/bottom, left/right)\r\n");
    uart_puts("  • What shape? (single pixel, line, block)\r\n");
    uart_puts("  • Vertical or horizontal?\r\n\r\n");
}

// Test commands
void cmd_gfxtest1(void) {
    run_gfx_test("GFXTEST1: Bit 0", 0, 0, 0x01);
}

void cmd_gfxtest2(void) {
    run_gfx_test("GFXTEST2: Bit 7", 0, 0, 0x80);
}

void cmd_gfxtest3(void) {
    run_gfx_test("GFXTEST3: Full Byte", 0, 0, 0xFF);
}

void cmd_gfxtest4(void) {
    run_gfx_test("GFXTEST4: Y=1", 1, 0, 0xFF);
}

void cmd_gfxtest5(void) {
    run_gfx_test("GFXTEST5: X=1", 0, 1, 0xFF);
}
