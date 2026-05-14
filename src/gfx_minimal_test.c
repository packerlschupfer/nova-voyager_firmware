/**
 * @file gfx_minimal_test.c
 * @brief Absolute minimal ST7920 test - NO RAM clear
 */

#include "lcd.h"
#include "shared.h"

extern void uart_puts(const char* s);

#define REFRESH_WDG() do { \
    volatile uint32_t* iwdg = (volatile uint32_t*)0x40003000; \
    *iwdg = 0xAAAA; \
} while(0)

/**
 * @brief Minimal test - just enter graphics, write ONE byte, exit
 * NO RAM clear operation
 */
void cmd_gfxmin(void) {
    uart_puts("\r\n═══ MINIMAL GRAPHICS TEST ═══\r\n\r\n");
    uart_puts("NO RAM clear - just ONE write\r\n");
    uart_puts("Writing 0xFF at Y=0, X=0\r\n\r\n");

    // Enter graphics mode
    lcd_cmd(0x34);
    lcd_delay_ms(1);
    lcd_cmd(0x36);
    lcd_delay_ms(1);

    // Write ONE byte - that's it!
    lcd_cmd(0x80);  // Y=0
    lcd_cmd(0x80);  // X=0
    lcd_data(0xFF);  // One byte
    lcd_data(0x00);  // Second byte

    uart_puts("╔════════════════════════════════════╗\r\n");
    uart_puts("║  CHECK LCD - WHAT DO YOU SEE?      ║\r\n");
    uart_puts("╚════════════════════════════════════╝\r\n\r\n");

    // Show for 5 seconds
    for (int i = 0; i < 5; i++) {
        delay_ms(1000);
        REFRESH_WDG();
    }

    // Back to text
    lcd_cmd(0x30);
    lcd_delay_ms(1);
    lcd_clear();
    lcd_print_at(0, 0, "Minimal test OK");
    lcd_print_at(1, 0, "Report results");

    uart_puts("═══ TEST COMPLETE ═══\r\n\r\n");
    uart_puts("REPORT:\r\n");
    uart_puts("  • What appeared on screen?\r\n");
    uart_puts("  • Where was it? (upper/lower half, position)\r\n");
    uart_puts("  • Single element or multiple?\r\n");
}

/**
 * @brief Pause UI task to prevent LCD interference during graphics tests
 */
void cmd_pauseui(void) {
    extern TaskHandle_t g_task_ui;

    if (g_task_ui) {
        vTaskSuspend(g_task_ui);
        uart_puts("UI task SUSPENDED - LCD won't be updated\r\n");
        uart_puts("Graphics tests will run without interference\r\n");
        uart_puts("Power cycle to restore normal operation\r\n");
    } else {
        uart_puts("UI task handle not found\r\n");
    }
}

/**
 * @brief Resume UI task
 */
void cmd_resumeui(void) {
    extern TaskHandle_t g_task_ui;

    if (g_task_ui) {
        vTaskResume(g_task_ui);
        uart_puts("UI task RESUMED\r\n");
    }
}
