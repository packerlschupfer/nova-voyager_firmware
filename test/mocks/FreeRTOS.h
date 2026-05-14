/**
 * @file FreeRTOS.h
 * @brief Mock FreeRTOS header for native unit tests
 */

#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>
#include <stdbool.h>

// Mock FreeRTOS types
typedef void* TaskHandle_t;
typedef void* QueueHandle_t;
typedef void* SemaphoreHandle_t;
typedef uint32_t TickType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portMAX_DELAY 0xFFFFFFFF

// Mock critical section macros (no-op for tests)
#define taskENTER_CRITICAL()
#define taskEXIT_CRITICAL()
#define taskDISABLE_INTERRUPTS()
#define taskENABLE_INTERRUPTS()

// Mock delay
#define pdMS_TO_TICKS(ms) (ms)
static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }

// Mock tick count
extern TickType_t mock_tick_count;
// When non-zero, each call to xTaskGetTickCount() advances time by this amount
extern TickType_t mock_tick_step;
static inline TickType_t xTaskGetTickCount(void) {
    mock_tick_count += mock_tick_step;
    return mock_tick_count;
}

#endif /* FREERTOS_H */
