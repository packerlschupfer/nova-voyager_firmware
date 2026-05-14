/**
 * @file lcd.h
 * @brief HD44780 16x4 Character LCD Driver
 *
 * Driver for the Nova Voyager's HD44780-compatible 16x4 character LCD.
 * Uses 8-bit parallel interface on GPIOA (data) and GPIOB (control).
 *
 * Hardware connections:
 *   - GPIOA PA0-PA7: 8-bit data bus
 *   - GPIOB PB0: RS (Register Select)
 *   - GPIOB PB1: RW (Read/Write)
 *   - GPIOB PB2: E (Enable)
 *
 * Note: This LCD has non-standard DDRAM addressing:
 *   Row 0: 0xC0-0xCF
 *   Row 1: 0xD0-0xDF
 *   Row 2: 0xC8-0xD7
 *   Row 3: 0xD8-0xE7
 */

#ifndef LCD_H
#define LCD_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* LCD Dimensions                                                             */
/*===========================================================================*/

#define LCD_ROWS    4
#define LCD_COLS    16

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

/**
 * @brief Initialize LCD GPIO and hardware
 *
 * Configures GPIO pins for 8-bit parallel interface and performs
 * HD44780 initialization sequence. Call during boot before scheduler.
 *
 * @param show_splash If true, display "NOVA VOYAGER" splash for 300ms
 */
void lcd_init(bool show_splash);

/**
 * @brief Send command byte to LCD
 * @param cmd Command byte (e.g., 0x01 for clear, 0x0C for display on)
 */
void lcd_cmd(uint8_t cmd);

/**
 * @brief Send data byte to LCD (character)
 * @param data ASCII character to display
 */
void lcd_data(uint8_t data);

/**
 * @brief Clear display and return cursor to home
 */
void lcd_clear(void);

/**
 * @brief Set cursor position
 * @param row Row (0-3)
 * @param col Column (0-15)
 */
void lcd_set_cursor(uint8_t row, uint8_t col);

/**
 * @brief Print string at current cursor position
 * @param str Null-terminated string
 */
void lcd_print(const char* str);

/**
 * @brief Print string at specified position
 * @param row Row (0-3)
 * @param col Column (0-15)
 * @param str Null-terminated string
 */
void lcd_print_at(uint8_t row, uint8_t col, const char* str);

/**
 * @brief Delay in milliseconds (busy-wait)
 *
 * Safe to use before FreeRTOS scheduler starts.
 * @param ms Milliseconds to delay
 */
void lcd_delay_ms(uint32_t ms);

/*===========================================================================*/
/* Graphics Capability Test Functions                                        */
/*===========================================================================*/

/**
 * @brief Create custom character in CGRAM
 * @param location Custom character location (0-7)
 * @param data 8 bytes of pixel data (5×8 pixels, 5 bits used per byte)
 *
 * HD44780 CGRAM allows 8 custom characters.
 * Each byte represents one row (5 pixels: xxxxx)
 */
void lcd_create_char(uint8_t location, const uint8_t* data);

/**
 * @brief Test CGRAM custom character capability
 *
 * Creates and displays a heart icon to verify CGRAM works.
 * If heart appears on LCD, custom graphics are supported.
 */
void lcd_test_cgram(void);

/**
 * @brief Test ST7920 graphics mode capability
 *
 * Attempts to enable ST7920 graphics mode and draw test pixels.
 * If display shows pixels or pattern, full 128×64 graphics mode is available.
 * If display goes blank or shows garbage, graphics mode not supported.
 */
void lcd_test_graphics_mode(void);

/**
 * @brief Comprehensive display capability test
 *
 * Runs both CGRAM and ST7920 graphics tests.
 * Reports results to UART console.
 */
void lcd_test_capabilities(void);

#endif /* LCD_H */
