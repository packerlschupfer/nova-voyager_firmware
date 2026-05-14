/**
 * @file display.h
 * @brief Display Formatting and Status Screen
 *
 * High-level display functions for formatting and showing:
 *   - Main status screen (RPM, load, depth, mode)
 *   - Error screens
 *   - Boot messages
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

/**
 * @brief Update main status display
 *
 * Renders the main operating screen showing:
 *   - Row 0: Target RPM | Actual RPM
 *   - Row 1: Load% | State | Tap Mode | Direction
 *   - Row 2: Current Depth | Target Depth
 *   - Row 3: F-key labels
 *
 * Call periodically from UI task when not in menu mode.
 */
void display_update(void);

/**
 * @brief Display boot message
 * @param line1 First line of text
 * @param line2 Second line of text
 *
 * Shows a 2-line boot progress message, waits briefly.
 */
void display_boot_message(const char* line1, const char* line2);

/**
 * @brief Write right-aligned number
 * @param val Value to display
 * @param width Field width (characters)
 *
 * Writes number right-aligned with space padding.
 * Must call lcd_set_cursor() first.
 */
void display_write_num(uint16_t val, uint8_t width);

/**
 * @brief Write depth value in xxx.x format
 * @param depth_01mm Depth in 0.1mm units (signed)
 * @param width Field width (characters)
 *
 * Writes depth right-aligned with sign and decimal point.
 * Must call lcd_set_cursor() first.
 */
void display_write_depth(int16_t depth_01mm, uint8_t width);

#endif /* DISPLAY_H */
