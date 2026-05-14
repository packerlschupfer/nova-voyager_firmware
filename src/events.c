/**
 * @file events.c
 * @brief Event handling implementation
 *
 * Handles all UI events (buttons, encoder, motor faults) and updates system state
 */

#include "events.h"
#include "config.h"
#include "shared.h"
#include "settings.h"
#include "tapping.h"
#include "motor.h"
#include "buzzer.h"
#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdbool.h>

/* External dependencies from serial_console.h (will be created) */
extern void uart_puts(const char* s);
extern void uart_putc(char c);
extern void print_num(int32_t n);

/* External dependencies from other modules */
extern void depth_calibrate_now(void);
extern void ui_enter_menu(void);
extern void ui_menu_rotate(int8_t delta);
extern void ui_menu_click(void);
extern bool encoder_estop_active(void);
extern uint16_t motor_get_temperature(void);

/* Speed command rate limiting - prevent MCB command overflow */
#define SPEED_UPDATE_INTERVAL_MS  100  // Max 10 speed updates/second
static TickType_t last_speed_cmd_tick = 0;

/* External global state */
extern shared_state_t g_state;

/*===========================================================================*/
/* Speed Adjustment Helper                                                   */
/*===========================================================================*/

// Variable speed steps for better UX at different RPM ranges
// Fine = encoder, Coarse = F1 button
uint16_t get_speed_step(uint16_t rpm, bool coarse) {
    if (rpm < 200)       return coarse ? 20 : 5;
    if (rpm < 500)       return coarse ? 50 : 5;
    if (rpm < 1000)      return coarse ? 100 : 5;
    if (rpm < 3000)      return coarse ? 200 : 10;
    return coarse ? 400 : 20;
}

/*===========================================================================*/
/* Event Handler Function Type                                                */
/*===========================================================================*/

typedef void (*event_handler_func_t)(void);

/*===========================================================================*/
/* Event Dispatch Table */
/*===========================================================================*/

typedef struct {
    event_type_t event;
    event_handler_func_t handler;
} event_dispatch_entry_t;

/*===========================================================================*/
/* Individual Event Handlers                                                  */
/*===========================================================================*/

static void handle_btn_zero(void) {
    DEBUG_PRINT("EVT: ZERO\r\n");
    depth_calibrate_now();
}

static void handle_btn_menu(void) {
    DEBUG_PRINT("EVT: MENU\r\n");
    STATE_LOCK();
    bool was_in_menu = g_state.menu_active;
    bool motor_on = g_state.motor_running;
    if (!was_in_menu && !motor_on) {
        g_state.menu_active = true;
        g_state.state = APP_STATE_MENU;
    }
    STATE_UNLOCK();
    if (!was_in_menu && !motor_on) {
        ui_enter_menu();
    } else if (motor_on) {
        DEBUG_PRINT("(blocked - motor running)\r\n");
    }
}

static void handle_boot_complete(void) {
    buzzer_beep(BEEP_SUCCESS);
    DEBUG_PRINT("EVT: BOOT COMPLETE\r\n");
}

static void handle_low_voltage(void) {
    DEBUG_PRINT("EVT: LOW VOLTAGE WARNING!\r\n");
    STATE_LOCK();
    g_state.error_until = HAL_GetTick() + 3000;  // Show for 3 seconds
    g_state.error_line1 = " LOW VOLTAGE! ";
    g_state.error_line2 = "Check power     ";
    STATE_UNLOCK();
}

static void handle_motor_fault(void) {
    DEBUG_PRINT("EVT: MOTOR FAULT!\r\n");
    MOTOR_CMD(CMD_MOTOR_STOP, 0);
    STATE_LOCK();
    g_state.state = APP_STATE_ERROR;
    STATE_UNLOCK();
}

static void handle_btn_f2(void) {
    DEBUG_PRINT("EVT: F2 (disabled)\r\n");
    // Use menu to configure triggers
}

static void handle_btn_f4(void) {
    DEBUG_PRINT("EVT: F4 (disabled)\r\n");
    // Use menu to configure triggers
}

