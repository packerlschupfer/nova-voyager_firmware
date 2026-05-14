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

// Note: STATE_LOCK/STATE_UNLOCK macros are defined in shared.h

#endif // COMMANDS_INTERNAL_H
