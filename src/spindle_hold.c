/**
 * @file spindle_hold.c
 * @brief Spindle Hold Module Implementation
 *
 * Phase 2.1: Extracted from task_motor.c (lines 549-664)
 */

#include "spindle_hold.h"
#include "config.h"
#include "motor.h"
#include "shared.h"  // Phase 3.2: For delay_ms() helper
#include "FreeRTOS.h"
#include "task.h"

// External UART functions for logging
extern void uart_puts(const char* s);
extern void uart_putc(char c);

/*===========================================================================*/
/* Module State (Phase 5.2: Thread-safety classified)                        */
/*===========================================================================*/

// [MODULE_LOCAL] Only accessed from motor task via public API
// No mutex needed - all calls from single task context
static bool spindle_hold_active = false;
static bool spindle_hold_is_safety_mode = false;  // true if safety hold (has timeout)
static TickType_t spindle_hold_start_time = 0;
static TickType_t spindle_hold_last_maintain = 0;
static uint8_t spindle_hold_cl_percent = HOLD_CL_PERCENT;  // CL used by maintain()

/*===========================================================================*/
/* Private Functions                                                          */
/*===========================================================================*/

/**
 * @brief Initialize and enable spindle hold with specified current limit
 * @param cl_percent Current limit percentage (10% for manual, 12% for safety)
 */
static void spindle_hold_start_with_cl(uint8_t cl_percent) {
    if (spindle_hold_active) return;

    uart_puts("Spindle hold: starting (CL=");
    char buf[4];
    int i = 0;
    uint8_t v = cl_percent;
    do { buf[i++] = '0' + (v % 10); v /= 10; } while (v && i < 3);
    while (i > 0) uart_putc(buf[--i]);
    uart_puts("%)\r\n");

    // Step 1: Initialize - set all voltage params to off
    motor_send_command(CMD_VR, HOLD_VR_OFF);
    delay_ms(5);
    motor_send_command(CMD_CURRENT_LIMIT, 0);
    delay_ms(5);
    motor_send_command(CMD_VS, HOLD_VS_OFF);
    delay_ms(5);

    // Step 2: Set voltage parameters
    motor_send_command(CMD_V8, HOLD_V8_PARAM);
    delay_ms(5);
    motor_send_command(CMD_VG, HOLD_VG_PARAM);
    delay_ms(5);

    // Step 3: Enable hold - ramp to 100%, low current limit, voltage on
    motor_send_command(CMD_VR, HOLD_VR_FULL);
    delay_ms(5);
    motor_send_command(CMD_CURRENT_LIMIT, cl_percent);
    delay_ms(5);
    motor_send_command(CMD_VS, HOLD_VS_ON);
    delay_ms(5);

    spindle_hold_active = true;
    spindle_hold_cl_percent = cl_percent;  // Remember for maintain()
    uart_puts("Spindle hold: active\r\n");
}

/**
 * @brief Maintain spindle hold by repeating command sequence
 * Call periodically (every 460ms) to keep hold active
 */
static void spindle_hold_maintain(void) {
    if (!spindle_hold_active) return;

    // Repeat the hold sequence to maintain position. Use the CL value
    // requested at start (safety hold uses 12%, manual uses 10%).
    motor_send_command(CMD_VR, HOLD_VR_FULL);
    delay_ms(2);
    motor_send_command(CMD_CURRENT_LIMIT, spindle_hold_cl_percent);
    delay_ms(2);
    motor_send_command(CMD_VS, HOLD_VS_ON);
    delay_ms(2);
}

/*===========================================================================*/
/* Public API Implementation                                                  */
/*===========================================================================*/

void spindle_hold_start(bool is_safety) {
    if (is_safety) {
        // Safety hold: CL=12%, has timeout
        spindle_hold_start_with_cl(HOLD_CL_SAFETY);
        spindle_hold_is_safety_mode = true;
        spindle_hold_start_time = xTaskGetTickCount();
        spindle_hold_last_maintain = spindle_hold_start_time;
    } else {
        // Manual hold: CL=10%, no timeout
        spindle_hold_start_with_cl(HOLD_CL_PERCENT);
        spindle_hold_is_safety_mode = false;
        spindle_hold_last_maintain = xTaskGetTickCount();
    }
}

void spindle_hold_release(void) {
    if (!spindle_hold_active) return;

    uart_puts("Spindle hold: releasing\r\n");

    // Simple release: just RS=0 (stop command)
    // Original firmware only sends single RS=0 to release hold
    motor_send_command(CMD_STOP, 0);
    delay_ms(10);

    spindle_hold_active = false;
    spindle_hold_is_safety_mode = false;
    uart_puts("Spindle hold: released\r\n");
}

void spindle_hold_update(void) {
    if (!spindle_hold_active) return;

    TickType_t now = xTaskGetTickCount();

    // Check for safety hold timeout
    if (spindle_hold_is_safety_mode) {
        uint32_t elapsed_ms = (now - spindle_hold_start_time) * portTICK_PERIOD_MS;
        if (elapsed_ms >= SAFETY_HOLD_TIMEOUT_MS) {
            uart_puts("Safety hold: timeout - auto-releasing\r\n");
            spindle_hold_release();
            return;
        }
    }

    // Maintain hold at regular intervals
    uint32_t since_last = (now - spindle_hold_last_maintain) * portTICK_PERIOD_MS;
    if (since_last >= HOLD_MAINTAIN_MS) {
        spindle_hold_maintain();
        spindle_hold_last_maintain = now;
    }
}

bool spindle_hold_is_active(void) {
    return spindle_hold_active;
}