static void handle_jam_detected(void) {
    DEBUG_PRINT("EVT: JAM DETECTED!\r\n");
    // Motor already stopped by motor task
    STATE_LOCK();
    g_state.state = APP_STATE_ERROR;
    g_state.motor_running = false;
    // Display jam message on LCD (persistent until user action)
    g_state.error_until = HAL_GetTick() + 5000;  // Show for 5 seconds
    g_state.error_line1 = "! DRILL BIT JAM!";
    g_state.error_line2 = "Release pressure";
    STATE_UNLOCK();
}

static void handle_load_spike(void) {
    DEBUG_PRINT("EVT: LOAD SPIKE!\r\n");
    const settings_t* ss = settings_get();

    STATE_LOCK();
    g_state.motor_running = false;
    // Don't set error state - allow quick recovery
    STATE_UNLOCK();

    // Show load spike warning on LCD (16 chars max)
    static char spike_line1[17], spike_line2[17];
    snprintf(spike_line1, 17, "! LOAD SPIKE !");
    if (ss) {
        snprintf(spike_line2, 17, "Thresh: %d%%", ss->sensor.spike_thresh);
    } else {
        snprintf(spike_line2, 17, "Thresh: --%%");
    }

    STATE_LOCK();
    g_state.error_until = HAL_GetTick() + 2000;  // Show for 2 seconds
    g_state.error_line1 = spike_line1;
    g_state.error_line2 = spike_line2;
    STATE_UNLOCK();
}

static void handle_overheat(void) {
    DEBUG_PRINT("EVT: OVERHEAT SHUTDOWN!\r\n");
    uint16_t temp = motor_get_temperature();

    STATE_LOCK();
    g_state.state = APP_STATE_ERROR;
    g_state.motor_running = false;
    g_state.motor_fault = true;
    STATE_UNLOCK();

    // Show overheat message on LCD (16 chars max)
    static char overheat_line1[17], overheat_line2[17];
    snprintf(overheat_line1, 17, "!! OVERHEAT !!");
    snprintf(overheat_line2, 17, "Temp: %dC", temp);

    STATE_LOCK();
    g_state.error_until = HAL_GetTick() + 5000;  // Show for 5 seconds
    g_state.error_line1 = overheat_line1;
    g_state.error_line2 = overheat_line2;
    STATE_UNLOCK();
}

static void handle_temp_warning(void) {
    DEBUG_PRINT("EVT: TEMP WARNING!\r\n");
    uint16_t temp = motor_get_temperature();

    // Show warning message briefly (16 chars max)
    static char temp_line1[17], temp_line2[17];
    snprintf(temp_line1, 17, " TEMP WARNING");
    snprintf(temp_line2, 17, "Temp: %dC", temp);

    STATE_LOCK();
    g_state.error_until = HAL_GetTick() + 2000;  // Show for 2 seconds
    g_state.error_line1 = temp_line1;
    g_state.error_line2 = temp_line2;
    STATE_UNLOCK();
}

static void handle_btn_f1(void) {
    // F1 short press = SPD: Coarse speed adjust (variable step, wraps at max)
    STATE_LOCK();
    uint16_t step = get_speed_step(g_state.target_rpm, true);  // coarse
    g_state.target_rpm += step;
    if (g_state.target_rpm > SPEED_MAX_RPM)
        g_state.target_rpm = SPEED_MIN_RPM;
    uint16_t new_rpm = g_state.target_rpm;
    bool motor_on = g_state.motor_running;
    STATE_UNLOCK();

    if (motor_on) {
        MOTOR_CMD(CMD_MOTOR_SET_SPEED, new_rpm);
    }
}

