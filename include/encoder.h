/**
 * @file encoder.h
 * @brief UI Rotary Encoder Driver
 *
 * Software-based quadrature decoder using polling.
 * Encoder on PC13/PC14, button on PC15.
 * Returns detent clicks (4 quadrature counts = 1 detent).
 */

#ifndef ENCODER_H
#define ENCODER_H

#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

/**
 * @brief Initialize encoder GPIO and interrupts
 */
void encoder_init(void);

/**
 * @brief Get encoder position (accumulated clicks)
 * @return Current position value
 */
int16_t encoder_get_position(void);

/**
 * @brief Get delta since last call and reset
 * @return Number of clicks since last call (positive=CW, negative=CCW)
 */
int8_t encoder_get_delta(void);

/**
 * @brief Reset encoder position to zero
 */
void encoder_reset(void);

/**
 * @brief Check if encoder button is pressed
 * @return true if button pressed
 */
bool encoder_button_pressed(void);

/**
 * @brief Check for button press event (edge detection)
 * @return true if button was just pressed
 */
bool encoder_button_clicked(void);

/**
 * @brief Call from EXTI interrupt handler (unused, polling used instead)
 * @param pin The GPIO pin that triggered
 */
void encoder_irq_handler(uint16_t pin);

/**
 * @brief Update encoder state (call from main loop for polling mode)
 */
void encoder_update(void);

/**
 * @brief Get ISR call count (for debugging)
 */
uint32_t encoder_get_isr_count(void);

/**
 * @brief Check if F1 button was clicked (EXTI-based, with debounce)
 * @return true if F1 was pressed since last check
 */
bool encoder_f1_clicked(void);

/**
 * @brief Check if F2 button was clicked (EXTI-based, with debounce)
 * @return true if F2 was pressed since last check
 */
bool encoder_f2_clicked(void);

/**
 * @brief Check if F3 button was clicked (EXTI-based, with debounce)
 * @return true if F3 was pressed since last check
 */
bool encoder_f3_clicked(void);

/**
 * @brief Check if ON/Start button was clicked (EXTI-based, with debounce)
 * @return true if ON was pressed since last check
 */
bool encoder_start_clicked(void);

/**
 * @brief Check if MENU button was clicked (EXTI-based, with debounce)
 * @return true if MENU was pressed since last check
 */
bool encoder_menu_clicked(void);

/**
 * @brief Get E-Stop current state (EXTI-based, level-sensitive)
 * @return true if E-Stop is active (pressed)
 */
bool encoder_estop_active(void);

/**
 * @brief Check if E-Stop state changed since last check
 * @return true if state changed
 */
bool encoder_estop_changed(void);

/**
 * @brief Get Guard current state (EXTI-based, level-sensitive)
 * @return true if Guard is open
 */
bool encoder_guard_open(void);

/**
 * @brief Check if Guard state changed since last check
 * @return true if state changed
 */
bool encoder_guard_changed(void);

/**
 * @brief Get Foot Pedal current state (EXTI-based)
 * @return true if pedal is pressed
 */
bool encoder_pedal_pressed(void);

/**
 * @brief Check if Pedal state changed since last check
 * @return true if state changed
 */
bool encoder_pedal_changed(void);

#endif /* ENCODER_H */
