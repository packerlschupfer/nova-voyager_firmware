/**
 * @file events.c
 * @brief Event handling implementation
 *
 * Handles all UI events (buttons, encoder, motor faults) and updates system state
 */

#include "events.h"
#include "events_policy.h"
#include "jam.h"
#include "config.h"
#include "shared.h"
#include "settings.h"
#include "tapping.h"
#include "motor.h"
#include "buzzer.h"
#include "display.h"
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
/* Edge latches — see encoder.h. Declared here because events.c predates the
 * encoder.h include and pulls in only what it uses. */
extern bool encoder_guard_opened_since(void);
extern bool encoder_estop_engaged_since(void);
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
    motor_hardware_disable();
    xQueueReset(g_motor_cmd_queue);
    MOTOR_CMD(CMD_MOTOR_STOP, 0);
    STATE_LOCK();
    g_state.state = APP_STATE_ERROR;
    g_state.motor_running = false;
    STATE_UNLOCK();
}

static void handle_btn_f2(void) {
    // F2 = arm/disarm tapping triggers
    const settings_t* s = settings_get();
    bool any = s->tapping.depth_trigger_enabled ||
               s->tapping.load_increase_enabled ||
               s->tapping.load_slip_enabled ||
               s->tapping.clutch_slip_enabled ||
               s->tapping.quill_trigger_enabled ||
               s->tapping.peck_trigger_enabled ||
               s->tapping.pedal_enabled;

    if (!any) {
        buzzer_beep(BEEP_ERROR);
        return;
    }

    STATE_LOCK();
    g_state.tapping_armed = !g_state.tapping_armed;
    bool armed = g_state.tapping_armed;
    STATE_UNLOCK();

    uart_puts(armed ? "Tapping: ARMED\r\n" : "Tapping: DISARMED\r\n");
    buzzer_beep(armed ? BEEP_SUCCESS : BEEP_CLICK);
}

static void handle_btn_f4(void) {
    display_cycle_row3();
}

static void handle_jam_detected(void) {
    DEBUG_PRINT("EVT: JAM DETECTED!\r\n");

    /* AUDIT FIX (LOW, jam.c:417): every detector routes through this one
     * handler now — jam.c used to send a second, detector-specific event that
     * overwrote this screen. Name the detector here instead, so the operator
     * learns which one tripped without a competing event doing it. */
    const char* line1 = "! DRILL BIT JAM!";
    const char* line2 = "Release pressure";
    const jam_status_t* js = jam_get_status();
    if (js) {
        switch (js->type) {
            case JAM_LOAD_SPIKE:
            case JAM_LOAD_STEP:
                line1 = "! LOAD SPIKE !";
                line2 = "Release pressure";
                break;
            case JAM_LOW_LOAD:
                line1 = "! NO LOAD !";
                line2 = "Belt or tool?";
                break;
            case JAM_STALL_DETECTED:
                line1 = "! MOTOR STALL !";
                line2 = "Release pressure";
                break;
            case JAM_STARTUP_TIMEOUT:
                line1 = "! NO START !";
                line2 = "Check spindle";
                break;
            case JAM_COMM_TIMEOUT:
                line1 = "! MCB COMMS !";
                line2 = "No response";
                break;
            case JAM_VIBRATION:
                line1 = "! VIBRATION !";
                line2 = "Check setup";
                break;
            default:
                break;  /* sustained load keeps the generic jam wording */
        }
    }

    /* REVIEW FIX (HIGH): "motor already stopped by motor task" was only half
     * true. jam.c's trigger_jam() calls motor_emergency_stop(), which drops PD4
     * and sends CMD_STOP over the UART — but it never touches task_motor's
     * `motor_enabled`, which is what jam_update() receives as `motor_commanded`.
     * So after the operator presses ON (the documented recovery, which calls
     * jam_acknowledge()), the very next poll saw motor_commanded=true,
     * startup_complete=true, motor_was_running=true, motor_running=false —
     * armed stall detection — and ~500 ms later fired JAM_STALL_DETECTED again.
     * No jam could be cleared without a power cycle.
     *
     * This only became reachable when the detectors started running from the
     * idle poll branch as well (task_motor.c: run_jam_detectors, last round);
     * before that jam_update() simply was not called while stopped, so the
     * re-trip could not happen and the missing stop went unnoticed.
     *
     * handle_motor_fault() and handle_overheat() both queue the stop. Do the
     * same: local_motor_stop() is what clears motor_enabled. */
    motor_hardware_disable();
    xQueueReset(g_motor_cmd_queue);
    MOTOR_CMD(CMD_MOTOR_STOP, 0);

    STATE_LOCK();
    g_state.state = APP_STATE_ERROR;
    g_state.motor_running = false;
    // Display jam message on LCD (persistent until user action)
    g_state.error_until = HAL_GetTick() + 5000;  // Show for 5 seconds
    g_state.error_line1 = line1;
    g_state.error_line2 = line2;
    STATE_UNLOCK();
}

