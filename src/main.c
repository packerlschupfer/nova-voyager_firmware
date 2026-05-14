/**
 * @file main.c
 * @brief Nova Voyager FreeRTOS Firmware - Main Entry Point
 *
 * This file contains:
 * - Global FreeRTOS objects (queues, mutexes, task handles)
 * - Main task (event processing and serial console)
 * - FreeRTOS hooks (stack overflow, hard fault, malloc failed)
 * - Static memory allocation buffers
 * - main() entry point (hardware initialization and scheduler start)
 *
 * All other functionality has been extracted to separate modules:
 * - init.c: Clock, UART, and shared state initialization
 * - serial_console.c: UART communication and command parsing
 * - commands.c: Command handlers and command table
 * - events.c: Event handling and speed adjustment logic
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "shared.h"
#include "config.h"
#include "settings.h"
#include "tapping.h"
#include "encoder.h"
#include "motor.h"
#include "buzzer.h"
#include "lcd.h"
#include "display.h"
#include "init.h"
#include "serial_console.h"
#include "commands.h"
#include "events.h"
#include "crash_dump.h"
#include "diagnostics.h"  // Phase 7: System diagnostics
#include "stm32f1xx_hal.h"
#include <string.h>

/* Forward declarations */
extern void ui_enter_menu(void);
extern void ui_init_buttons(void);
extern void ui_scheduler_started(void);
extern void motor_task_init(void);
extern void depth_task_init(void);
extern void task_motor(void *pvParameters);
extern void task_depth(void *pvParameters);
extern void task_ui(void *pvParameters);
extern void task_tapping(void *pvParameters);

/*===========================================================================*/
/* Global FreeRTOS Objects                                                   */
/*===========================================================================*/

QueueHandle_t g_event_queue = NULL;
QueueHandle_t g_motor_cmd_queue = NULL;
SemaphoreHandle_t g_state_mutex = NULL;
SemaphoreHandle_t g_motor_mutex = NULL;
SemaphoreHandle_t g_uart_mutex = NULL;
shared_state_t g_state;

TaskHandle_t g_task_ui = NULL;
TaskHandle_t g_task_motor = NULL;
TaskHandle_t g_task_depth = NULL;
TaskHandle_t g_task_tapping = NULL;
TaskHandle_t g_task_main = NULL;

// Boot type detection (set early in main, used by all tasks)
boot_type_t g_boot_type = BOOT_COLD;

// Mutex ordering debug tracking (BUILD_DEBUG only)
#ifdef BUILD_DEBUG
volatile bool g_state_mutex_held = false;
#endif

/*===========================================================================*/
/* Main Task                                                                  */
/*===========================================================================*/

