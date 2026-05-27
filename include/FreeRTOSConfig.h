/**
 * @file FreeRTOSConfig.h
 * @brief FreeRTOS Configuration for Nova Voyager
 *
 * Target: GD32F303RCT6 (Cortex-M4, 48KB RAM, 256KB Flash)
 * Clock: Set by USE_120MHZ in config.h (120MHz or 72MHz)
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "config.h"  // For SYSCLK_FREQ

/*===========================================================================*/
/* Hardware/Application Specific                                              */
/*===========================================================================*/

#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      SYSCLK_FREQ
#define configSYSTICK_CLOCK_HZ                  (configCPU_CLOCK_HZ / 8)  /* SysTick uses HCLK/8 by default */
#define configTICK_RATE_HZ                      1000
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                256  /* Increased from 128 to prevent IDLE stack overflow */
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configUSE_MUTEXES                       1
/* AUDIT FIX (HIGH, motor.c:120): motor_send_command() writes the shared
 * tx_buffer and drives USART3, and is called from the UI, console, tapping and
 * motor tasks. It now takes g_motor_mutex itself, but callers such as
 * task_tapping.c's state machine already hold that mutex to make a whole
 * sequence atomic — so the mutex has to be recursive or the first nested call
 * deadlocks the machine. */
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIME_SLICING                  1
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_MINI_LIST_ITEM                1
#define configSTACK_DEPTH_TYPE                  uint16_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t
#define configHEAP_CLEAR_MEMORY_ON_FREE         0

/*===========================================================================*/
/* Memory Allocation                                                          */
/*===========================================================================*/

// All tasks/queues/semaphores use static allocation for deterministic RAM usage
// Minimal heap kept for potential FreeRTOS internal use
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#ifdef BUILD_GAMES
#define configTOTAL_HEAP_SIZE                   4096
#else
#define configTOTAL_HEAP_SIZE                   512
#endif
#define configAPPLICATION_ALLOCATED_HEAP        0
#define configSTACK_ALLOCATION_FROM_SEPARATE_HEAP 0

/*===========================================================================*/
/* Hook Functions                                                             */
/*===========================================================================*/

#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     1
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
#define configUSE_SB_COMPLETED_CALLBACK         0

/*===========================================================================*/
/* Run Time Stats                                                             */
/*===========================================================================*/

#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/*===========================================================================*/
/* Co-routine Definitions                                                     */
/*===========================================================================*/

#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/*===========================================================================*/
/* Software Timer Definitions                                                 */
/*===========================================================================*/

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               3
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            configMINIMAL_STACK_SIZE

/*===========================================================================*/
/* Cortex-M Specific                                                          */
/*===========================================================================*/

#ifdef __NVIC_PRIO_BITS
    #define configPRIO_BITS __NVIC_PRIO_BITS
#else
    #define configPRIO_BITS 4
#endif

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5

#define configKERNEL_INTERRUPT_PRIORITY         (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/*===========================================================================*/
/* Optional Function Includes                                                 */
/*===========================================================================*/

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          0
/* REVIEW FIX: SELFTEST suspends task_ui to take the LCD, and game_launch()
 * does the same — so it must resume ONLY if it was the one that suspended, or
 * it hands the panel back to task_ui while a game is still drawing on the same
 * unlocked bus. Asking the scheduler is the honest test. */
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xEventGroupSetBitFromISR        1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_xTaskGetHandle                  0
#define INCLUDE_xTaskResumeFromISR              1

/*===========================================================================*/
/* Assert Definition                                                          */
/*===========================================================================*/

/* Both declared here because several translation units have historically
 * picked up uart_puts() from this header. uart_puts_raw() is the one
 * configASSERT may use — see below. */
extern void uart_puts(const char* s);
extern void uart_puts_raw(const char* s);

/* A failed assert stops the world. It must therefore do what every other
 * stop-the-world path in this firmware does FIRST: kill the spindle.
 *
 * REVIEW FIX (CRITICAL): this used to be
 *     uart_puts("ASSERT FAIL!"); taskDISABLE_INTERRUPTS(); for(;;);
 * which is unsafe twice over.
 *
 *  1. NO MOTOR CUTOFF, and then interrupts are masked. taskDISABLE_INTERRUPTS()
 *     raises BASEPRI to configMAX_SYSCALL_INTERRUPT_PRIORITY (5); the E-Stop
 *     EXTI0 runs at priority 6 and is therefore MASKED BY IT. So an assert while
 *     drilling left PD4 asserted, the spindle turning, and the operator's E-Stop
 *     physically unable to fire — for the full ~5 s until the IWDG reset. Every
 *     other hook (stack overflow, hard fault, malloc failure) drops PD4 first.
 *  2. uart_puts() takes g_uart_mutex with portMAX_DELAY when the scheduler is
 *     running. configASSERT fires from inside FreeRTOS internals — including
 *     PendSV and critical sections — where taking a mutex is illegal, and quite
 *     possibly while the interrupted task holds it. main.c:197 documents this
 *     exact trap and fixed the other hooks with uart_puts_raw().
 *
 * GPIOD->BSRR bit 20 is BR4: PD4 LOW, the same hardware cutoff the E-Stop and
 * guard ISRs perform. Written as a literal because this header is included
 * before any device header. */
#define configASSERT(x)                                                        \
    if ((x) == 0) {                                                            \
        (*(volatile unsigned long *)0x40011410UL) = (1UL << 20); /* GPIOD BSRR: PD4 LOW */ \
        uart_puts_raw("ASSERT FAIL!\r\n");                                     \
        taskDISABLE_INTERRUPTS();                                              \
        for (;;);                                                              \
    }

/*===========================================================================*/
/* FreeRTOS Interrupt Handlers (map to STM32 names)                           */
/*===========================================================================*/

#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
