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
#include "brownout.h"
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

// AUDIT FIX (MEDIUM, shared.h:380): NOLOAD-section magic word for
// cmd_coldboot. Deliberately not initialized here (that would emit a .data
// initializer that overwrites the value on every boot); startup does not
// touch this section either, so the value survives a soft reset.
volatile uint32_t g_force_cold_boot_magic __attribute__((section(".noinit"), used));

TaskHandle_t g_task_ui = NULL;
TaskHandle_t g_task_motor = NULL;
TaskHandle_t g_task_depth = NULL;
TaskHandle_t g_task_tapping = NULL;
TaskHandle_t g_task_main = NULL;

// Boot type detection (set early in main, used by all tasks)
boot_type_t g_boot_type = BOOT_COLD;

// Mutex ordering debug tracking (BUILD_DEBUG only)
#ifdef BUILD_DEBUG
volatile void* g_state_mutex_owner = NULL;   /* debug builds: STATE_LOCK owner */
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

        /* Remember the last-used spindle speed across a power cycle, as the
         * original firmware did. The encoder writes g_state.target_rpm and
         * nothing else, so without this, pressing OFF — which is wired to NRST
         * — reverted to whatever a menu edit last stored. The value comes
         * from settings_note_operator_speed(), called by the encoder,
         * favourite recall and the SPEED command; this only commits it once it
         * has settled. See include/speed_autosave.h. */
        settings_speed_autosave_poll(HAL_GetTick());

        // Process events from queue (EVENT_QUEUE_TIMEOUT_MS timeout)
        if (xQueueReceive(g_event_queue, &evt, pdMS_TO_TICKS(EVENT_QUEUE_TIMEOUT_MS)) == pdTRUE) {
            handle_event(evt);
        }
    }
}

/*===========================================================================*/
/* FreeRTOS Hooks                                                             */
/*===========================================================================*/

/* The ONLY writer of the HAL tick. There is no HAL_Init(), no HAL_InitTick()
 * and no SysTick_Config() anywhere in the tree — main() explicitly stops
 * SysTick at :498 — so uwTick does not move until vTaskStartScheduler() runs
 * and this hook starts firing. HAL_GetTick() therefore returns 0 for the whole
 * of main(), and HAL_Delay() would spin forever there. */
void vApplicationTickHook(void) {
    HAL_IncTick();
}

// AUDIT FIX (CRITICAL, main.c:181): fault hooks now drop PD4 (motor enable)
// FIRST — before any UART/crash-dump/mutex operation. HardFault has fixed
// priority -1 and pre-empts the EXTI0 E-Stop ISR, so if a fault happens
// mid-drilling the operator can slam E-Stop and it does nothing — the
// spindle keeps turning until the IWDG (~5s) resets. One bare register
// write here is the only pre-uart_puts action that's ISR-safe.
#define FAULT_HOOK_KILL_MOTOR() do { GPIOD->BSRR = (1U << 20); } while (0)

