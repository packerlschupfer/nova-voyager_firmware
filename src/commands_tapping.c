/**
 * @file commands_tapping.c
 * @brief Tapping and step drill-related commands
 */

#include "commands_internal.h"
#include "tapping.h"

/*===========================================================================*/
/* Helper Functions                                                          */
/*===========================================================================*/

/**
 * @brief Validate value is within range
 * @param val Value to validate
 * @param min Minimum (inclusive)
 * @param max Maximum (inclusive)
 * @param unit Optional unit string (e.g., "ms", "%", NULL)
 * @return true if valid, false if out of range (prints error)
 */
static bool validate_range(int val, int min, int max, const char* unit) {
    if (val >= min && val <= max) {
        return true;
    }
    uart_puts("Range: ");
    print_num(min);
    uart_putc('-');
    print_num(max);
    if (unit) {
        uart_putc(' ');
        uart_puts(unit);
    }
    uart_puts("\r\n");
    return false;
}

/*===========================================================================*/
/* Tapping Setting Commands                                                  */
/*===========================================================================*/

void cmd_tapload(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx > 8 && cmd_buf[7] == ' ') {
        int val = cmd_get_arg_int(8);
        if (validate_range(val, TAP_LOAD_THRESHOLD_MIN, TAP_LOAD_THRESHOLD_MAX, "%")) {
            tapping_set_load_threshold(val);
            uart_puts("Load threshold: "); print_num(val); uart_puts("%\r\n");
        }
    } else {
        const tapping_settings_t* ts = tapping_get_settings();
        uart_puts("Load threshold: "); print_num(ts->load_increase_threshold); uart_puts("%\r\n");
    }
}

void cmd_taprev(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx > 7 && cmd_buf[6] == ' ') {
        int val = cmd_get_arg_int(7);
        if (validate_range(val, TAP_REVERSE_TIME_MIN, TAP_REVERSE_TIME_MAX, "ms")) {
            tapping_set_reverse_time(val);
            uart_puts("Reverse time: "); print_num(val); uart_puts("ms\r\n");
        }
    } else {
        const tapping_settings_t* ts = tapping_get_settings();
        uart_puts("Reverse time: "); print_num(ts->load_increase_reverse_ms); uart_puts("ms\r\n");
    }
}

void cmd_tappeck(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx > 8 && cmd_buf[7] == ' ') {
        int fwd = 0, rev = 0, cyc = 0;
        int i = 8;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            fwd = fwd * 10 + (cmd_buf[i++] - '0');
        }
        if (i < cmd_idx && cmd_buf[i] == ' ') i++;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            rev = rev * 10 + (cmd_buf[i++] - '0');
        }
        if (i < cmd_idx && cmd_buf[i] == ' ') i++;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            cyc = cyc * 10 + (cmd_buf[i++] - '0');
        }
        if (fwd >= TAP_PECK_FWD_MS_MIN && fwd <= TAP_PECK_FWD_MS_MAX &&
            rev >= TAP_PECK_REV_MS_MIN && rev <= TAP_PECK_REV_MS_MAX) {
            tapping_set_peck_params(fwd, rev, cyc);
            settings_set_peck_fwd_ms(fwd);
            settings_set_peck_rev_ms(rev);
            settings_set_peck_cycles(cyc);
            DEBUG_PRINT("Peck: fwd="); print_num(fwd);
            uart_puts("ms, rev="); print_num(rev);
            uart_puts("ms, cycles="); print_num(cyc); uart_puts("\r\n");
        } else {
            uart_puts("Usage: TAPPECK <fwd_ms> <rev_ms> <cyc>\r\n");
            uart_puts("  fwd: 50-5000ms, rev: 50-2000ms\r\n");
        }
    } else {
        const tapping_settings_t* ts = tapping_get_settings();
        DEBUG_PRINT("Peck: fwd="); print_num(ts->peck_fwd_ms);
        uart_puts("ms, rev="); print_num(ts->peck_rev_ms);
        uart_puts("ms, cycles="); print_num(ts->peck_cycles); uart_puts("\r\n");
    }
}

