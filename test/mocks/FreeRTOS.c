/**
 * @file FreeRTOS.c
 * @brief FreeRTOS mock implementations
 */

#include "FreeRTOS.h"

// Mock tick counter (shared with HAL_GetTick)
TickType_t mock_tick_count = 0;

// When non-zero, xTaskGetTickCount() auto-advances by this amount each call.
// Enables timeout tests without needing real time to pass.
TickType_t mock_tick_step = 0;