/* AUDIT FIX (HIGH, main.c:185): this used uart_puts(), which takes
 * g_uart_mutex with portMAX_DELAY whenever the scheduler is running
 * (serial_console.c:45). FreeRTOS calls this hook from vTaskSwitchContext(),
 * i.e. from inside PendSV — and the likeliest task to blow its stack is one
 * that was printing, so it is probably holding that very mutex. xSemaphoreTake
 * would then run xTaskPriorityInherit() and vTaskPlaceOnEventList() against
 * pxCurrentTCB from inside the context switch that is currently choosing the
 * next task. The hook then spins in for(;;) at PendSV priority, where SysTick
 * (same priority, so no preemption) can never run.
 *
 * This is exactly the trap documented for HardFault_Handler below, which was
 * converted to uart_puts_raw(); this hook and the malloc hook were missed.
 *
 * crash_dump_log() is safe from here: eeprom.c bit-bangs I2C with busy loops,
 * not HAL_GetTick timeouts. Its dump.timestamp does read HAL_GetTick(), which
 * is a plain variable read — the value is simply frozen at the tick when the
 * switch began, which is close enough for a crash record. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    FAULT_HOOK_KILL_MOTOR();
    (void)xTask;
    uart_puts_raw("\r\n!!! STACK OVERFLOW: ");
    uart_puts_raw(pcTaskName);
    uart_puts_raw(" !!!\r\n");

    // Log crash dump
    crash_dump_log(CRASH_TYPE_STACK_OVERFLOW, 0, 0, 0, pcTaskName);

    uart_puts_raw("Crash logged. System will reset via watchdog.\r\n");
    for (;;);  // Watchdog will reset in ~5s
}

// Print a 32-bit value as 8 hex digits.
static void fault_print_hex32(uint32_t v) {
    /* print_hex_byte() routes through uart_putc(), which calls FreeRTOS.
     * Not usable here — emit the nibbles directly. */
    static const char hexd[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        uart_putc_raw(hexd[(v >> (i * 4)) & 0xFu]);
    }
}

static void fault_print_reg(const char* name, uint32_t v) {
    uart_puts_raw(name);
    uart_puts_raw("=0x");
    fault_print_hex32(v);
    uart_puts_raw("\r\n");
}

/**
 * @brief Emit a fault report. Factored out of HardFault_Handler so the exact
 *        same code can be exercised from task context via the FAULTREPORT
 *        debug command — that separates "the reporting logic is broken" from
 *        "something about fault context is broken", which is otherwise a guess.
 */
void fault_report_body(uint32_t cfsr, uint32_t hfsr) {
    uart_puts_raw("\r\n!!! HARD FAULT !!!\r\n");
    fault_print_reg("  CFSR ", cfsr);
    fault_print_reg("  HFSR ", hfsr);

    if (cfsr & (1u << 15)) {            /* BFARVALID */
        fault_print_reg("  BFAR ", SCB->BFAR);
    } else {
        uart_puts_raw("  BFAR  not valid\r\n");
    }
    if (cfsr & (1u << 7)) {             /* MMARVALID */
        fault_print_reg("  MMFAR", SCB->MMFAR);
    }

    /* Name the bits, so a field report does not depend on the reader
     * remembering the CFSR layout. */
    if (cfsr & (1u << 8))  uart_puts_raw("  IBUSERR: instruction fetch bus error\r\n");
    if (cfsr & (1u << 9))  uart_puts_raw("  PRECISERR: data bus error, BFAR valid\r\n");
    if (cfsr & (1u << 10)) uart_puts_raw("  IMPRECISERR: buffered data bus error\r\n");
    if (cfsr & (1u << 16)) uart_puts_raw("  UNDEFINSTR: undefined instruction\r\n");
    if (cfsr & (1u << 17)) uart_puts_raw("  INVSTATE: invalid execution state\r\n");
    if (cfsr & (1u << 24)) uart_puts_raw("  UNALIGNED: unaligned access\r\n");

}

/**
 * @brief HardFault handler.
 *
 * Deliberately a plain C handler, NOT a naked wrapper that recovers the stacked
 * frame. A naked version was written and tested on hardware 2026-08-29: the
 * fault fired correctly (CFSR=PRECISERR, BFAR exact) but the CPU never reached
 * the C half, leaving the board hung with no report AND no watchdog reset —
 * strictly worse than resetting. The cause was not identified, so the mechanism
 * was dropped rather than shipped unproven. A stacked PC would be valuable; a
 * drill press that stops resetting on a fault is not an acceptable price.
 *
 * What this does report is everything that does not require the frame, all of
 * it verified on hardware:
 *   - CFSR/HFSR/BFAR, with the fault bits named
 *   - via uart_puts_raw(), because uart_puts() takes a FreeRTOS mutex and
 *     calling that from priority -1 does not return. The old handler used it
 *     and could therefore never report anything, which is the likeliest reason
 *     two real lockups produced no console output.
 *   - with one IWDG feed first, because the main loop has stopped feeding it
 *     and the watchdog was observed truncating the report after two lines.
 */