void cmd_tapact(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx > 7 && cmd_buf[6] == ' ') {
        int val = cmd_buf[7] - '0';
        if (val >= 0 && val <= 1) {
            tapping_set_depth_action((tap_depth_action_t)val);
            uart_puts("Depth action: "); uart_puts(val ? "REVERSE" : "STOP"); uart_puts("\r\n");
        } else {
            uart_puts("0=stop, 1=reverse\r\n");
        }
    } else {
        const tapping_settings_t* ts = tapping_get_settings();
        uart_puts("Depth action: ");
        uart_puts(ts->depth_action ? "REVERSE" : "STOP");
        uart_puts("\r\n");
    }
}

void cmd_tapthr(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx > 7 && cmd_buf[6] == ' ') {
        int val = cmd_buf[7] - '0';
        if (val >= 0 && val <= 1) {
            tapping_set_through_detect(val);
            uart_puts("Through detect: "); uart_puts(val ? "ON" : "OFF"); uart_puts("\r\n");
        } else {
            uart_puts("0=off, 1=on\r\n");
        }
    } else {
        const tapping_settings_t* ts = tapping_get_settings();
        uart_puts("Through detect: ");
        uart_puts(ts->load_slip_enabled ? "ON" : "OFF");
        uart_puts("\r\n");
    }
}

void cmd_tapbrk(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx > 7 && cmd_buf[6] == ' ') {
        int delay = cmd_get_arg_int(7);
        if (delay >= 50 && delay <= 500) {
            settings_set_brake_delay(delay);
            uart_puts("Brake delay: "); print_num(delay); uart_puts(" ms\r\n");
        } else {
            uart_puts("Range: 50-500 ms\r\n");
        }
    } else {
        const settings_t* s = settings_get();
        uart_puts("Brake delay: "); print_num(s->tapping.brake_delay_ms); uart_puts(" ms\r\n");
    }
}

/*===========================================================================*/
/* TAP Command - Status Display (Read-Only)                                  */
/*===========================================================================*/

void cmd_tap(void) {
    const settings_t* s = settings_get();

    // Build trigger string
    uart_puts("Tap Triggers: ");
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
        uart_puts(triggers);
    } else {
        uart_puts("NONE (all disabled)");
    }
    uart_puts("\r\n");

    // Show state info
    STATE_LOCK();
    int state = (int)g_state.tap_state;
    int app = (int)g_state.state;
    bool pedal = g_state.pedal_pressed;
    bool motor_run = g_state.motor_running;
    bool motor_fwd = g_state.motor_forward;
    bool sim = g_state.sim_mode;
    int16_t sim_d = g_state.sim_depth;
    STATE_UNLOCK();

    uart_puts("TapState: "); print_num(state);
    uart_puts(", AppState: "); print_num(app);
    uart_puts("\r\nPedal: "); uart_puts(pedal ? "PRESSED" : "RELEASED");
    uart_puts(", Motor: "); uart_puts(motor_run ? "RUN" : "STOP");
    uart_puts(" "); uart_puts(motor_fwd ? "FWD" : "REV");
    if (sim) {
        uart_puts("\r\n[SIM] Depth: "); print_num(sim_d/10); uart_putc('.'); print_num((sim_d<0?-sim_d:sim_d)%10); uart_puts(" mm");
    }
    uart_puts("\r\nCommands: TAPTEST, TAPSTOP, TAPSIM\r\n");
}

/*===========================================================================*/
/* Debug Tapping Test Commands                                               */
/*===========================================================================*/

#ifdef BUILD_DEBUG
void cmd_taptest(void) {
    const settings_t* s = settings_get();
    bool any_trigger = s->tapping.depth_trigger_enabled ||
                       s->tapping.load_increase_enabled ||
                       s->tapping.load_slip_enabled ||
                       s->tapping.clutch_slip_enabled ||
                       s->tapping.quill_trigger_enabled ||
                       s->tapping.peck_trigger_enabled ||
                       s->tapping.pedal_enabled;

    if (!any_trigger) {
        uart_puts("Enable at least one trigger first (use menu)\r\n");
    } else {
        uart_puts("Starting tapping test...\r\n");
        STATE_LOCK();
        g_state.state = APP_STATE_TAPPING;
        g_state.motor_running = true;
        g_state.motor_forward = true;
        STATE_UNLOCK();
        MOTOR_CMD(CMD_MOTOR_FORWARD, 0);
        uart_puts("Motor FORWARD, waiting for trigger...\r\n");
        uart_puts("  Enabled triggers will activate automatically\r\n");
    }
}