/* No longer reached: jam.c sends one EVT_JAM_DETECTED per trip and
 * handle_jam_detected() names the detector. Kept registered so an
 * EVT_LOAD_SPIKE from any future source still lands somewhere sane rather than
 * being silently dropped, but note it does NOT set APP_STATE_ERROR — the
 * emergency stop that accompanies a real spike is jam.c's job. */
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
    xQueueReset(g_motor_cmd_queue);
    MOTOR_CMD(CMD_MOTOR_STOP, 0);
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
    // F1 short press = cycle through favorite speeds
    static uint8_t fav_index = 0;
    const settings_t* s = settings_get();

    uint8_t start = fav_index;
    do {
        fav_index = (fav_index + 1) % NUM_FAVORITE_SPEEDS;
    } while (s->speed.favorite[fav_index] == 0 && fav_index != start);

    uint16_t fav_rpm = s->speed.favorite[fav_index];
    if (fav_rpm == 0) {
        buzzer_beep(BEEP_ERROR);
        return;
    }

    /* REVIEW FIX (HIGH): honour the operator's own speed cap. Favourites are
     * clamped only to the compile-time SPEED_MAX_RPM when stored, and
     * set_defaults() seeds favorite[7] = 5500 — so with Speed>Max set to 800 for
     * a hole saw, one F1 tap commanded 5500 RPM on a running spindle. The
     * encoder path (below), the console SPEED command and settings_set_speed()
     * all respect max_limit; this recall did not. Clamp rather than refuse: the
     * operator asked for a speed, and the cap is what they set it to. */
    if (fav_rpm > s->speed.max_limit) {
        fav_rpm = s->speed.max_limit;
    }

    STATE_LOCK();
    g_state.target_rpm = fav_rpm;
    bool motor_on = g_state.motor_running;
    STATE_UNLOCK();

    /* Recalling a favourite is the operator choosing a speed. */
    settings_note_operator_speed(fav_rpm, HAL_GetTick());

    if (motor_on) {
        MOTOR_CMD(CMD_MOTOR_SET_SPEED, fav_rpm);
    }
    buzzer_beep(BEEP_SUCCESS);
}

static void handle_btn_f3(void) {
    // F3 = DEP: Set target depth to current depth
    // Also enable depth_mode=1 (Std) if it was off
    STATE_LOCK();
    int16_t current = g_state.current_depth;
    STATE_UNLOCK();

    /* REVIEW FIX (MEDIUM): current_depth was copied in with no clamp, so
     * pressing F3 with the quill retracted ABOVE the zero point armed a
     * NEGATIVE target. `current_depth >= target` is then true the instant the
     * spindle starts, so the auto-stop fired immediately — and the re-arm block
     * in task_depth requires `target > 0`, so the fired latch could never
     * clear and the depth stop stayed dead for the rest of the session with
     * nothing on screen to say so. A target above the zero point is not a
     * depth; refuse it rather than arming something that cannot work. */
    if (current <= 0) {
        uart_puts("F3: quill is at or above zero - nothing to set as a depth\r\n");
        STATE_LOCK();
        g_state.error_until = HAL_GetTick() + 2000;
        g_state.error_line1 = "SET DEPTH FAILED";
        g_state.error_line2 = "Lower quill 1st ";
        STATE_UNLOCK();
        return;
    }

    STATE_LOCK();
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
    // AUDIT FIX (MEDIUM, task_ui.c:197): the in-menu suppression at task_ui
    // was dead code (long_fired always false at press time), so a held F1
    // inside the menu used to silently save the target RPM into a favorite
    // slot in addition to firing menu_back. Guard the handler itself.
    if (g_state.menu_active) return;

    // F1 long press = save current target speed to next empty fav slot
    static uint8_t save_index = 0;
    STATE_LOCK();
    uint16_t rpm = g_state.target_rpm;
    STATE_UNLOCK();

    // Find next empty slot (or overwrite round-robin)
    uint8_t start = save_index;
    const settings_t* s = settings_get();
    do {
        if (s->speed.favorite[save_index] == 0) break;
        save_index = (save_index + 1) % NUM_FAVORITE_SPEEDS;
    } while (save_index != start);

    settings_set_favorite_speed(save_index, rpm);
    DEBUG_PRINT("EVT: F1 LONG save fav[");
    DEBUG_PRINTNUM(save_index);
    uart_puts("]=");
    DEBUG_PRINTNUM(rpm);
    uart_puts(" RPM\r\n");

    save_index = (save_index + 1) % NUM_FAVORITE_SPEEDS;
    buzzer_beep(BEEP_SUCCESS);
}

