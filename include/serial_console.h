/**
 * @file serial_console.h
 * @brief Serial console and UART communication
 *
 * Provides UART communication functions, command parsing, and debug console
 * Extracted from main.c for better code organization
 */

#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* UART Functions (Debug Console)                                            */
/*===========================================================================*/

/**
 * @brief Send single character over UART1
 * @param c Character to send
 */
void uart_putc(char c);

/**
 * @brief Send null-terminated string over UART1
 * @param s String to send
 */
void uart_puts(const char* s);

/**
 * @brief Non-blocking UART character receive
 * @return Character if available, -1 if buffer empty
 */
int uart_getc_nonblocking(void);

/*===========================================================================*/
/* Motor UART Functions (Direct Access for Debugging)                        */
/*===========================================================================*/

/**
 * @brief Send single byte over motor UART (USART3)
 * @param c Byte to send
 */
void motor_putc(uint8_t c);

/**
 * @brief Read byte from motor UART with timeout
 * @param timeout_loops Maximum polling loops to wait
 * @return Byte value if received, -1 if timeout
 */
int motor_getc_timeout(uint32_t timeout_loops);

/**
 * @brief Read response from motor UART into buffer
 * @param buf Buffer to store response
 * @param max_len Maximum buffer length
 * @return Number of bytes read
 */
int motor_read_resp(uint8_t* buf, int max_len);

/*===========================================================================*/
/* Utility Functions                                                          */
/*===========================================================================*/

/**
 * @brief Print byte as 2-digit hex over UART
 * @param b Byte to print
 */
void print_hex_byte(uint8_t b);

/**
 * @brief Print signed integer over UART
 * @param n Integer to print (supports negative)
 */
void print_num(int32_t n);

/**
 * @brief Get integer argument from command buffer
 * @param arg_start Starting position in command buffer
 * @return Parsed integer value
 */
int cmd_get_arg_int(int arg_start);

/**
 * @brief Access command buffer directly
 * @return Pointer to command buffer
 */
extern char* get_cmd_buf(void);

/**
 * @brief Get current command buffer index
 * @return Current position in command buffer
 */
extern uint8_t get_cmd_idx(void);

/**
 * @brief Set command buffer index
 * @param idx New position in command buffer
 */
extern void set_cmd_idx(uint8_t idx);

/*===========================================================================*/
/* Serial Command Processing                                                  */
/*===========================================================================*/

/**
 * @brief Process incoming serial commands
 *
 * Checks for complete command in buffer and dispatches to handler.
 * Call this periodically from main loop or task.
 */
void check_serial_commands(void);

#endif // SERIAL_CONSOLE_H
