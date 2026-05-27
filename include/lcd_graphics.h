/**
 * @file lcd_graphics.h
 * @brief ST7920 Graphics Mode API
 *
 * Graphics primitives for ST7920 128×64 pixel display
 */

#ifndef LCD_GRAPHICS_H
#define LCD_GRAPHICS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Enable ST7920 graphics mode
 */
void lcd_graphics_enable(void);

/**
 * @brief Disable graphics mode, return to text
 */
void lcd_graphics_disable(void);

/**
 * @brief Draw 8x8 pixel icon in graphics mode
 * @param x X position in pixels, must be a multiple of 16 (0,16,...,112)
 * @param y Y position in pixels (0-56)
 * @param icon Pointer to 8-byte icon bitmap, one byte per row, MSB left
 */
void lcd_draw_icon_8x8(uint8_t x, uint8_t y, const uint8_t* icon);
void lcd_draw_icon_8x8(uint8_t x, uint8_t y, const uint8_t* icon);

/**
 * @brief Test icon drawing capabilities
 * Draws 6 test icons (play, stop, warning, OK, up, down)
 */
void lcd_test_icons(void);

/**
 * @brief Systematic graphics memory layout test
 * Draws simple patterns (lines, box) to understand ST7920 addressing
 */
void lcd_test_graphics_layout(void);

/*===========================================================================*/
/* Graphics Primitives for Games                                             */
/*===========================================================================*/

/**
 * @brief Enable/disable graphics mode with frame buffer
 * @param enable true to enable graphics mode, false to disable
 */
void lcd_graphics_mode(bool enable);

/**
 * @brief Clear frame buffer
 */
void lcd_graphics_clear(void);

/**
 * @brief Set a single pixel
 * @param x X coordinate (0-127)
 * @param y Y coordinate (0-63)
 * @param value true to set pixel, false to clear
 */
void lcd_graphics_pixel(int16_t x, int16_t y, bool value);

/**
 * @brief Draw filled rectangle
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param value true to fill, false to clear
 */
void lcd_graphics_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, bool value);

/**
 * @brief Draw hollow rectangle
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @param value true to draw, false to clear
 */
void lcd_graphics_rect(int16_t x, int16_t y, int16_t w, int16_t h, bool value);

/**
 * @brief Update display from frame buffer
 */
void lcd_graphics_update(void);

/**
 * @brief Blit a full 128×64 1-bit bitmap directly into the framebuffer and
 *        push it to the LCD. `data` must be exactly 1024 bytes, row-major,
 *        MSB-left, 16 bytes/row, 1 = pixel ON — same format the ST7920
 *        graphics-mode framebuffer already uses, so this is a memcpy + update.
 */
void lcd_graphics_blit_1bit(const uint8_t *data);

#endif // LCD_GRAPHICS_H