static void handle_btn_f3(void) {
    // F3 = DEP: Set target depth to current depth
    // Also enable depth_mode=1 (Std) if it was off
    STATE_LOCK();
    int16_t current = g_state.current_depth;
    g_state.target_depth = current;
    if (g_state.depth_mode == 0) {
        g_state.depth_mode = 1;  // Enable Std mode
    }
    uint8_t mode = g_state.depth_mode;
    STATE_UNLOCK();

    DEBUG_PRINT("EVT: F3 target=");
    DEBUG_PRINTNUM(current / 10);
    DEBUG_PRINTC('.');
    DEBUG_PRINTNUM((current < 0 ? -current : current) % 10);
    uart_puts("mm mode=");
    uart_putc('0' + mode);
    uart_puts("\r\n");
}

static void handle_btn_encoder(void) {
    STATE_LOCK();
    bool in_menu = g_state.menu_active;
    STATE_UNLOCK();

    if (in_menu) {
        DEBUG_PRINT("EVT: ENCODER click in menu\r\n");
        ui_menu_click();  // Enter submenu or activate item
    } else {
        // Toggle fine/coarse speed mode
        STATE_LOCK();
        g_state.speed_fine_mode = !g_state.speed_fine_mode;
        bool fine = g_state.speed_fine_mode;
        STATE_UNLOCK();
        uart_puts(fine ? "Speed: FINE\r\n" : "Speed: COARSE\r\n");
    }
}

static void handle_btn_f1_long(void) {
    // F1 long press = Cycle through favorite speeds
    static uint8_t fav_index = 0;
    const settings_t* s = settings_get();

    // Find next non-zero favorite
    uint8_t start = fav_index;
    do {
        fav_index = (fav_index + 1) % NUM_FAVORITE_SPEEDS;
    } while (s->speed.favorite[fav_index] == 0 && fav_index != start);

    uint16_t fav_rpm = s->speed.favorite[fav_index];
    if (fav_rpm > 0) {
        STATE_LOCK();
        g_state.target_rpm = fav_rpm;
        bool motor_on = g_state.motor_running;
        STATE_UNLOCK();

        DEBUG_PRINT("EVT: F1 LONG fav[");
        DEBUG_PRINTNUM(fav_index);
        uart_puts("]=");
        DEBUG_PRINTNUM(fav_rpm);
        uart_puts(" RPM\r\n");

        if (motor_on) {
            MOTOR_CMD(CMD_MOTOR_SET_SPEED, fav_rpm);
        }
    }
}

static void handle_btn_enc_long(void) {
    // Encoder long press = Show status info screen
    DEBUG_PRINT("EVT: ENCODER LONG - Status screen\r\n");

    // Get current settings
    const settings_t* s = settings_get();
    STATE_LOCK();
    bool fine = g_state.speed_fine_mode;
    uint8_t depth_mode = g_state.depth_mode;
    STATE_UNLOCK();

    // Build status strings for LCD (static so they persist after function returns)
    static char line1[17], line2[17];

    // Line 1: Tap trigger info (16 chars max)
    // Build trigger string
    char triggers[8] = "";
    int idx = 0;
    if (s->tapping.depth_trigger_enabled) triggers[idx++] = 'D';
    if (s->tapping.load_increase_enabled) triggers[idx++] = 'I';
    if (s->tapping.load_slip_enabled) triggers[idx++] = 'S';
    if (s->tapping.clutch_slip_enabled) triggers[idx++] = 'C';
    if (s->tapping.quill_trigger_enabled) triggers[idx++] = 'Q';
    if (s->tapping.peck_trigger_enabled) triggers[idx++] = 'K';
    if (s->tapping.pedal_enabled) triggers[idx++] = 'P';
    triggers[idx] = '\0';

    if (idx > 0) {
        snprintf(line1, 17, "Tap:%s", triggers);
    } else {
        snprintf(line1, 17, "Tap:OFF");
    }

    // Line 2: Speed and depth mode (16 chars max)
    snprintf(line2, 17, "Spd:%s Dep:%s",
        fine ? "FINE" : "COAR",
        depth_mode == 0 ? "OFF" : depth_mode == 1 ? "STD" : "PRE");

    // Show on LCD for 2 seconds
    STATE_LOCK();
    g_state.error_until = HAL_GetTick() + 2000;
    g_state.error_line1 = line1;
    g_state.error_line2 = line2;
    STATE_UNLOCK();
}

