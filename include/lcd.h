/**
 * @file lcd.h
 * @brief ST7920 16x4 Character LCD Driver
 *
 * Driver for the Nova Voyager's ST7920-based 16x4 character LCD.
 * Uses 8-bit parallel interface on GPIOA (data) and GPIOB (control).
 *
 * Hardware connections:
 *   - GPIOA PA0-PA7: 8-bit data bus
 *   - GPIOB PB0: RS (Register Select)
 *   - GPIOB PB1: RW (Read/Write)
 *   - GPIOB PB2: E (Enable)
 *
 * DDRAM addressing — READ THIS BEFORE WRITING A NEW SCREEN.
 *
 * The ST7920 is WORD-addressed: each DDRAM address holds TWO characters. Each
 * row therefore owns 8 addresses, not 16:
 *
 *   Row 0: 0xC0-0xC7    Row 1: 0xD0-0xD7
 *   Row 2: 0xC8-0xCF    Row 3: 0xD8-0xDF
 *
 * The `col` argument of lcd_set_cursor()/lcd_print_at() is a WORD index 0-7,
 * so column N is character 2N and a row fits 16 characters. Two consequences
 * that have each caused a real bug:
 *
 *   - A column of 8 or more is not "past the end of the row", it is the next
 *     row's base: row 1 column 8 is 0xD8, which is row 3 column 0. The column
 *     is clamped to 7 now, but the arithmetic is worth knowing.
 *   - Auto-increment runs through the row's 16 characters and then continues
 *     into whichever row follows in DDRAM, so a string longer than the space
 *     remaining spills onto a different physical line.
 *
 * This block previously documented one character per address (Row 0:
 * 0xC0-0xCF and so on). That reading is what produced the CalcRPM screen
 * printing onto row 3 and pong's win banner spilling onto row 2.
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
 * @param col Column — ONLY col=0 is reliable on ST7920 (word-addressed DDRAM)
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
/* Dirty-Row Update                                                          */
/*===========================================================================*/

/**
 * @brief Update a row, only rewriting characters that changed since last call.
 * @param row Row (0-3)
 * @param buf Exactly LCD_COLS (16) characters (not null-terminated)
 *
 * Compares against an internal shadow buffer. On first call after
 * lcd_shadow_invalidate(), writes the entire row. Subsequent calls
 * only write the changed span (first..last dirty char).
 */
void lcd_update_row(uint8_t row, const char* buf);

/**
 * @brief Update a row with raw bytes (for 2-byte CGRAM/CGROM chars).
 * Same dirty tracking as lcd_update_row but takes uint8_t buffer.
 */
void lcd_update_row_2byte(uint8_t row, const uint8_t* buf);

/**
 * @brief Mark shadow buffer invalid (forces full rewrite on next update).
 * Call after lcd_clear() or any operation that changes display content
 * outside of lcd_update_row().
 */
void lcd_shadow_invalidate(void);

/**
 * @brief Atomically copy the whole text shadow.
 * @param out Destination [LCD_ROWS][LCD_COLS]; rows are NOT NUL-terminated.
 * @return false if nothing has been painted via the row path (or a
 *         full-screen flow has invalidated it), in which case out is untouched.
 * @note Snapshot, not a live pointer, so a dump cannot be torn by the display
 *       task mid-print. See the caveat in lcd.c.
 */
bool lcd_shadow_snapshot(char out[LCD_ROWS][LCD_COLS]);

/**
 * @brief Mark shadow buffer as valid (enables dirty tracking).
 * Call once after the first full frame has been written via lcd_update_row().
 */
void lcd_shadow_commit(void);

/*===========================================================================*/
/* Graphics Capability Test Functions                                        */
/*===========================================================================*/

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
