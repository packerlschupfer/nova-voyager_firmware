/**
 * @file commands_internal.h
 * @brief Internal shared declarations for command modules
 */

#ifndef COMMANDS_INTERNAL_H
#define COMMANDS_INTERNAL_H

#include "commands.h"
#include "serial_console.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "shared.h"
#include "config.h"
#include "settings.h"
#include "motor.h"
#include "stm32f1xx_hal.h"
#include "encoder.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

/*===========================================================================*/
/* External Dependencies (from main.c)                                       */
/*===========================================================================*/

extern QueueHandle_t g_event_queue;
extern QueueHandle_t g_motor_cmd_queue;
extern SemaphoreHandle_t g_state_mutex;
extern shared_state_t g_state;
extern TaskHandle_t g_task_main;
extern TaskHandle_t g_task_ui;
extern TaskHandle_t g_task_motor;
extern TaskHandle_t g_task_depth;
extern TaskHandle_t g_task_tapping;

/*===========================================================================*/
/* Command Buffer Access                                                     */
/*===========================================================================*/

char* get_cmd_buf(void);
uint8_t get_cmd_idx(void);
void set_cmd_idx(uint8_t idx);

/*===========================================================================*/
/* Shared Helper Functions                                                   */
/*===========================================================================*/

// Command matching (defined in commands.c)
bool cmd_match(const char* prefix);
bool cmd_is(const char* cmd);
int cmd_get_arg_int(int arg_start);

// Motor UART helpers (defined in commands_motor.c)
void motor_putc(uint8_t c);
int motor_read_resp(uint8_t* buf, int max_len);
void motor_test_gf(void);
void motor_test_qq(uint8_t cmd_h, uint8_t cmd_l);
void motor_test_rs(void);

/*===========================================================================*/
/* Long-running command pump                                                 */
/*===========================================================================*/

/**
 * @brief Keep the machine alive during a long console command, and report
 *        whether that command should abort.
 *
 * Console handlers run inside task_main's loop (main.c: check_serial_commands),
 * so a handler that blocks also blocks the loop that refreshes the IWDG and
 * drains g_event_queue. Both ways of coping with that were wrong:
 *
 *   - a blind `IWDG->KR = 0xAAAA` inside the handler keeps the machine alive
 *     but defeats main.c's ALL_TASKS_ALIVE() gate (a genuinely hung UI/motor
 *     task no longer resets), and leaves STOP/guard/E-Stop events sitting
 *     unread in g_event_queue for the whole command;
 *   - feeding nothing at all watchdog-resets the machine mid-command, because
 *     main.c's refresh is exactly what is blocked.
 *
 * console_pump() does what the main loop does: refresh the main heartbeat,
 * refresh the IWDG *only* while every task is alive, and report an abort as
 * soon as a UI event is queued or the operator asks to stop. task_main runs at
 * priority 1 — below UI/depth/tapping/motor — so the other tasks keep
 * heartbeating even while a handler busy-waits, and the gate stays true.
 *
 * The manual abort is ESC or Ctrl-C specifically, checked by *peeking* at the
 * RX ring. "Any key" would be wrong here: process_serial_char() dispatches the
 * handler from inside check_serial_commands()'s drain loop, so while a handler
 * runs the ring legitimately holds the bytes that follow the command — the LF
 * of a CRLF terminator, or the next line from a scripted driver that sent
 * several at once. Treating those as a keypress aborts the command instantly
 * and never runs it; consuming them corrupts the command that follows.
 * Non-abort bytes are therefore left in the ring for check_serial_commands()
 * to process normally once the handler returns.
 *
 * Call once per iteration of any command loop that runs longer than ~1 s, and
 * abort when it returns true.
 *
 * @return true if the caller should stop its loop.
 */
#define CONSOLE_ABORT_ESC   0x1B
#define CONSOLE_ABORT_CTRLC 0x03

static inline bool console_pump(void) {
    HEARTBEAT_UPDATE_MAIN();
    if (ALL_TASKS_ALIVE()) {
        IWDG->KR = 0xAAAA;
    }
    if (uxQueueMessagesWaiting(g_event_queue) > 0) {
        return true;   // safety/UI event waiting - give the main loop the CPU
    }
    int c = uart_peek_nonblocking();
    if (c == CONSOLE_ABORT_ESC || c == CONSOLE_ABORT_CTRLC) {
        (void)uart_getc_nonblocking();   // consume the abort key only
        return true;
    }
    return false;
}

// Note: STATE_LOCK/STATE_UNLOCK macros are defined in shared.h

#endif // COMMANDS_INTERNAL_H