static void handle_enc_cw(void) {
    // Single lock covers both the flag reads and the rpm update — no window between them
    STATE_LOCK();
    bool in_menu  = g_state.menu_active;
    bool motor_on = false;
    uint16_t new_rpm = 0;
    if (!in_menu) {
        bool fine_mode = g_state.speed_fine_mode;
        motor_on = g_state.motor_running;
        uint16_t step = get_speed_step(g_state.target_rpm, !fine_mode);  // coarse when not fine
        if (g_state.target_rpm < SPEED_MAX_RPM) {
            g_state.target_rpm += step;
            if (g_state.target_rpm > SPEED_MAX_RPM)
                g_state.target_rpm = SPEED_MAX_RPM;
        }
        new_rpm = g_state.target_rpm;
    }
    STATE_UNLOCK();

    if (in_menu) {
        ui_menu_rotate(1);  // Down in menu
    } else if (motor_on) {
        // Update motor speed if running (rate-limited to prevent MCB command overflow)
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - last_speed_cmd_tick;
        if (elapsed >= pdMS_TO_TICKS(SPEED_UPDATE_INTERVAL_MS)) {
            MOTOR_CMD(CMD_MOTOR_SET_SPEED, new_rpm);
            last_speed_cmd_tick = now;
        }
    }
}

static void handle_enc_ccw(void) {
    // Single lock covers both the flag reads and the rpm update — no window between them
    STATE_LOCK();
    bool in_menu  = g_state.menu_active;
    bool motor_on = false;
    uint16_t new_rpm = 0;
    if (!in_menu) {
        bool fine_mode = g_state.speed_fine_mode;
        motor_on = g_state.motor_running;
        uint16_t step = get_speed_step(g_state.target_rpm, !fine_mode);  // coarse when not fine
        if (g_state.target_rpm > SPEED_MIN_RPM) {
            g_state.target_rpm -= step;
            if (g_state.target_rpm < SPEED_MIN_RPM)
                g_state.target_rpm = SPEED_MIN_RPM;
        }
        new_rpm = g_state.target_rpm;
    }
    STATE_UNLOCK();

    if (in_menu) {
        ui_menu_rotate(-1);  // Up in menu
    } else if (motor_on) {
        // Update motor speed if running (rate-limited to prevent MCB command overflow)
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - last_speed_cmd_tick;
        if (elapsed >= pdMS_TO_TICKS(SPEED_UPDATE_INTERVAL_MS)) {
            MOTOR_CMD(CMD_MOTOR_SET_SPEED, new_rpm);
            last_speed_cmd_tick = now;
        }
    }
}

static void handle_btn_guard(void) {
    // Guard state changed - check if opened while motor running
    STATE_LOCK();
    bool guard_closed = g_state.guard_closed;
    app_state_t state = g_state.state;
    STATE_UNLOCK();

    if (!guard_closed && (state == APP_STATE_DRILLING || state == APP_STATE_TAPPING)) {
        // Guard opened while running - stop motor + safety spindle hold
        DEBUG_PRINT("EVT: GUARD OPENED - stopping motor + spindle hold!\r\n");
        MOTOR_CMD(CMD_MOTOR_STOP, 0);
        // Activate safety spindle hold (CL=12%) - prevents spindle rotation
        motor_spindle_hold_safety();
        STATE_LOCK();
        g_state.state = APP_STATE_IDLE;
        // Show error message (persistent until guard closed)
        g_state.error_until = HAL_GetTick() + 30000;  // 30 seconds
        g_state.error_line1 = " GUARD OPENED!  ";
        g_state.error_line2 = " Close to clear ";
        STATE_UNLOCK();
    } else if (guard_closed && motor_is_spindle_hold_active()) {
        // Guard closed and spindle hold was active - release hold, clear error
        DEBUG_PRINT("EVT: GUARD CLOSED - releasing spindle hold\r\n");
        motor_spindle_release();
        STATE_LOCK();
        g_state.error_until = 0;  // Clear error immediately
        g_state.error_line1 = "";
        g_state.error_line2 = "";
        STATE_UNLOCK();
        DEBUG_PRINT("Guard error cleared\r\n");
    }
}