static void handle_btn_enc_long(void) {
    /* REVIEW FIX (MEDIUM): same gap handle_btn_f1_long was fixed for, and it
     * was left open here. process_button_long_press() for the encoder runs
     * BEFORE the in-menu early return in task_ui, so holding the encoder past
     * 500 ms to confirm a menu value also queued this event — which then set
     * error_until and repointed error_line1/2 at its statics, painting a
     * status screen over the menu and leaving it up for 2 s after exit. */
    if (g_state.menu_active) return;

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
        uint16_t max_rpm = settings_get()->speed.max_limit;
        if (g_state.target_rpm < max_rpm) {
            g_state.target_rpm += step;
            if (g_state.target_rpm > max_rpm)
                g_state.target_rpm = max_rpm;
        }
        new_rpm = g_state.target_rpm;
    }
    STATE_UNLOCK();

    if (in_menu) {
        ui_menu_rotate(1);  // Down in menu
    } else {
        /* The operator turned the knob: this is a speed they chose, so it is
         * eligible to be remembered across a power cycle. */
        settings_note_operator_speed(new_rpm, HAL_GetTick());
    }
    if (!in_menu && motor_on) {
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
    } else {
        /* The operator turned the knob: this is a speed they chose, so it is
         * eligible to be remembered across a power cycle. */
        settings_note_operator_speed(new_rpm, HAL_GetTick());
    }
    if (!in_menu && motor_on) {
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

    /* AUDIT FIX (HIGH, events.c:441): was `!guard_closed`, i.e. the live level
     * at dequeue time. A guard bumped open and shut before this ran matched
     * neither branch and the spindle coasted unbraked. The condition now lives
     * in include/events_policy.h, where test/test_events_policy covers the
     * bounce case against the real predicate. */
    const bool motor_active = (state == APP_STATE_DRILLING || state == APP_STATE_TAPPING);
    const bool aborting = guard_requires_abort(encoder_guard_opened_since(),
                                               guard_closed, motor_active);

    if (aborting) {
        // Guard opened while running - stop motor + safety spindle hold
        DEBUG_PRINT("EVT: GUARD OPENED - stopping motor + spindle hold!\r\n");
        xQueueReset(g_motor_cmd_queue);
        MOTOR_CMD(CMD_MOTOR_STOP, 0);
        /* REVIEW FIX (HIGH): this used to call motor_hardware_enable() here.
         * The enable is an immediate GPIO write from task_main, but the STOP
         * above is only QUEUED — task_motor can be up to ~1.25 s into a status
         * poll before it dequeues. So PD4 went back HIGH with the MCB still in
         * its commanded-run state and the spindle could spin back up after the
         * guard opened, until the stop was finally executed.
         *
         * It is also redundant now: spindle_hold_start_with_cl() raises PD4
         * itself, in the right order — after its own VR/CL/VS-off preamble and
         * immediately before the commands that create the torque. */
        motor_spindle_hold_safety();
        STATE_LOCK();
        g_state.state = APP_STATE_IDLE;
        // Show error message (persistent until guard closed)
        g_state.error_until = HAL_GetTick() + 30000;  // 30 seconds
        g_state.error_line1 = " GUARD OPENED!  ";
        g_state.error_line2 = " Close to clear ";
        STATE_UNLOCK();
    } else if (!aborting && guard_closed) {
        /* Guard closed again.
         *
         * REVIEW FIX (MEDIUM): clearing the banner used to require
         * guard_permits_release(), i.e. a LIVE spindle hold — but a safety hold
         * auto-releases after SAFETY_HOLD_TIMEOUT_MS (2 s) while the banner is
         * armed for 30. Open the guard, wait more than two seconds, close it:
         * neither branch ran, and " GUARD OPENED! / Close to clear" kept
         * painting over the status screen for the balance of the 30 s on a
         * closed, ready machine — telling the operator to do the thing they had
         * just done.
         *
         * Releasing the hold still depends on there being one; clearing the
         * banner depends only on the guard being closed. */
        /* The hold release keeps its own policy — guard_permits_release() is
         * still the single answer to "may the hold be dropped now", and stays
         * unit-tested. Only the BANNER was wrongly tied to it. */
        if (guard_permits_release(aborting, guard_closed,
                                  motor_is_spindle_hold_active())) {
            DEBUG_PRINT("EVT: GUARD CLOSED - releasing spindle hold\r\n");
            motor_spindle_release();
        }
        STATE_LOCK();
        g_state.error_until = 0;  // Clear error immediately
        g_state.error_line1 = "";
        g_state.error_line2 = "";
        STATE_UNLOCK();
        DEBUG_PRINT("Guard error cleared\r\n");
    }
}

