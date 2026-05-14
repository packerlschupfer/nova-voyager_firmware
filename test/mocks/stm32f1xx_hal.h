/**
 * @file stm32f1xx_hal.h
 * @brief Mock STM32 HAL header for native unit tests
 */

#ifndef STM32F1XX_HAL_H
#define STM32F1XX_HAL_H

#include <stdint.h>
#include <stdbool.h>

// Mock HAL types
typedef struct {
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
} USART_TypeDef;

// Status register bits
#define USART_SR_TXE    (1 << 7)
#define USART_SR_TC     (1 << 6)
#define USART_SR_RXNE   (1 << 5)

// Control register bits
#define USART_CR1_TE    (1 << 3)
#define USART_CR1_RE    (1 << 2)
#define USART_CR1_UE    (1 << 13)
#define USART_CR3_DMAR  (1 << 6)

// Mock USART instances
extern USART_TypeDef mock_USART1;
extern USART_TypeDef mock_USART3;
#define USART1 (&mock_USART1)
#define USART3 (&mock_USART3)

// Mock GPIO
typedef struct {
    volatile uint32_t CRL;
    volatile uint32_t CRH;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t BRR;
} GPIO_TypeDef;

#define GPIO_PIN_0  (1 << 0)
#define GPIO_PIN_1  (1 << 1)
#define GPIO_PIN_2  (1 << 2)
// ... add more as needed

// Mock RCC
typedef struct {
    volatile uint32_t APB2ENR;
    volatile uint32_t APB1ENR;
    volatile uint32_t AHBENR;
} RCC_TypeDef;
extern RCC_TypeDef mock_RCC;
#define RCC (&mock_RCC)

#define RCC_APB2ENR_IOPBEN (1 << 3)
#define RCC_APB1ENR_USART3EN (1 << 18)
#define RCC_AHBENR_DMA1EN (1 << 0)

// Mock GPIO instances
extern GPIO_TypeDef mock_GPIOA, mock_GPIOB, mock_GPIOC, mock_GPIOD;
#define GPIOA (&mock_GPIOA)
#define GPIOB (&mock_GPIOB)
#define GPIOC (&mock_GPIOC)
#define GPIOD (&mock_GPIOD)

// Mock HAL functions (use FreeRTOS mock_tick_count)
extern uint32_t mock_tick_count;
static inline uint32_t HAL_GetTick(void) { return mock_tick_count; }
static inline void HAL_Delay(uint32_t ms) { mock_tick_count += ms; }

// Mock SCB for VTOR
typedef struct {
    volatile uint32_t VTOR;
} SCB_TypeDef;
extern SCB_TypeDef mock_SCB;
#define SCB (&mock_SCB)

// Mock SysTick
typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
} SysTick_TypeDef;
extern SysTick_TypeDef mock_SysTick;
#define SysTick (&mock_SysTick)

#endif /* STM32F1XX_HAL_H */