void cmd_tapstop(void) {
    uart_puts("Stopping tapping test...\r\n");
    MOTOR_CMD(CMD_MOTOR_STOP, 0);
    STATE_LOCK();
    g_state.state = APP_STATE_IDLE;
    g_state.motor_running = false;
    g_state.tap_state = TAP_STATE_IDLE;
    STATE_UNLOCK();
    uart_puts("Motor STOPPED, state IDLE\r\n");
}

void cmd_tapsim(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx >= 8 && cmd_buf[6] == ' ') {
        char sim_cmd = cmd_buf[7];
        if (sim_cmd == 'P' || sim_cmd == 'p') {
            STATE_LOCK();
            g_state.sim_mode = true;
            g_state.pedal_pressed = !g_state.pedal_pressed;
            bool pressed = g_state.pedal_pressed;
            STATE_UNLOCK();
            uart_puts("[SIM] Pedal: "); uart_puts(pressed ? "PRESSED" : "RELEASED"); uart_puts("\r\n");
        }
        else if (sim_cmd == '+') {
            STATE_LOCK();
            g_state.sim_mode = true;
            g_state.sim_depth += 30;
            int16_t d = g_state.sim_depth;
            STATE_UNLOCK();
            uart_puts("[SIM] Depth +3mm -> "); print_num(d/10); uart_putc('.'); print_num((d<0?-d:d)%10); uart_puts(" mm\r\n");
        }
        else if (sim_cmd == '-') {
            STATE_LOCK();
            g_state.sim_mode = true;
            g_state.sim_depth -= 30;
            int16_t d = g_state.sim_depth;
            STATE_UNLOCK();
            uart_puts("[SIM] Depth -3mm -> "); print_num(d/10); uart_putc('.'); print_num((d<0?-d:d)%10); uart_puts(" mm\r\n");
        }
        else if (sim_cmd == 'X' || sim_cmd == 'x') {
            STATE_LOCK();
            g_state.sim_mode = false;
            STATE_UNLOCK();
            uart_puts("Simulation OFF (using hardware)\r\n");
        }
        else {
            uart_puts("TAPSIM: P=pedal, +=lower, -=lift, X=exit sim\r\n");
        }
    } else {
        STATE_LOCK();
        bool sim = g_state.sim_mode;
        STATE_UNLOCK();
        uart_puts("Simulation: "); uart_puts(sim ? "ON" : "OFF"); uart_puts("\r\n");
        uart_puts("Usage: TAPSIM P|+|-|X\r\n");
    }
}
#endif

/*===========================================================================*/
/* Step Drill Commands                                                       */
/*===========================================================================*/

void cmd_drill(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    // DRILL N - set enabled (0/1)
    if (cmd_idx == 7 && cmd_buf[5] == ' ') {
        int enabled = cmd_buf[6] - '0';
        if (enabled == 0 || enabled == 1) {
            settings_set_step_drill_enabled(enabled);
            uart_puts("Step drill: "); uart_puts(enabled ? "ON" : "OFF"); uart_puts("\r\n");
            return;
        }
    }

    // DRILL - show status
    const settings_t* s = settings_get();
    uart_puts("Step Drill: "); uart_puts(s->step_drill.enabled ? "ON" : "OFF");
    uart_puts("\r\n  Start dia:  "); print_num(s->step_drill.start_diameter); uart_puts(" mm");
    uart_puts("\r\n  Target dia: "); print_num(s->step_drill.target_diameter); uart_puts(" mm");
    if (s->step_drill.target_diameter == 0) uart_puts(" (disabled)");
    uart_puts("\r\n  Dia inc:    "); print_num(s->step_drill.diameter_increment); uart_puts(" mm/step");
    uart_puts("\r\n  Step depth: "); print_num(s->step_drill.step_depth_x2 * 5); uart_puts(" mm (x0.1)");
    uart_puts("\r\n  Base RPM:   "); print_num(s->step_drill.base_rpm); uart_puts("\r\n");
    uart_puts("Usage: DRILL 0|1\r\n");
}

void cmd_drillcfg(void) {
    uart_puts("Use MENU > Drill to configure step drill settings\r\n");
}
