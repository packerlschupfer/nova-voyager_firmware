/**
 * @file lcd.c
 * @brief HD44780 16x4 Character LCD Driver
 *
 * Driver for the Nova Voyager's HD44780-compatible 16x4 character LCD.
 * Uses 8-bit parallel interface on GPIOA (data) and GPIOB (control).
 */

#include "lcd.h"
#include "shared.h"  // For delay_ms() in graphics tests
#include "stm32f1xx_hal.h"

// External debug output
extern void uart_puts(const char* s);

/*===========================================================================*/
/* Hardware Macros                                                            */
/*===========================================================================*/

#define LCD_RS_HIGH()   (GPIOB->BSRR = (1 << 0))
#define LCD_RS_LOW()    (GPIOB->BRR  = (1 << 0))
#define LCD_RW_HIGH()   (GPIOB->BSRR = (1 << 1))
#define LCD_RW_LOW()    (GPIOB->BRR  = (1 << 1))
#define LCD_E_HIGH()    (GPIOB->BSRR = (1 << 2))
#define LCD_E_LOW()     (GPIOB->BRR  = (1 << 2))

/*===========================================================================*/
/* Private Functions                                                          */
/*===========================================================================*/

// Busy-wait microsecond delay
// Calibrated for GD32F303 @ 120MHz with -Os (~8 cycles/iter on M4)
// 1μs = 120MHz / 8cycles = 15 iterations → multiply by 12 for margin
static void lcd_delay_us(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 12; i++);
}

// Write byte to LCD data bus with enable pulse
// Timing (120MHz, -Os): RS setup ~2μs >> 100ns min; E-high ~2μs >> 450ns min;
// E-low ~150μs >> 70μs min; total cycle ~152μs >> 72μs min spec.
static void lcd_write_byte(uint8_t data) {
    GPIOA->ODR = (GPIOA->ODR & 0xFF00) | data;
    lcd_delay_us(2);   // RS/data setup: ~2μs (need ≥100ns)
    LCD_E_HIGH();
    lcd_delay_us(2);   // E-high: ~2μs (need ≥450ns)
    LCD_E_LOW();
    lcd_delay_us(125); // E-low: ~150μs (need ≥70μs for 72μs cycle)
}

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

void lcd_delay_ms(uint32_t ms) {
    // Busy-wait delay - safe before FreeRTOS scheduler starts
    // GD32F303 @ 120MHz, ~10 cycles/iter → 10/120MHz = 83ns/iter → 12000 iters/ms
    for (volatile uint32_t i = 0; i < ms * 12000; i++);
}

void lcd_cmd(uint8_t cmd) {
    LCD_RS_LOW();
    LCD_RW_LOW();
    lcd_write_byte(cmd);
    if (cmd <= 0x03) {
        lcd_delay_ms(2);  // Clear/home need extra time
    }
}

void lcd_data(uint8_t data) {
    LCD_RS_HIGH();
    LCD_RW_LOW();
    lcd_write_byte(data);
}

void lcd_clear(void) {
    lcd_cmd(0x01);
    lcd_delay_ms(2);
}

void lcd_set_cursor(uint8_t row, uint8_t col) {
    // Nova Voyager 16x4 LCD - non-standard but continuous addressing:
    // Row 0: 0xC0-0xCF (16 chars)
    // Row 1: 0xD0-0xDF (16 chars)
    // Row 2: 0xC8-0xD7 (16 chars, overlaps row 1 in DDRAM but different physical line)
    // Row 3: 0xD8-0xE7 (16 chars)
    // Auto-increment works within each row for all 16 columns
    static const uint8_t row_bases[4] = {0xC0, 0xD0, 0xC8, 0xD8};
    uint8_t addr = row_bases[row & 3] + (col & 0x0F);
    lcd_cmd(addr);
}

void lcd_print(const char* str) {
    while (*str) {
        lcd_data(*str++);
    }
}

void lcd_print_at(uint8_t row, uint8_t col, const char* str) {
    lcd_set_cursor(row, col);
    lcd_print(str);
}

/*===========================================================================*/
/* Graphics Capability Test Functions                                        */
/*===========================================================================*/

/**
 * @brief Create custom character in CGRAM
 * @param location Custom char location (0-7)
 * @param data 8 bytes of pixel data (5 bits used per byte)
 */
void lcd_create_char(uint8_t location, const uint8_t* data) {
    location &= 0x07;  // Only 8 locations available
    lcd_cmd(0x40 | (location << 3));  // Set CGRAM address
    for (int i = 0; i < 8; i++) {
        lcd_data(data[i]);  // Write 8 rows of pixel data
    }
    lcd_cmd(0x80);  // Return to DDRAM address 0
}

/**
 * @brief Test CGRAM custom character capability
 * Creates a heart icon and displays it
 */
void lcd_test_cgram(void) {
    uart_puts("Testing CGRAM custom characters...\r\n");

    // Define heart icon (5x8 pixels)
    // Bit pattern: xxxxx (5 bits used, 3 bits ignored)
    uint8_t heart[8] = {
        0b00000,  // .....
        0b01010,  // .#.#.
        0b11111,  // #####
        0b11111,  // #####
        0b01110,  // .###.
        0b00100,  // ..#..
        0b00000,  // .....
        0b00000   // .....
    };

    // Create custom character at location 0
    lcd_create_char(0, heart);

    // Display it on screen
    lcd_set_cursor(0, 0);
    lcd_data(0x00);  // Display custom char 0 (heart)
    lcd_print(" <- CGRAM Test");

    lcd_set_cursor(1, 0);
    lcd_print("If heart visible");
    lcd_set_cursor(2, 0);
    lcd_print("CGRAM WORKS!");

    uart_puts("CGRAM test complete - check LCD for heart icon\r\n");
}