void HardFault_Handler(void) {
    FAULT_HOOK_KILL_MOTOR();

    /* Buy time to finish the report; we do NOT keep feeding, so the reset at
     * the end still happens — it just cannot pre-empt the diagnosis. */
    IWDG->KR = 0xAAAA;

    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;

    fault_report_body(cfsr, hfsr);    char task_name[16] = "UNKNOWN";
#ifdef BUILD_DEBUG
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    if (current_task) {
        const char* name = pcTaskGetName(current_task);
        if (name) {
            strncpy(task_name, name, sizeof(task_name) - 1);
        }
    }
#endif

    /* REVIEW FIX (MEDIUM): the call passed (type, 0, cfsr, hfsr, name) against
     * the signature (type, pc, lr, psr, name) — so pc was hardcoded 0, CFSR was
     * stored as lr, and hfsr went into a parameter crash_dump.c discards. Every
     * CRASHSHOW reported "PC: 0x00000000" and printed the CFSR under the LR
     * label. The comment here claimed the fields had been corrected; they had
     * not. CFSR and HFSR are already decoded live by fault_report_body() above,
     * so what the stored record actually needs is the faulting address.
     *
     * The exception frame holds it. FreeRTOS tasks run on the PSP, so for the
     * common case — a fault inside a task — the frame is at PSP with the
     * stacked LR at offset 5 and PC at offset 6. A fault taken inside another
     * handler stacks on the MSP instead and this would report that task's last
     * frame rather than the faulting one; distinguishing them needs EXC_RETURN
     * from LR at handler entry, which is not reachable from C after the
     * prologue. Naming the limitation is better than storing a zero. */
    uint32_t fault_pc = 0, fault_lr = 0;
    {
        const uint32_t psp = __get_PSP();
        /* REVIEW FIX: the bound admitted a PSP in the last words of SRAM while
         * the reads below reach psp+27 — a bus fault taken there, already
         * inside HardFault_Handler, escalates to lockup and loses BOTH the
         * console report and the EEPROM record. That is strictly worse than
         * the zero this replaced. Leave room for the whole basic frame and
         * require word alignment. */
        if (psp >= 0x20000000u && psp <= (0x2000C000u - 32u) && (psp & 3u) == 0u) {
            const uint32_t* frame = (const uint32_t*)psp;
            fault_lr = frame[5];
            fault_pc = frame[6];
        }
    }

    /* EEPROM write last: it is the slowest thing left and losing it must not
     * cost the console report. */
    IWDG->KR = 0xAAAA;
    uart_puts_raw("Logging to EEPROM, then watchdog reset (~5s)...\r\n");
    crash_dump_log(CRASH_TYPE_HARD_FAULT, fault_pc, fault_lr, 0, task_name);
    uart_puts_raw("Crash logged.\r\n");

    for (;;);  // Watchdog resets in ~5s
}

// FreeRTOS interrupt handlers
// The ARM_CM3 port defines these handlers with inline assembly
// We just need to make sure they're exported with the right names

/* Raw output for the same reason as the stack-overflow hook: this can be
 * reached from a task that already holds g_uart_mutex, and it never returns,
 * so a blocking take here is an unrecoverable hang instead of a crash report. */
