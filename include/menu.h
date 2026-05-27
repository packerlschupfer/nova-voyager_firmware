/**
 * @file menu.h
 * @brief Menu System for 16x4 LCD Configuration UI
 *
 * Hierarchical menu navigation using encoder and buttons.
 * Manages configuration of speed, tapping, depth, motor settings.
 *
 * Features:
 *   - Hierarchical submenus (Speed, Tapping, Depth, Motor, etc.)
 *   - Integer and enum value editing
 *   - Settings cache with apply-on-exit
 *   - System actions (Save, Reset, DFU)
 */

#ifndef MENU_H
#define MENU_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

/**
 * @brief Enter the menu system
 *
 * Loads current settings into cache and sets initial menu state.
 * Call when user presses MENU button.
 */
void menu_enter(void);

/**
 * @brief Exit the menu system
 *
 * Applies cached settings and returns to normal display.
 */
void menu_exit(void);

/**
 * @brief Handle encoder click in menu
 *
 * Enters submenu, starts editing, confirms edit, or goes back.
 */
void menu_click(void);

/**
 * @brief Handle encoder rotation in menu
 * @param delta Rotation direction (+1 = CW/down, -1 = CCW/up)
 *
 * Navigates menu items or adjusts values when editing.
 */
void menu_rotate(int8_t delta);

/**
 * @brief Draw current menu screen to LCD
 *
 * Renders 4 visible menu items with selection indicator.
 * Call periodically from UI task when menu is active.
 */
void menu_draw(void);

/**
 * @brief Go back one menu level
 *
 * Returns to parent menu or exits if at top level.
 * Call when user presses F1/back button.
 */
void menu_back(void);

#endif /* MENU_H */