/**
 * @brief Test ST7920 graphics mode capability
 * Attempts to enable graphics mode and draw test pattern
 */
void lcd_test_graphics_mode(void) {
    uart_puts("Testing ST7920 graphics mode...\r\n");

    // Save current state
    lcd_clear();

    // Try ST7920 extended instruction set
    lcd_cmd(0x30);  // Basic instruction set (8-bit interface)
    lcd_delay_ms(1);

    lcd_cmd(0x34);  // Extended instruction set enable
    lcd_delay_ms(1);

    lcd_cmd(0x36);  // Graphics display ON
    lcd_delay_ms(1);

    // Try to write pixel data at position (0,0)
    lcd_cmd(0x80);  // Set Y address (vertical, 0-31)
    lcd_cmd(0x80);  // Set X address (horizontal, 0-15 for 128 pixels/8)

    // Write test pattern (2 bytes = 16 pixels horizontal)
    lcd_data(0xFF);  // All pixels ON (first 8 pixels)
    lcd_data(0xFF);  // All pixels ON (next 8 pixels)

    lcd_delay_ms(10);  // Let display update

    // Try more positions
    lcd_cmd(0x81);  // Y=1
    lcd_cmd(0x80);  // X=0
    lcd_data(0xAA);  // Alternating pattern 10101010
    lcd_data(0x55);  // Alternating pattern 01010101

    // FIXED: Use FreeRTOS delay instead of busy-wait (prevents watchdog reset)
    delay_ms(500);  // Show for 0.5 seconds (was 2s busy-wait that caused watchdog)

    // Return to text mode
    lcd_cmd(0x30);  // Basic instruction set
    lcd_delay_ms(1);

    lcd_clear();
    lcd_print_at(0, 0, "Graphics test OK");
    lcd_print_at(1, 0, "Did you see");
    lcd_print_at(2, 0, "pixel pattern?");

    uart_puts("Graphics mode test complete\r\n");
    uart_puts("If you saw pixels/pattern -> ST7920 graphics works!\r\n");
    uart_puts("If display went blank -> Graphics mode not supported\r\n");
}

/**
 * @brief Test display capabilities and report findings
 */
void lcd_test_capabilities(void) {
    uart_puts("\r\n=== LCD CAPABILITY TEST ===\r\n\r\n");

    // Test 1: CGRAM custom characters
    uart_puts("Test 1: CGRAM Custom Characters\r\n");
    lcd_test_cgram();

    // Use FreeRTOS delay to prevent watchdog reset (shared.h included)
    delay_ms(1500);  // Show for 1.5 seconds (was 3s busy-wait)

    // Test 2: ST7920 graphics mode
    uart_puts("\r\nTest 2: ST7920 Graphics Mode\r\n");
    lcd_test_graphics_mode();

    uart_puts("\r\n=== TEST COMPLETE ===\r\n");
    uart_puts("Results:\r\n");
    uart_puts("- CGRAM: Check if heart icon appeared\r\n");
    uart_puts("- Graphics: Check if pixels appeared before text\r\n");
}

void lcd_init(bool show_splash) {
    uart_puts("LCD: GPIO...\r\n");

    // Enable GPIO clocks
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;

    // PA0-PA7: 8-bit data bus (push-pull 50MHz)
    GPIOA->CRL = 0x33333333;

    // PB0=RS, PB1=RW, PB2=E (push-pull 50MHz)
    GPIOB->CRL &= ~0x00000FFF;
    GPIOB->CRL |= 0x00000333;

    // Initialize control signals
    LCD_RS_LOW();
    LCD_RW_LOW();
    LCD_E_LOW();
    GPIOA->ODR = 0;

    uart_puts("LCD: delay 50ms...\r\n");
    lcd_delay_ms(50);  // Power-up delay

    uart_puts("LCD: cmd 0x38...\r\n");
    // HD44780 initialization sequence
    lcd_cmd(0x38);  // 8-bit, 2-line, 5x8
    lcd_delay_ms(5);
    lcd_cmd(0x38);
    lcd_delay_ms(1);
    lcd_cmd(0x38);

    uart_puts("LCD: cmd 0x0C, 0x06...\r\n");
    lcd_cmd(0x0C);  // Display on, cursor off
    lcd_cmd(0x06);  // Entry mode: increment

    uart_puts("LCD: clear...\r\n");
    lcd_clear();

    // Conditional splash screen (only on cold boot for fast soft boot)
    if (show_splash) {
        uart_puts("LCD: splash...\r\n");
        lcd_print_at(0, 2, "NOVA VOYAGER");
        lcd_print_at(1, 1, "Custom FW v1.0");
        uart_puts("LCD: splash delay...\r\n");
        lcd_delay_ms(300);  // Reduced from 1000ms for faster boot
        // Refresh watchdog after long delay (prevents timeout)
        IWDG->KR = 0xAAAA;
    }

    uart_puts("LCD: done\r\n");
}