/* True when APP_STATE_ERROR was already set at the moment the E-Stop was
 * engaged — i.e. the error is not the E-Stop's to clear on release. */
static bool s_error_predates_estop = false;

static void handle_btn_estop(void) {
    // E-Stop is level-sensitive - check current state
    bool estop_active = encoder_estop_active();
    /* AUDIT FIX (HIGH, events.c:479): same edge-vs-level defect as the guard
     * handler. A press released before this event was dequeued took the
     * "RELEASED" branch only: the motor command queue was never purged, no
     * CMD_MOTOR_STOP was sent, and the state was cleared to IDLE as though
     * nothing had happened. An E-Stop that the operator felt as a press must
     * always stop the machine, however briefly it was held.
     *
     * When both are true (a bounce) both branches run in order, which lands on
     * exactly the state a press-then-release physically produced: commands
     * purged, motor stopped, then recovery to IDLE. Running only the engaged
     * branch would leave the UI showing a persistent E-Stop screen with no
     * further edge coming to clear it. */
    const bool estop_was_engaged = encoder_estop_engaged_since();
    DEBUG_PRINT("EVT: E-STOP ");
    uart_puts(estop_active ? "ENGAGED!\r\n" :
              (estop_was_engaged ? "BOUNCED (engaged then released)\r\n" : "RELEASED\r\n"));

    if (estop_requires_stop(estop_was_engaged, estop_active)) {
        // Hardware cutoff already done by EXTI0 ISR (GPIOD->BSRR)
        // Purge any pending motor commands (FORWARD/REVERSE from tapping)
        xQueueReset(g_motor_cmd_queue);
        MOTOR_CMD(CMD_MOTOR_STOP, 0);
        /* No motor_hardware_enable() here — see the guard handler above for
         * why re-driving PD4 before the queued stop has executed is wrong, and
         * why spindle_hold_start_with_cl() owns the enable now. */
        motor_spindle_hold_safety();
        STATE_LOCK();
        /* REVIEW FIX (HIGH): remember whether the machine was ALREADY in a
         * fault before the E-Stop was engaged. The release path below used to
         * clear APP_STATE_ERROR and motor_fault unconditionally, so tapping and
         * releasing the mushroom head — or a noise glitch producing a
         * press/release pair — wiped an OVERHEAT or a motor fault and left the
         * machine immediately restartable into the condition that caused it.
         * Only an error the E-Stop itself raised is the E-Stop's to clear. */
        s_error_predates_estop = (g_state.state == APP_STATE_ERROR);
        g_state.state = APP_STATE_ERROR;
        g_state.estop_active = true;
        g_state.motor_running = false;
        g_state.motor_fault = true;
        // BUGFIX 2026-07-14: don't set error_until for E-Stop. The
        // APP_STATE_ERROR + estop=true branch of display_update already
        // renders a persistent E-Stop screen — the 30 s transient message
        // caused two different screens back-to-back (transient "!! E-STOP !!
        // Release to clear" for 30s, then the "!!!!! EMERGENCY STOP !!!!!"
        // permanent one). One persistent screen is cleaner.
        g_state.error_until = 0;
        g_state.error_line1 = "";
        g_state.error_line2 = "";
        STATE_UNLOCK();
    }

    if (estop_requires_recovery(estop_active)) {
        // E-Stop released - release spindle hold, clear error, allow recovery
        DEBUG_PRINT("E-Stop release: clearing error message...\r\n");
        motor_spindle_release();
        /* REVIEW FIX (HIGH): this path clears APP_STATE_ERROR regardless of
         * what caused it. jam_update() returns early forever while a jam is
         * unacknowledged (jam.c:202), and jam_acknowledge() had exactly one
         * caller — the START-clears-error path below, reachable only from
         * APP_STATE_ERROR. So: jam trips, the operator taps and releases the
         * E-Stop, the error is wiped, the machine is immediately restartable —
         * and startup/stall/comm/vibration detection is dead until reboot with
         * nothing on screen to say so. Releasing the E-Stop returns the machine
         * to service, so it has to re-arm detection exactly like ON does. */
        extern void jam_acknowledge(void);
        jam_acknowledge();
        STATE_LOCK();
        g_state.estop_active = false;
        if (s_error_predates_estop) {
            /* Something else was already wrong — an overheat, a motor fault.
             * The E-Stop is released; that condition is not. Leave ERROR and
             * its screen in place so the operator still has to deal with it. */
            uart_puts("E-Stop released - earlier fault still active\r\n");
        } else {
            g_state.motor_fault = false;
            g_state.state = APP_STATE_IDLE;
            g_state.error_until = 0;  // Clear error timer
            g_state.error_line1 = "";
            g_state.error_line2 = "";
        }
        s_error_predates_estop = false;
        STATE_UNLOCK();
        DEBUG_PRINT("E-Stop error cleared\r\n");
    }
}