static void task_main(void *pvParameters) {
    (void)pvParameters;

    uart_puts("Main task running\r\n");
    uart_puts("> ");  // Initial prompt
    event_type_t evt;

    // Initialize heartbeats at boot
    HEARTBEAT_UPDATE_MAIN();
    HEARTBEAT_UPDATE_UI();
    HEARTBEAT_UPDATE_MOTOR();
    HEARTBEAT_UPDATE_DEPTH();
    HEARTBEAT_UPDATE_TAPPING();

    // Overflow tracking for logging and stall detection
    static uint16_t last_evt_overflows = 0;
    static uint16_t last_motor_overflows = 0;
    static uint8_t  overflow_stall_count = 0;
    static uint16_t last_stall_ev_ov = 0;

    for (;;) {
        // CRITICAL SAFETY: Update main task heartbeat
        HEARTBEAT_UPDATE_MAIN();

        // CRITICAL SAFETY: Only refresh watchdog if ALL tasks are alive
        // If any task is stuck, let watchdog reset the system
        bool tasks_alive = ALL_TASKS_ALIVE();

        // Stall detection: if event queue keeps overflowing the system is not draining events
        if (tasks_alive) {
            if (g_state.event_queue_overflows != last_stall_ev_ov) {
                overflow_stall_count++;
                last_stall_ev_ov = g_state.event_queue_overflows;
                if (overflow_stall_count >= 10) {
                    tasks_alive = false;  // Treat persistent overflow as stall
                }
            } else {
                overflow_stall_count = 0;
            }
        }

        if (tasks_alive) {
            IWDG->KR = 0xAAAA;  // Refresh watchdog
        } else {
            // At least one task is stuck - log which one(s) before reset
            uart_puts("\r\n[WATCHDOG] Task failure detected:\r\n");
            if (!HEARTBEAT_IS_ALIVE(g_state.heartbeat_main))    uart_puts("  MAIN stuck\r\n");
            if (!HEARTBEAT_IS_ALIVE(g_state.heartbeat_ui))      uart_puts("  UI stuck\r\n");
            if (!HEARTBEAT_IS_ALIVE(g_state.heartbeat_motor))   uart_puts("  MOTOR stuck\r\n");
            if (!HEARTBEAT_IS_ALIVE(g_state.heartbeat_depth))   uart_puts("  DEPTH stuck\r\n");
            if (!HEARTBEAT_IS_ALIVE(g_state.heartbeat_tapping)) uart_puts("  TAPPING stuck\r\n");
            if (overflow_stall_count >= 10)                     uart_puts("  EVENT QUEUE saturated (stall)\r\n");
            uart_puts("[WATCHDOG] System will reset in <3s...\r\n");
            // Don't refresh watchdog - let it reset the system
        }

        // Log queue overflow warnings
        uint16_t ev_ov  = g_state.event_queue_overflows;
        uint16_t mot_ov = g_state.motor_queue_overflows;
        if (ev_ov != last_evt_overflows) {
            uart_puts("[WARN] Event queue overflow - events dropped!\r\n");
            last_evt_overflows = ev_ov;
        }
        if (mot_ov != last_motor_overflows) {
            uart_puts("[WARN] Motor queue overflow - commands dropped!\r\n");
            last_motor_overflows = mot_ov;
        }

        // Check for serial commands (must poll frequently - no UART FIFO)
        check_serial_commands();

        // Process events from queue (EVENT_QUEUE_TIMEOUT_MS timeout)
        if (xQueueReceive(g_event_queue, &evt, pdMS_TO_TICKS(EVENT_QUEUE_TIMEOUT_MS)) == pdTRUE) {
            handle_event(evt);
        }
    }
}

/*===========================================================================*/
/* FreeRTOS Hooks                                                             */
/*===========================================================================*/

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    uart_puts("\r\n!!! STACK OVERFLOW: ");
    uart_puts(pcTaskName);
    uart_puts(" !!!\r\n");

    // Log crash dump
    crash_dump_log(CRASH_TYPE_STACK_OVERFLOW, 0, 0, 0, pcTaskName);

    uart_puts("Crash logged. System will reset via watchdog.\r\n");
    for (;;);  // Watchdog will reset in ~3s
}

void HardFault_Handler(void) {
    uart_puts("\r\n!!! HARD FAULT !!!\r\n");

    // Extract fault information (simplified - full handler would parse stack frame)
    uint32_t hfsr = SCB->HFSR;  // Hard fault status register
    uint32_t cfsr = SCB->CFSR;  // Configurable fault status register
    uint32_t lr = 0;

    // Try to determine current task (may not work if scheduler not running)
    char task_name[16] = "UNKNOWN";
#ifdef BUILD_DEBUG
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    if (current_task) {
        const char* name = pcTaskGetName(current_task);
        if (name) {
            strncpy(task_name, name, sizeof(task_name) - 1);
        }
    }
#endif

    // Log crash dump to EEPROM
    crash_dump_log(CRASH_TYPE_HARD_FAULT, cfsr, hfsr, lr, task_name);

    uart_puts("Crash logged. HFSR=0x");
    extern void print_hex_byte(uint8_t);
    for (int i = 3; i >= 0; i--) {
        print_hex_byte((hfsr >> (i * 8)) & 0xFF);
    }
    uart_puts("\r\n");

    uart_puts("System will reset via watchdog (~3s).\r\n");
    for (;;);  // Watchdog will reset in ~3s
}

// FreeRTOS interrupt handlers
// The ARM_CM3 port defines these handlers with inline assembly
// We just need to make sure they're exported with the right names