static void handle_btn_estop(void) {
    // E-Stop is level-sensitive - check current state
    bool estop_active = encoder_estop_active();
    DEBUG_PRINT("EVT: E-STOP ");
    uart_puts(estop_active ? "ENGAGED!\r\n" : "RELEASED\r\n");

    if (estop_active) {
        // Hardware cutoff FIRST — direct GPIO write, cannot block
        motor_hardware_disable();
        // E-Stop engaged - immediate motor stop + safety spindle hold
        MOTOR_CMD(CMD_MOTOR_STOP, 0);
        // Activate safety spindle hold (CL=12%) - prevents spindle rotation
        motor_spindle_hold_safety();
        STATE_LOCK();
        g_state.state = APP_STATE_ERROR;
        g_state.estop_active = true;
        g_state.motor_running = false;
        g_state.motor_fault = true;
        // Show persistent error on LCD
        g_state.error_until = HAL_GetTick() + ESTOP_DISPLAY_MS;
        g_state.error_line1 = "!! E-STOP !!   ";
        g_state.error_line2 = "Release to clear";
        STATE_UNLOCK();
    } else {
        // E-Stop released - release spindle hold, clear error, allow recovery
        DEBUG_PRINT("E-Stop release: clearing error message...\r\n");
        motor_spindle_release();
        STATE_LOCK();
        g_state.estop_active = false;
        g_state.motor_fault = false;
        g_state.state = APP_STATE_IDLE;
        g_state.error_until = 0;  // Clear error timer
        g_state.error_line1 = "";
        g_state.error_line2 = "";
        uint32_t check_time = g_state.error_until;
        STATE_UNLOCK();
        DEBUG_PRINT("E-Stop error cleared: error_until=");
        DEBUG_PRINTNUM(check_time);
        uart_puts("\r\n");
    }
}

static void handle_btn_start(void) {
    DEBUG_PRINT("EVT: START\r\n");
    STATE_LOCK();
    app_state_t state = g_state.state;
    uint16_t rpm = g_state.target_rpm;
    bool estop = g_state.estop_active;
    bool guard_closed = g_state.guard_closed;
    STATE_UNLOCK();

    // Clear non-estop errors with START button
    if (state == APP_STATE_ERROR && !estop) {
        DEBUG_PRINT("Clearing error...\r\n");
        STATE_LOCK();
        g_state.state = APP_STATE_IDLE;
        g_state.motor_fault = false;
        g_state.error_until = 0;  // Clear error message immediately
        STATE_UNLOCK();
        return;
    }

    // Get settings
    const settings_t* settings = settings_get();

    // SAFETY: refuse to start if E-Stop active
    if (estop) {
        uart_puts("E-Stop active - motor blocked\r\n");
        STATE_LOCK();
        g_state.error_until = HAL_GetTick() + 1500;
        g_state.error_line1 = " E-STOP ACTIVE! ";
        g_state.error_line2 = "Release to start";
        STATE_UNLOCK();
        return;
    }

    // SAFETY: refuse to start if guard is open (and guard check enabled)
    if (settings && settings->sensor.guard_check_enabled && !guard_closed) {
        uart_puts("Guard open - motor blocked\r\n");
        STATE_LOCK();
        g_state.error_until = HAL_GetTick() + 1500;
        g_state.error_line1 = "  GUARD OPEN!   ";
        g_state.error_line2 = " Close to start ";
        STATE_UNLOCK();
        return;
    }

    if (state == APP_STATE_IDLE) {
        MOTOR_CMD(CMD_MOTOR_SET_SPEED, rpm);

        // Apply motor settings (profile + power output) before starting
        const settings_t* settings = settings_get();
        if (settings) {
            motor_set_profile(settings->motor.profile);
            motor_set_power_output(settings->power.power_output);
        }

        // Check if any triggers are enabled
        bool any_trigger = false;
        if (settings) {
            any_trigger = settings->tapping.depth_trigger_enabled ||
                         settings->tapping.load_increase_enabled ||
                         settings->tapping.load_slip_enabled ||
                         settings->tapping.clutch_slip_enabled ||
                         settings->tapping.quill_trigger_enabled ||
                         settings->tapping.peck_trigger_enabled ||
                         settings->tapping.pedal_enabled;
        }

        if (any_trigger) {
            STATE_LOCK();
            g_state.state = APP_STATE_TAPPING;
            g_state.motor_running = true;  // Optimistic - motor task confirms via GF poll
            STATE_UNLOCK();
        } else {
            STATE_LOCK();
            g_state.state = APP_STATE_DRILLING;
            g_state.motor_running = true;  // Optimistic - motor task confirms via GF poll
            STATE_UNLOCK();
        }
        MOTOR_CMD(CMD_MOTOR_FORWARD, 0);
    } else if (state == APP_STATE_DRILLING || state == APP_STATE_TAPPING) {
        MOTOR_CMD(CMD_MOTOR_STOP, 0);
        STATE_LOCK();
        g_state.state = APP_STATE_IDLE;
        STATE_UNLOCK();
    }
}