static void handle_btn_start(void) {
    DEBUG_PRINT("EVT: START\r\n");
    STATE_LOCK();
    app_state_t state = g_state.state;
    uint16_t rpm = g_state.target_rpm;
    bool estop = g_state.estop_active;
    bool guard_closed = g_state.guard_closed;
    bool flash_busy = g_state.flash_in_progress;
    STATE_UNLOCK();

    if (flash_busy) {
        return;
    }

    // Clear non-estop errors with START button
    if (state == APP_STATE_ERROR && !estop) {
        DEBUG_PRINT("Clearing error...\r\n");
        STATE_LOCK();
        g_state.state = APP_STATE_IDLE;
        g_state.motor_fault = false;
        g_state.error_until = 0;  // Clear error message immediately
        STATE_UNLOCK();
        // AUDIT FIX (HIGH, jam.c:179): jam status latches forever without
        // acknowledgement — jam_acknowledge/jam_reset had zero production
        // callers, so the first jam trip (typically the pre-fix boot false
        // JAM_COMM_TIMEOUT) permanently disabled startup/stall/comm/vibration
        // detection until reboot. The "Press ON to clear" flow is the
        // documented recovery path, so wire it here.
        extern void jam_acknowledge(void);
        jam_acknowledge();
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
        MOTOR_CMD(CMD_MOTOR_APPLY_SETTINGS, 0);

        // Check if any triggers are enabled
        const settings_t* settings = settings_get();
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

        STATE_LOCK();
        g_state.state = any_trigger ? APP_STATE_TAPPING : APP_STATE_DRILLING;
        STATE_UNLOCK();
        MOTOR_CMD(CMD_MOTOR_FORWARD, 0);
    } else if (state == APP_STATE_DRILLING || state == APP_STATE_TAPPING) {
        MOTOR_CMD(CMD_MOTOR_STOP, 0);
        STATE_LOCK();
        g_state.state = APP_STATE_IDLE;
        STATE_UNLOCK();
    }
}

static void handle_depth_target(void) {
    DEBUG_PRINT("EVT: Depth target reached\r\n");
    buzzer_beep(BEEP_CONFIRM);
}

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
    {EVT_DEPTH_TARGET,  handle_depth_target},
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