void vApplicationMallocFailedHook(void) {
    FAULT_HOOK_KILL_MOTOR();
    uart_puts_raw("\r\n!!! MALLOC FAILED !!!\r\n");

    // Log crash dump
    // Use stack pointer as fallback if LR/PSR not available
    uint32_t lr = 0, psr = 0;
    #ifdef __ARM_ARCH
    __ASM volatile ("MOV %0, LR" : "=r" (lr));
    __ASM volatile ("MRS %0, xPSR" : "=r" (psr));
    #endif
    crash_dump_log(CRASH_TYPE_MALLOC_FAILED, 0, lr, psr, "HEAP");

    uart_puts_raw("Crash logged. System will reset via watchdog.\r\n");
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
        /* REVIEW FIX: "let normal detection happen next boot" is what the old
         * comment claimed, but leaving the flags set means the NEXT boot reads
         * the soft-reset flag THIS boot set — so an ordinary NRST power-cycle
         * after a forced cold boot reported SOFT BOOT and skipped the splash
         * and boot report. Clear them like every other path does. */
        RCC->CSR |= RCC_CSR_RMVF;
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
    // FreeRTOS also does this in vTaskStartScheduler(), but being explicit is
    // safer. REVIEW FIX (MEDIUM): it does NOT make HAL_Delay() usable here —
    // nothing increments uwTick before the scheduler starts (see
    // vApplicationTickHook). Use lcd_delay_ms(), the calibrated busy-wait, for
    // any delay on the boot path.
    __enable_irq();

    // Initialize clock to 72MHz (bootloader may leave us at 8MHz HSI)
    clock_init();

    /* Brown-out interlock, as early as possible: it only touches RCC/PWR/EXTI/
     * NVIC, and every millisecond before it is armed is a millisecond in which
     * a sagging rail could leave PD4 high with the core misbehaving. See
     * include/brownout.h — this exists because powering the machine down
     * produced a burst of watchdog resets, and the IWDG needs up to ~5 s to
     * notice while the motor enable line stays as it was. */
    brownout_init();

    // Detect boot type from RCC reset flags (must be before clearing flags)
    detect_boot_type();

    /* Captured before DBGMCU_CR is cleared below, and reported once the UART is
     * up. Reading it over SWD is useless — OpenOCD re-sets the register when it
     * attaches, so a debugger read always shows the frozen state it just
     * created. Having the firmware report the value it actually inherited is
     * the only way to tell "came up clean" from "came up with the watchdog
     * frozen by an earlier debug session". */
    uint32_t dbgmcu_at_boot = 0u;
    (void)dbgmcu_at_boot;

#ifndef BUILD_DEBUG
    // Clear DBGMCU_CR before starting the watchdog.
    //
    // Attaching a debugger sets DBG_IWDG_STOP (bit 8), which freezes the
    // independent watchdog whenever the core is halted — and DBGMCU_CR lives in
    // the debug power domain, so it SURVIVES system reset. A production board
    // that has ever been connected to an ST-Link can therefore be running with
    // its watchdog disabled, with nothing in the firmware aware of it.
    //
    // Found 2026-08-29: this board read DBGMCU_CR=0x307 with IWDG freeze set,
    // and it is why the HardFault handler appeared not to work — the board sat
    // in the fault forever instead of being reset. Same mechanism explains why
    // two earlier lockups never reset themselves.
    //
    // Debug builds deliberately leave it alone: freezing the watchdog while
    // single-stepping is exactly what a developer wants.
    dbgmcu_at_boot = *((volatile uint32_t *)0xE0042004u);
    *((volatile uint32_t *)0xE0042004u) = 0u;   // DBGMCU->CR
#endif

    // CRITICAL SAFETY: Initialize Independent Watchdog (5s timeout)
    // IWDG clock = 40kHz LSI, prescaler /256, reload 781 = ~5s
    // Allows time for cold boot splash + beeps (~2.5s) with safety margin
    IWDG->KR = 0x5555;      // Enable register access
    IWDG->PR = 6;           // Prescaler /256 (40000/256 = 156.25 Hz)
    IWDG->RLR = 781;        // ~5 second timeout (781 / 156.25 Hz = 5.0s)
    IWDG->KR = 0xAAAA;      // Reload
    IWDG->KR = 0xCCCC;      // Start watchdog

#if ENABLE_PRECISE_BUS_FAULTS
    // Before anything can fault: make data bus errors precise so BFAR names the
    // address. See config.h for the rationale and the cost.
    SCnSCB->ACTLR |= (1u << 1);   // DISDEFWBUF
#endif

    // Initialize UART (now at correct 72MHz clock)
    uart_init();

    // Boot type indicator
    uart_puts("\r\n\r\n");
    switch (g_boot_type) {
        case BOOT_COLD:     uart_puts("*** COLD BOOT (power on) ***\r\n"); break;
        /* SFTRSTF = SYSRESETREQ, i.e. NVIC_SystemReset() or a debugger's
         * software reset — st-flash and OpenOCD both produce this. It is NOT
         * the OFF button: OFF is wired to NRST (docs/MOTOR_PROTOCOL.md:583),
         * which sets PINRSTF and reports as PIN RESET below. The old label said
         * "(OFF button)" and was actively misleading during the 2026-08-29
         * lockup work, where which reset had occurred was load-bearing. */
        case BOOT_SOFT:     uart_puts("*** SOFT BOOT (software reset) ***\r\n"); break;
        case BOOT_WATCHDOG: uart_puts("*** WATCHDOG RESET ***\r\n"); break;
        case BOOT_PIN:      uart_puts("*** PIN RESET (NRST) ***\r\n"); break;
    }

    // Surface a clock fault immediately, before anything else can confuse the
    // picture. uart_init() picks the 8 MHz divisor when SYSCLK is not on the
    // PLL, so this is readable on the fallback path — that is the whole reason
    // refusing-and-reporting beats hanging in clock_init().
#ifndef BUILD_DEBUG
    /* Inherited-then-cleared, on one line. If the first value is non-zero this
     * board came up with its watchdog frozen by a previous debugger session —
     * DBGMCU_CR is in the debug power domain and survives SYSRESETREQ. */
    uart_puts("DBGMCU_CR at boot: 0x");
    {
        static const char hexd[] = "0123456789ABCDEF";
        for (int i = 7; i >= 0; i--) uart_putc(hexd[(dbgmcu_at_boot >> (i * 4)) & 0xFu]);
    }
    uart_puts((dbgmcu_at_boot & (1u << 8)) ? " (IWDG WAS FROZEN) -> cleared to 0x"
                                           : " (clean) -> 0x");
    {
        uint32_t now = *((volatile uint32_t *)0xE0042004u);
        static const char hexd[] = "0123456789ABCDEF";
        for (int i = 7; i >= 0; i--) uart_putc(hexd[(now >> (i * 4)) & 0xFu]);
    }
    uart_puts("\r\n");
#endif

    if (g_clock_fault) {
        uart_puts("\r\n*** CLOCK FAULT: HSE crystal did not start ***\r\n");
        uart_puts("Running on internal HSI at 8 MHz. Timing is unreliable.\r\n");
        uart_puts("Motor start is refused. Service the crystal.\r\n\r\n");
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
    /* Bus mutexes first: settings_init() reaches the EEPROM, and both locks
     * were previously created lazily on first use — a test-then-store race
     * that only boot ordering made unreachable. Create them before the first
     * user rather than relying on that ordering. */
    extern void lcd_lock_init(void);
    extern void eeprom_lock_init(void);
    lcd_lock_init();
    eeprom_lock_init();

    settings_init();  // May be slow if initializing defaults to flash

    // AUDIT FIX (HIGH, task_tapping.c:379): push persisted tapping fields
    // into tapping.c's runtime store. Without this, every trigger enable /
    // threshold / peck timing sits at the static zero-defaults after boot,
    // so tapping F2-armed cycles silently do nothing while the menu still
    // shows the (never-honored) persisted values as ON.
    settings_sync_to_tapping();
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

    /* Apply saved settings to shared state.
     *
     * REVIEW FIX (HIGH): depth_mode was restored but target_depth was NOT, and
     * g_state is static so it starts at 0. check_target_depth() returns early
     * on target == 0, so a saved depth auto-stop was armed in the UI and inert
     * in fact after every power cycle — the operator sets "stop at 20 mm",
     * saves, power-cycles, and the machine drills straight through. The value
     * does round-trip through EEPROM; nothing was reading it back out.
     *
     * REVIEW FIX (HIGH): target_rpm was also copied raw, so a default_rpm above
     * the operator's max_limit (reachable because settings.c accepts any stored
     * default_rpm in [500, 5500] regardless of the cap) survived boot. */
    const settings_t* s = settings_get();
    g_state.target_rpm = s->speed.default_rpm;
    if (g_state.target_rpm > s->speed.max_limit) {
        g_state.target_rpm = s->speed.max_limit;
    }
    g_state.depth_mode = s->depth.mode;
    g_state.target_depth = s->depth.target;

    // Create FreeRTOS objects (using static allocation)
    if (full_boot) uart_puts("Creating queues...\r\n");
    g_event_queue = xQueueCreateStatic(32, sizeof(event_type_t), ucEventQueueStorage, &xEventQueueBuffer);
    g_motor_cmd_queue = xQueueCreateStatic(16, sizeof(motor_cmd_t), ucMotorCmdQueueStorage, &xMotorCmdQueueBuffer);
    g_state_mutex = xSemaphoreCreateMutexStatic(&xStateMutexBuffer);
    /* Recursive: motor_send_command() takes this itself, and sequence-level
     * callers (task_tapping.c) hold it across several such calls. */
    g_motor_mutex = xSemaphoreCreateRecursiveMutexStatic(&xMotorMutexBuffer);
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
    display_init();       // Load CGRAM bar characters
    IWDG->KR = 0xAAAA;  // Refresh watchdog after LCD init (includes 300ms splash)

    // jam_init() had zero callers, so every field was BSS-zero at boot; unit
    // tests only passed because their mocks initialize jam state manually.
    //
    // REVIEW FIX (MEDIUM): the old comment here claimed this also fixed the
    // false first-poll JAM_COMM_TIMEOUT by giving last_response_time
    // "HAL_GetTick() ≈ boot completion". It does not — HAL_GetTick() is 0
    // until the scheduler starts (see vApplicationTickHook), so jam_init()
    // stores exactly the BSS-zero it was supposed to replace. What actually
    // prevents the false trip is jam_notify_response(), which the motor task
    // calls from every successful reply, including the idle polls that run
    // long before the first start. This call still matters for the rest of the
    // state — it just is not the comm-timeout fix.
    extern void jam_init(void);
    jam_init();

    // Show boot message based on boot type.
    // NOTE: HAL_Delay() does NOT work here — SysTick is stopped and the tick
    // hook has not started. lcd_delay_ms() is the busy-wait to use.
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
    // AUDIT FIX (HIGH, temperature.c:44): initialize the temperature ADC
    // path BEFORE the depth task's CONT+DMA setup, so the lazy first-read
    // init doesn't wipe CONT and DMA later. temperature_init clears CR2 to
    // reset the ADC; safe here because depth ADC hasn't been configured yet.
    extern void temperature_init(void);
    temperature_init();
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

    // Guard-open handling is now purely runtime: display_update()
    // renders the persistent "!! GUARD OPEN !!" screen whenever
    // g_state.guard_closed is false, and safety_can_start_motor() refuses
    // motor starts. Previously the boot path blocked here in a busy loop
    // until the guard closed, which duplicated the runtime warning AND
    // made encoder/menu unreachable while stuck — power-on with the guard
    // open is now benign: main screen renders with a big guard-warning
    // overlay, everything else keeps ticking.
    extern bool encoder_guard_open(void);
    if (encoder_guard_open()) uart_puts("Guard: OPEN (runtime warning will display)\r\n");
    else if (full_boot)       uart_puts("Guard: CLOSED (OK)\r\n");

    // Set initial state
    STATE_LOCK();
    g_state.state = APP_STATE_IDLE;
    g_state.guard_closed = !encoder_guard_open();
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