void vApplicationMallocFailedHook(void) {
    uart_puts("\r\n!!! MALLOC FAILED !!!\r\n");

    // Log crash dump
    // Use stack pointer as fallback if LR/PSR not available
    uint32_t lr = 0, psr = 0;
    #ifdef __ARM_ARCH
    __ASM volatile ("MOV %0, LR" : "=r" (lr));
    __ASM volatile ("MRS %0, xPSR" : "=r" (psr));
    #endif
    crash_dump_log(CRASH_TYPE_MALLOC_FAILED, 0, lr, psr, "HEAP");

    uart_puts("Crash logged. System will reset via watchdog.\r\n");
    for (;;);  // Watchdog will reset in ~3s
}

// Static memory for idle task
static StaticTask_t xIdleTaskTCB;
static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];

// Static memory for application tasks
// Stack sizes tuned per task requirements:
// - Main: 192 words - handles serial commands, MREAD with critical sections
// - Depth: 128 words - simple ADC polling
// - Motor: 128 words - serial polling and command processing
// - UI: 160 words - LCD drawing, menu system
// - Tapping: 160 words - state machine, motor control
// C4 fix: Increased stack sizes for safety margin
static StaticTask_t xMainTaskTCB;
static StackType_t uxMainTaskStack[256];    // Was 192, increased for MREAD
static StaticTask_t xDepthTaskTCB;
static StackType_t uxDepthTaskStack[STACK_SIZE_DEPTH];
static StaticTask_t xMotorTaskTCB;
static StackType_t uxMotorTaskStack[STACK_SIZE_MOTOR];
static StaticTask_t xUITaskTCB;
static StackType_t uxUITaskStack[STACK_SIZE_UI];
static StaticTask_t xTappingTaskTCB;
static StackType_t uxTappingTaskStack[STACK_SIZE_TAPPING];

// Static memory for queues
static StaticQueue_t xEventQueueBuffer;
static uint8_t ucEventQueueStorage[32 * sizeof(event_type_t)];  // Increased from 16 to 32
static StaticQueue_t xMotorCmdQueueBuffer;
static uint8_t ucMotorCmdQueueStorage[16 * sizeof(motor_cmd_t)];  // Increased from 8 to 16
static StaticSemaphore_t xStateMutexBuffer;
static StaticSemaphore_t xMotorMutexBuffer;
static StaticSemaphore_t xUartMutexBuffer;

