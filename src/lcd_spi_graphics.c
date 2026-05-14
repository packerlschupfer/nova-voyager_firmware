/**
 * @file lcd_spi_graphics.c
 * @brief Graphics via SPI2 (not parallel!)
 *
 * Based on finding: Original firmware uses SPI2 for graphics
 * SPI2_IRQHandler at 0x08005191 handles "LCD display updates"
 */

#include "stm32f1xx_hal.h"
#include "shared.h"

extern void uart_puts(const char* s);

/**
 * @brief Initialize SPI2 for LCD graphics
 */
void spi2_init_for_graphics(void) {
    uart_puts("Initializing SPI2 for graphics...\r\n");

    // Enable SPI2 clock
    RCC->APB1ENR |= (1 << 14);  // SPI2EN

    // Configure GPIO for SPI2
    // PB13 = SCK, PB15 = MOSI, PB12 = CS
    RCC->APB2ENR |= (1 << 3);  // GPIOB clock

    // PB13: AF push-pull (SPI2 SCK)
    GPIOB->CRH &= ~(0xF << 20);
    GPIOB->CRH |= (0xB << 20);  // AF push-pull 50MHz

    // PB15: AF push-pull (SPI2 MOSI)
    GPIOB->CRH &= ~(0xF << 28);
    GPIOB->CRH |= (0xB << 28);  // AF push-pull 50MHz

    // PB12: Output push-pull (CS)
    GPIOB->CRH &= ~(0xF << 16);
    GPIOB->CRH |= (0x3 << 16);  // Output 50MHz
    GPIOB->BSRR = (1 << 12);  // CS high (inactive)

    // Configure SPI2
    SPI2->CR1 = 0;  // Reset
    SPI2->CR1 = (1 << 2)   // MSTR (master mode)
              | (0x3 << 3) // BR=011 (APB1/16 = 30MHz/16 = 1.875MHz)
              | (1 << 8)   // SSI
              | (1 << 9);  // SSM (software CS)

    SPI2->CR1 |= (1 << 6);  // SPE (enable SPI2)

    uart_puts("SPI2 initialized: Master, ~2MHz, 8-bit\r\n");
}

/**
 * @brief Send byte via SPI2
 */
static void spi2_send(uint8_t data) {
    while (!(SPI2->SR & (1 << 1)));  // Wait TXE
    SPI2->DR = data;
    while (!(SPI2->SR & (1 << 1)));  // Wait TXE
    while (SPI2->SR & (1 << 7));     // Wait BSY clear
}

/**
 * @brief Test SPI2 graphics
 */
void cmd_testspi(void) {
    uart_puts("\r\n═══ SPI2 GRAPHICS TEST ═══\r\n\r\n");

    spi2_init_for_graphics();

    uart_puts("Sending test pattern via SPI2...\r\n");

    // CS low (select display)
    GPIOB->BRR = (1 << 12);

    // Send some test bytes
    for (int i = 0; i < 16; i++) {
        spi2_send(0xFF);
    }

    // CS high (deselect)
    GPIOB->BSRR = (1 << 12);

    uart_puts("SPI2 test complete\r\n");
    uart_puts("Check if ANYTHING changed on LCD\r\n");

    delay_ms(5000);

    uart_puts("═══ TEST COMPLETE ═══\r\n");
}
