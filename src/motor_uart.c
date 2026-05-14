/**
 * @file motor_uart.c
 * @brief USART3 Hardware Abstraction Layer
 *
 * MODULE: Motor UART Hardware Layer
 * LAYER: Hardware (lowest level)
 * THREAD SAFETY: Called from motor task only (no mutex needed)
 *
 * Provides byte-level and packet-level I/O for USART3:
 * - Hardware initialization (9600 baud, 8N1)
 * - Byte TX/RX with timeout protection
 * - Optional DMA circular buffer (currently disabled)
 * - Packet transmission helper
 *
 * All operations include timeout protection for robustness.
 * Used by: task_motor.c, motor.c (via motor_uart_send_packet)
 *
 * Hardware: USART3 on PB10 (TX), PB11 (RX)
 * Baud: 9600
 * Protocol: 8N1
 */

#include "motor_uart.h"
#include "config.h"
#include "diagnostics.h"
#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

// Enable DMA for USART3 RX (reduces CPU usage ~5-10%)
#define USE_USART3_DMA  0  // Disabled - causes cold boot watchdog reset

/*===========================================================================*/
/* USART3 Hardware                                                           */
/*===========================================================================*/

#define MOTOR_USART     USART3

/*===========================================================================*/
/* DMA Configuration (if enabled)                                            */
/*===========================================================================*/

#if USE_USART3_DMA
#define DMA1_CH3_BASE   (DMA1_BASE + 0x08 + 0x14*2)  // Channel 3 offset
#define DMA1_CH3_CCR    (*(volatile uint32_t*)(DMA1_CH3_BASE + 0x00))
#define DMA1_CH3_CNDTR  (*(volatile uint32_t*)(DMA1_CH3_BASE + 0x04))
#define DMA1_CH3_CPAR   (*(volatile uint32_t*)(DMA1_CH3_BASE + 0x08))
#define DMA1_CH3_CMAR   (*(volatile uint32_t*)(DMA1_CH3_BASE + 0x0C))
#define DMA1_IFCR       (*(volatile uint32_t*)(DMA1_BASE + 0x04))

// DMA circular buffer for USART3 RX
#define USART3_DMA_BUFFER_SIZE  64
static volatile uint8_t usart3_dma_buffer[USART3_DMA_BUFFER_SIZE];
static volatile size_t dma_read_pos = 0;

// Get current DMA write position
static inline size_t dma_get_write_pos(void) {
    return USART3_DMA_BUFFER_SIZE - DMA1_CH3_CNDTR;
}

// Get number of bytes available in DMA buffer
static size_t dma_bytes_available(void) {
    size_t write_pos = dma_get_write_pos();
    if (write_pos >= dma_read_pos) {
        return write_pos - dma_read_pos;
    } else {
        return USART3_DMA_BUFFER_SIZE - dma_read_pos + write_pos;
    }
}
#endif

/*===========================================================================*/
/* Initialization                                                            */
/*===========================================================================*/

void motor_uart_init(void) {
    // Enable clocks
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;

    // PB10 = TX (AF push-pull), PB11 = RX (input floating)
    GPIOB->CRH &= ~(0xFF << 8);
    GPIOB->CRH |= (0x4B << 8);  // PB10: AF PP 50MHz, PB11: Input floating

    // Configure USART3: 9600 baud, 8N1
    // APB1 clock: 30MHz at 120MHz sysclk, 36MHz at 72MHz sysclk
    MOTOR_USART->BRR = APB1_FREQ / 9600;

#if USE_USART3_DMA
    // Enable DMA1 clock
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;

    // First enable USART without DMA
    MOTOR_USART->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;

    // Clear any pending USART RX data
    (void)MOTOR_USART->SR;
    (void)MOTOR_USART->DR;

    // Configure DMA1_Channel3 for USART3 RX
    DMA1_CH3_CCR = 0;  // Disable channel first

    // Clear DMA channel 3 flags (bits 8-11 for channel 3)
    DMA1_IFCR = (0xF << 8);

    DMA1_CH3_CPAR = (uint32_t)&MOTOR_USART->DR;  // Peripheral address
    DMA1_CH3_CMAR = (uint32_t)usart3_dma_buffer;  // Memory address
    DMA1_CH3_CNDTR = USART3_DMA_BUFFER_SIZE;  // Number of transfers

    // CCR: CIRC=1, MINC=1, PSIZE=00 (8-bit), MSIZE=00 (8-bit), PL=01
    DMA1_CH3_CCR = (1 << 5) |   // CIRC - circular mode
                   (1 << 7) |   // MINC - memory increment
                   (1 << 12);   // PL[0] - medium priority

    dma_read_pos = 0;  // Initialize read position

    // Enable DMA channel
    DMA1_CH3_CCR |= (1 << 0);  // EN - enable DMA channel

    // Now enable USART DMA receiver
    MOTOR_USART->CR3 = USART_CR3_DMAR;  // Enable DMA for RX
#else
    MOTOR_USART->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
#endif
}

/*===========================================================================*/
/* Byte-Level I/O                                                            */
/*===========================================================================*/

bool motor_uart_send_byte(uint8_t b) {
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(MOTOR_UART_BYTE_TIMEOUT_MS);

    while (!(MOTOR_USART->SR & USART_SR_TXE)) {
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            extern void uart_puts(const char* s);
            uart_puts("[MOTOR_UART] TX timeout\r\n");
            diagnostics_uart_tx_timeout();
            return false;  // Timeout
        }
    }
    MOTOR_USART->DR = b;
    diagnostics_uart_tx_bytes(1);
    return true;  // Success
}

#if USE_USART3_DMA

bool motor_uart_rx_available(void) {
    return dma_bytes_available() > 0;
}

uint8_t motor_uart_read_byte(void) {
    // Wait until data available with timeout protection
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(MOTOR_UART_RX_TIMEOUT_MS);

    while (!motor_uart_rx_available()) {
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            extern void uart_puts(const char* s);
            uart_puts("[MOTOR_UART] RX timeout\r\n");
            diagnostics_uart_rx_timeout();
            return 0;  // Timeout - return dummy byte
        }
    }

    uint8_t byte = usart3_dma_buffer[dma_read_pos];
    dma_read_pos = (dma_read_pos + 1) % USART3_DMA_BUFFER_SIZE;
    diagnostics_uart_rx_bytes(1);
    return byte;
}

void motor_uart_flush_rx(void) {
    // Move read position to write position (discard all buffered data)
    dma_read_pos = dma_get_write_pos();
}

#else

bool motor_uart_rx_available(void) {
    return (MOTOR_USART->SR & USART_SR_RXNE) != 0;
}

uint8_t motor_uart_read_byte(void) {
    return MOTOR_USART->DR & 0xFF;
}

void motor_uart_flush_rx(void) {
    while (MOTOR_USART->SR & USART_SR_RXNE) {
        (void)MOTOR_USART->DR;  // Read and discard
    }
}

#endif

/*===========================================================================*/
/* Packet-Level I/O                                                          */
/*===========================================================================*/

bool motor_uart_send_packet(const uint8_t* packet, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!motor_uart_send_byte(packet[i])) {
            return false;  // Timeout on one of the bytes
        }
    }
    return true;  // All bytes sent successfully
}