void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    configSTACK_DEPTH_TYPE * puxIdleTaskStackSize ) {
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *puxIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

// Static memory for timer task
static StaticTask_t xTimerTaskTCB;
static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     configSTACK_DEPTH_TYPE * puxTimerTaskStackSize ) {
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *puxTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

/*===========================================================================*/
/* Boot Type Detection                                                        */
/*===========================================================================*/

// RCC_CSR register bits - use STM32 HAL definitions if available
#ifndef RCC_CSR_RMVF
#define RCC_CSR_RMVF        (1U << 24)
#endif
#ifndef RCC_CSR_PINRSTF
#define RCC_CSR_PINRSTF     (1U << 26)
#endif
#ifndef RCC_CSR_PORRSTF
#define RCC_CSR_PORRSTF     (1U << 27)
#endif
#ifndef RCC_CSR_SFTRSTF
#define RCC_CSR_SFTRSTF     (1U << 28)
#endif
#ifndef RCC_CSR_IWDGRSTF
#define RCC_CSR_IWDGRSTF    (1U << 29)
#endif
#ifndef RCC_CSR_WWDGRSTF
#define RCC_CSR_WWDGRSTF    (1U << 30)
#endif
#ifndef RCC_CSR_LPWRRSTF
#define RCC_CSR_LPWRRSTF    (1U << 31)
#endif

// Test command can force COLD BOOT behavior (survives soft reset in SRAM)
// Place safely after .bss, far from stack (which grows down from 0x2000C000)
// Phase 2.5: Boot magic constants now in shared.h

static void detect_boot_type(void) {
    // Check for test override FIRST (before RCC flags)
    if (*FORCE_COLD_BOOT_MAGIC_ADDR == FORCE_COLD_BOOT_MAGIC_VALUE) {
        g_boot_type = BOOT_COLD;
        *FORCE_COLD_BOOT_MAGIC_ADDR = 0;  // Clear flag after use
        // Don't clear RCC flags yet - let normal detection happen next boot
        return;
    }

    uint32_t csr = RCC->CSR;

    // Check flags in priority order
    if (csr & RCC_CSR_IWDGRSTF) {
        g_boot_type = BOOT_WATCHDOG;
    } else if (csr & RCC_CSR_WWDGRSTF) {
        g_boot_type = BOOT_WATCHDOG;
    } else if (csr & RCC_CSR_SFTRSTF) {
        g_boot_type = BOOT_SOFT;
    } else if (csr & RCC_CSR_PORRSTF) {
        g_boot_type = BOOT_COLD;
    } else if (csr & RCC_CSR_PINRSTF) {
        g_boot_type = BOOT_PIN;
    } else {
        // Unknown - treat as cold boot
        g_boot_type = BOOT_COLD;
    }

    // Clear all reset flags for next reset detection
    RCC->CSR |= RCC_CSR_RMVF;
}

/*===========================================================================*/
/* Main Entry Point                                                           */
/*===========================================================================*/

int main(void) {
    // CRITICAL: Disable SysTick before enabling IRQs — bootloader may have left
    // it running with VTOR still pointing at 0x08000000 (bootloader vectors).
    SysTick->CTRL = 0;

    // Set VTOR to application base before re-enabling IRQs.
    SCB->VTOR = 0x08003000;

    // Bootloader disables interrupts (PRIMASK=1) before jumping here.
    // Re-enable now that SysTick is stopped and VTOR is correct.
    // FreeRTOS also does this in vTaskStartScheduler(), but being explicit
    // is safer and allows HAL_Delay() to work before the scheduler starts.
    __enable_irq();

    // Initialize clock to 72MHz (bootloader may leave us at 8MHz HSI)
    clock_init();

    // Detect boot type from RCC reset flags (must be before clearing flags)
    detect_boot_type();

    // CRITICAL SAFETY: Initialize Independent Watchdog (5s timeout)
    // IWDG clock = 40kHz LSI, prescaler /256, reload 781 = ~5s
    // Allows time for cold boot splash + beeps (~2.5s) with safety margin
    IWDG->KR = 0x5555;      // Enable register access
    IWDG->PR = 6;           // Prescaler /256 (40000/256 = 156.25 Hz)
    IWDG->RLR = 781;        // ~5 second timeout (781 / 156.25 Hz = 5.0s)
    IWDG->KR = 0xAAAA;      // Reload
    IWDG->KR = 0xCCCC;      // Start watchdog

    // Initialize UART (now at correct 72MHz clock)
    uart_init();

    // Boot type indicator
    uart_puts("\r\n\r\n");
    switch (g_boot_type) {
        case BOOT_COLD:     uart_puts("*** COLD BOOT (power on) ***\r\n"); break;
        case BOOT_SOFT:     uart_puts("*** SOFT BOOT (OFF button) ***\r\n"); break;
        case BOOT_WATCHDOG: uart_puts("*** WATCHDOG RESET ***\r\n"); break;
        case BOOT_PIN:      uart_puts("*** PIN RESET (NRST) ***\r\n"); break;
    }

    // Cold boot: full splash and beeps
    // Soft boot: minimal output, fast startup
    bool full_boot = (g_boot_type == BOOT_COLD || g_boot_type == BOOT_WATCHDOG);

    if (full_boot) {
        uart_puts("=== Nova Voyager FreeRTOS Firmware ===\r\n");
        uart_puts(FW_VERSION_STRING "\r\n");
        uart_puts("Initializing...\r\n");
    }

    // Initialize buzzer
    buzzer_init();
    if (full_boot) {
        // buzzer_beep(BEEP_BOOT_STAGE);  // Disabled for fast boot  // Stage 1: Clock/UART ready
        IWDG->KR = 0xAAAA;  // Refresh watchdog after beep
    }

    // Initialize shared state
    shared_init();
    if (full_boot) uart_puts("Shared state: OK\r\n");

    // Initialize settings from storage
    if (full_boot) uart_puts("Init settings...\r\n");
    settings_init();  // May be slow if initializing defaults to flash
    IWDG->KR = 0xAAAA;  // Refresh after settings (can be slow!)
    if (full_boot) {
        uart_puts("Settings: ");
        uart_puts(settings_using_eeprom() ? "EEPROM\r\n" : "Flash\r\n");
    }

    // Check for crash dump (always - important for debugging)
    crash_dump_init();

    // Phase 7: Initialize diagnostics system
    diagnostics_init();
    if (full_boot) {
        uart_puts("Diagnostics: OK\r\n");
    }

    if (full_boot) {
        // buzzer_beep(BEEP_BOOT_STAGE);  // Disabled for fast boot  // Stage 2: State + Settings
        IWDG->KR = 0xAAAA;  // Refresh watchdog after beep
    }

    // Apply saved settings to shared state
    const settings_t* s = settings_get();
    g_state.target_rpm = s->speed.default_rpm;
    // 0 /* tap_mode removed */ removed; // TEMP: mode removed
    g_state.depth_mode = s->depth.mode;

    // Create FreeRTOS objects (using static allocation)
    if (full_boot) uart_puts("Creating queues...\r\n");
    g_event_queue = xQueueCreateStatic(32, sizeof(event_type_t), ucEventQueueStorage, &xEventQueueBuffer);
    g_motor_cmd_queue = xQueueCreateStatic(16, sizeof(motor_cmd_t), ucMotorCmdQueueStorage, &xMotorCmdQueueBuffer);
    g_state_mutex = xSemaphoreCreateMutexStatic(&xStateMutexBuffer);
    g_motor_mutex = xSemaphoreCreateMutexStatic(&xMotorMutexBuffer);
    g_uart_mutex = xSemaphoreCreateMutexStatic(&xUartMutexBuffer);

    if (!g_event_queue || !g_motor_cmd_queue || !g_state_mutex || !g_motor_mutex || !g_uart_mutex) {
        uart_puts("ERROR: Failed to create FreeRTOS objects!\r\n");
        buzzer_beep(BEEP_ERROR);
        for (;;);
    }
    if (full_boot) {
        uart_puts("Queues: OK\r\n");
        // buzzer_beep(BEEP_BOOT_STAGE);  // Disabled for fast boot  // Stage 3: FreeRTOS objects
        IWDG->KR = 0xAAAA;  // Refresh watchdog after beep
    }

    // Initialize LCD (show splash only on cold boot for fast soft boot)
    if (full_boot) uart_puts("Init LCD...\r\n");
    lcd_init(full_boot);  // Pass boot type to control splash display
    IWDG->KR = 0xAAAA;  // Refresh watchdog after LCD init (includes 300ms splash)

    // Show boot message based on boot type
    // NOTE: HAL_Delay() now works here — PRIMASK cleared and SysTick running.
    extern void lcd_delay_ms(uint32_t ms);  // Use busy-wait delay

    if (g_boot_type == BOOT_WATCHDOG) {
        display_boot_message("! WATCHDOG !", "Reset occurred");
        lcd_delay_ms(1000);  // Show warning for 1s
        IWDG->KR = 0xAAAA;  // Refresh after delay
        // buzzer_beep(BEEP_BOOT_STAGE);  // Disabled for fast boot  // Stage 4: Post-warning
    } else if (full_boot) {
        // Show version splash briefly during boot progress (no extra delay)
        display_boot_message("Nova Voyager", FW_VERSION_STRING);
        // buzzer_beep(BEEP_BOOT_STAGE);  // Disabled for fast boot  // Stage 4: Post-splash
    }
    // Soft boot: skip splash, go straight to UI

    // Initialize buttons
    if (full_boot) {
        uart_puts("Init buttons...\r\n");
        display_boot_message("Booting...", "Buttons...");
    }
    ui_init_buttons();
    IWDG->KR = 0xAAAA;  // Refresh watchdog

    // Initialize motor UART
    if (full_boot) {
        uart_puts("Init motor UART...\r\n");
        display_boot_message("Booting...", "Motor...");
        // buzzer_beep(BEEP_BOOT_STAGE);  // Disabled for fast boot  // Stage 5: Motor UART
    }
    motor_task_init();
    IWDG->KR = 0xAAAA;  // Refresh watchdog
    if (full_boot) uart_puts("Motor UART OK\r\n");

    // Initialize depth ADC
    if (full_boot) {
        uart_puts("Init depth ADC...\r\n");
        display_boot_message("Booting...", "Depth ADC...");
        // buzzer_beep(BEEP_BOOT_STAGE);  // Disabled for fast boot  // Stage 6: Depth ADC
    }
    depth_task_init();
    IWDG->KR = 0xAAAA;  // Refresh watchdog
    if (full_boot) uart_puts("Depth ADC OK\r\n");

    // Create tasks using static allocation
    if (full_boot) {
        uart_puts("Creating tasks (static)...\r\n");
        display_boot_message("Booting...", "Tasks...");
    } else {
        uart_puts("Creating tasks (static)...\r\n");
    }

    // All tasks use static allocation - deterministic RAM usage
    g_task_main = xTaskCreateStatic(task_main, "Main", 256, NULL, 1, uxMainTaskStack, &xMainTaskTCB);
    if (g_task_main == NULL) { uart_puts("Main FAIL!\r\n"); for (;;); }
    uart_puts("Main OK, ");

    g_task_depth = xTaskCreateStatic(task_depth, "Depth", STACK_SIZE_DEPTH, NULL, PRIORITY_DEPTH, uxDepthTaskStack, &xDepthTaskTCB);
    if (g_task_depth == NULL) { uart_puts("Depth FAIL!\r\n"); for (;;); }
    uart_puts("Depth OK, ");

    g_task_motor = xTaskCreateStatic(task_motor, "Motor", STACK_SIZE_MOTOR, NULL, PRIORITY_MOTOR, uxMotorTaskStack, &xMotorTaskTCB);
    if (g_task_motor == NULL) { uart_puts("Motor FAIL!\r\n"); for (;;); }
    uart_puts("Motor OK, ");

    g_task_ui = xTaskCreateStatic(task_ui, "UI", STACK_SIZE_UI, NULL, PRIORITY_UI, uxUITaskStack, &xUITaskTCB);
    if (g_task_ui == NULL) { uart_puts("UI FAIL!\r\n"); for (;;); }
    uart_puts("UI OK, ");

    g_task_tapping = xTaskCreateStatic(task_tapping, "Tap", STACK_SIZE_TAPPING, NULL, PRIORITY_TAPPING, uxTappingTaskStack, &xTappingTaskTCB);
    if (g_task_tapping == NULL) { uart_puts("Tap FAIL!\r\n"); for (;;); }
    uart_puts("Tap OK\r\n");

    if (full_boot) uart_puts("Tasks: OK\r\n");
    if (full_boot) {
        display_boot_message("Booting...", "Starting...");
        // buzzer_beep(BEEP_BOOT_STAGE);  // Disabled for fast boot  // Stage 7: Tasks created
        IWDG->KR = 0xAAAA;  // Refresh watchdog after beep
    }

    // CRITICAL SAFETY: Validate guard switch is CLOSED before allowing operation
    extern bool encoder_guard_open(void);
    bool guard_open = encoder_guard_open();
    if (guard_open) {
        uart_puts("WARNING: Guard switch is OPEN at boot!\r\n");
        display_boot_message("!! GUARD OPEN !!", "Close to start");
        buzzer_beep(BEEP_ERROR);
        // Wait for guard to close before continuing
        while (encoder_guard_open()) {
            IWDG->KR = 0xAAAA;  // Refresh watchdog while waiting
            for (volatile int i = 0; i < 100000; i++);  // Brief delay
        }
        uart_puts("Guard closed - continuing boot\r\n");
        buzzer_beep(BEEP_SUCCESS);
    } else if (full_boot) {
        uart_puts("Guard: CLOSED (OK)\r\n");
    }

    // Set initial state
    STATE_LOCK();
    g_state.state = APP_STATE_IDLE;
    g_state.guard_closed = !guard_open;
    STATE_UNLOCK();

    // Boot complete tone (only on full boot)
    if (full_boot) {
        // buzzer_beep(BEEP_BOOT_STAGE);  // Disabled for fast boot  // Stage 8: READY!
        IWDG->KR = 0xAAAA;  // Refresh watchdog after beep
    }

    uart_puts("Starting scheduler...\r\n");

    // Notify UI that scheduler is about to start (so it can use vTaskDelay)
    ui_scheduler_started();

    uart_puts("Calling vTaskStartScheduler...\r\n");
    // Start FreeRTOS scheduler
    vTaskStartScheduler();

    // Should never get here
    uart_puts("ERROR: Scheduler returned!\r\n");
    for (;;);

    return 0;
}
