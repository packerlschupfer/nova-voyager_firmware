/**
 * @file init.c
 * @brief System initialization implementation
 */

#include "init.h"
#include "config.h"
#include "shared.h"
#include "stm32f1xx_hal.h"
#include <string.h>

/* External dependencies */
extern shared_state_t g_state;

/*===========================================================================*/
/* Clock Configuration - from 8MHz HSE (USE_120MHZ in config.h)             */
/*===========================================================================*/

void clock_init(void) {
#if USE_120MHZ
    // GD32F303 at 120MHz
    // Set flash wait states (3 for 96-120MHz)
    FLASH->ACR = 0x33;  // 3 wait states + prefetch enable

    // Enable HSE (8MHz external crystal)
    RCC->CR |= (1 << 16);  // HSEON
    while (!(RCC->CR & (1 << 17)));  // Wait for HSERDY

    // Configure PLL: HSE * 15 = 120MHz, APB1 = /4 (30MHz), APB2 = /1 (120MHz)
    // PLLSRC=HSE (bit 16), PLLMUL=15 (bits 21:18 = 1101), PPRE1=/4 (bits 10:8 = 101)
    RCC->CFGR = (1 << 16) | (0xD << 18) | (5 << 8);

    // Enable PLL
    RCC->CR |= (1 << 24);  // PLLON
    while (!(RCC->CR & (1 << 25)));  // Wait for PLLRDY

    // Switch to PLL as system clock
    RCC->CFGR |= 0x02;  // SW = PLL
    while ((RCC->CFGR & 0x0C) != 0x08);  // Wait for SWS = PLL
#else
    // STM32-compatible 72MHz
    // Set flash wait states (2 for 48-72MHz)
    FLASH->ACR = 0x32;  // 2 wait states + prefetch enable

    // Enable HSE (8MHz external crystal)
    RCC->CR |= (1 << 16);  // HSEON
    while (!(RCC->CR & (1 << 17)));  // Wait for HSERDY

    // Configure PLL: HSE * 9 = 72MHz, APB1 = /2 (36MHz max)
    // PLLSRC=HSE (bit 16), PLLMUL=9 (bits 21:18 = 0111), PPRE1=/2 (bits 10:8 = 100)
    RCC->CFGR = (1 << 16) | (7 << 18) | (4 << 8);

    // Enable PLL
    RCC->CR |= (1 << 24);  // PLLON
    while (!(RCC->CR & (1 << 25)));  // Wait for PLLRDY

    // Switch to PLL as system clock
    RCC->CFGR |= 0x02;  // SW = PLL
    while ((RCC->CFGR & 0x0C) != 0x08);  // Wait for SWS = PLL
#endif
}

/*===========================================================================*/
/* UART Initialization                                                       */
/*===========================================================================*/

void uart_init(void) {
    // Same init as working code
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_IOPAEN;
    GPIOA->CRH &= ~(0xFF << 4);
    GPIOA->CRH |= (0xB << 4);    // PA9: AF push-pull 50MHz (TX)
    GPIOA->CRH |= (0x4 << 8);    // PA10: Floating input (RX)

    // Clock-aware baud rate: USART1 is on APB2
    // At PLL: use APB2_FREQ from config.h
    // At HSI (8MHz): BRR = 8000000/9600 = 833 = 0x341
    uint32_t sws = (RCC->CFGR >> 2) & 0x3;
    USART1->BRR = (sws == 0x02) ? (APB2_FREQ / 9600) : 0x341;

    // Enable TX, RX, RXNE interrupt, and USART
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    // Enable USART1 interrupt in NVIC (lower priority than FreeRTOS kernel)
    NVIC_SetPriority(USART1_IRQn, 6);
    NVIC_EnableIRQ(USART1_IRQn);
}

/*===========================================================================*/
/* Shared State Initialization                                               */
/*===========================================================================*/

void shared_init(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.state = APP_STATE_STARTUP;
    g_state.target_rpm = SPEED_DEFAULT_RPM;
    // 0 /* tap_mode removed */ removed;
}
