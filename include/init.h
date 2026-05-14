/**
 * @file init.h
 * @brief System initialization functions
 *
 * Handles clock configuration, UART setup, and shared state initialization
 * Extracted from main.c for better code organization
 */

#ifndef INIT_H
#define INIT_H

#include <stdint.h>

/**
 * @brief Initialize system clock
 *
 * Configures the system clock from 8MHz HSE:
 * - 120MHz with PLL (if USE_120MHZ=1 in config.h)
 * - 72MHz with PLL (if USE_120MHZ=0)
 *
 * Sets appropriate flash wait states and bus prescalers.
 */
void clock_init(void);

/**
 * @brief Initialize UART1 for debug console
 *
 * Configures USART1 at 9600 baud with interrupt-driven RX:
 * - PA9: TX (AF push-pull)
 * - PA10: RX (floating input)
 * - RXNE interrupt enabled
 *
 * Automatically adjusts baud rate based on current clock.
 */
void uart_init(void);

/**
 * @brief Initialize shared state structure
 *
 * Zeros the global g_state structure and sets defaults:
 * - state = APP_STATE_STARTUP
 * - target_rpm = SPEED_DEFAULT_RPM
 */
void shared_init(void);

#endif // INIT_H
