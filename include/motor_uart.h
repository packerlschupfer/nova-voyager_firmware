/**
 * @file motor_uart.h
 * @brief USART3 Hardware Abstraction for Motor Controller Communication
 *
 * Provides byte-level and packet-level I/O operations for USART3.
 * All functions include timeout protection.
 */

#ifndef MOTOR_UART_H
#define MOTOR_UART_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*===========================================================================*/
/* Initialization                                                            */
/*===========================================================================*/

/**
 * @brief Initialize USART3 for motor controller communication
 *
 * Configures USART3 @ 9600 baud on PB10 (TX) and PB11 (RX).
 * Optionally enables DMA1_Channel3 for RX if USE_USART3_DMA=1.
 */
void motor_uart_init(void);

/*===========================================================================*/
/* Byte-Level I/O                                                            */
/*===========================================================================*/

/**
 * @brief Send single byte via USART3 with timeout
 *
 * Waits for TXE flag with timeout protection (MOTOR_UART_BYTE_TIMEOUT_MS).
 * Typically called by motor_uart_send_packet() for packet transmission.
 *
 * @param byte Byte to send
 * @return true if sent successfully, false on timeout
 *
 * @note On timeout, logs error and updates diagnostics counter
 */
bool motor_uart_send_byte(uint8_t byte);

/**
 * @brief Check if RX data available
 * @return true if at least one byte can be read
 */
bool motor_uart_rx_available(void);

/**
 * @brief Read single byte from RX buffer
 *
 * Waits for data with timeout (MOTOR_UART_RX_TIMEOUT_MS in DMA mode).
 * Returns immediately if data available.
 *
 * @return Received byte, or 0x00 on timeout
 *
 * @note DMA mode: Reads from circular buffer
 * @note Polling mode: Reads directly from USART3->DR
 */
uint8_t motor_uart_read_byte(void);

/**
 * @brief Flush/discard all pending RX data
 */
void motor_uart_flush_rx(void);

/*===========================================================================*/
/* Packet-Level I/O                                                          */
/*===========================================================================*/

/**
 * @brief Send packet via USART3 with timeout protection
 *
 * Sends multiple bytes sequentially, aborting on first timeout.
 * Used by motor protocol layer for command/query transmission.
 *
 * @param packet Packet buffer (typically from protocol_build_*)
 * @param len Packet length in bytes
 * @return true if all bytes sent, false on timeout
 *
 * @note Timeout per byte is MOTOR_UART_BYTE_TIMEOUT_MS
 * @note On failure, partial packet may have been transmitted
 */
bool motor_uart_send_packet(const uint8_t* packet, size_t len);

#endif // MOTOR_UART_H