// Additional handlers extracted below...
// (Keeping original switch for now, will replace after extracting all)

/*===========================================================================*/
/* Event Dispatch Table Array */
/*===========================================================================*/

static const event_dispatch_entry_t event_dispatch_table[] = {
    // All event handlers (21 total)
    {EVT_BTN_ZERO,      handle_btn_zero},
    {EVT_BTN_MENU,      handle_btn_menu},
    {EVT_BTN_START,     handle_btn_start},
    {EVT_BTN_F1,        handle_btn_f1},
    {EVT_BTN_F1_LONG,   handle_btn_f1_long},
    {EVT_BTN_F2,        handle_btn_f2},
    {EVT_BTN_F3,        handle_btn_f3},
    {EVT_BTN_F4,        handle_btn_f4},
    {EVT_BTN_ENCODER,   handle_btn_encoder},
    {EVT_BTN_ENC_LONG,  handle_btn_enc_long},
    {EVT_BTN_ESTOP,     handle_btn_estop},
    {EVT_BTN_GUARD,     handle_btn_guard},
    {EVT_ENC_CW,        handle_enc_cw},
    {EVT_ENC_CCW,       handle_enc_ccw},
    {EVT_MOTOR_FAULT,   handle_motor_fault},
    {EVT_JAM_DETECTED,  handle_jam_detected},
    {EVT_LOAD_SPIKE,    handle_load_spike},
    {EVT_OVERHEAT,      handle_overheat},
    {EVT_TEMP_WARNING,  handle_temp_warning},
    {EVT_BOOT_COMPLETE, handle_boot_complete},
    {EVT_LOW_VOLTAGE,   handle_low_voltage},
};

#define EVENT_DISPATCH_TABLE_SIZE (sizeof(event_dispatch_table) / sizeof(event_dispatch_entry_t))

/*===========================================================================*/
/* Event Handler */
/*===========================================================================*/

void handle_event(event_type_t evt) {
    // Dispatch table lookup for all event handlers
    for (size_t i = 0; i < EVENT_DISPATCH_TABLE_SIZE; i++) {
        if (event_dispatch_table[i].event == evt) {
            event_dispatch_table[i].handler();
            return;
        }
    }

    // Unknown event - log warning but don't crash (defensive programming)
    DEBUG_PRINT("WARN: Unknown event 0x");
    char hex[5];
    uint16_t ev = (uint16_t)evt;
    for (int i = 3; i >= 0; i--) {
        uint8_t nibble = (ev >> (i * 4)) & 0xF;
        hex[3 - i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
    }
    hex[4] = '\0';
    uart_puts(hex);
    uart_puts("\r\n");
}
