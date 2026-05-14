/**
 * @file stm32f1xx_hal.c
 * @brief Mock HAL peripheral instances
 */

#include "stm32f1xx_hal.h"

// Mock peripheral instances
USART_TypeDef mock_USART1 = {0};
USART_TypeDef mock_USART3 = {0};
GPIO_TypeDef mock_GPIOA = {0};
GPIO_TypeDef mock_GPIOB = {0};
GPIO_TypeDef mock_GPIOC = {0};
GPIO_TypeDef mock_GPIOD = {0};
RCC_TypeDef mock_RCC = {0};
SCB_TypeDef mock_SCB = {0};
SysTick_TypeDef mock_SysTick = {0};
