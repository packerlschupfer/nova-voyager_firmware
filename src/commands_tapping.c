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
            /* REVIEW FIX: wrote only the volatile runtime store, so the
             * value took effect immediately and then vanished on the next
             * power cycle — SAVE reported "No changes to save" because
             * current_settings never saw it. Persist, then push. */
            settings_set_load_increase_threshold((uint8_t)val);
            settings_sync_to_tapping();
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
            settings_set_load_increase_reverse_ms((uint16_t)val);
            settings_sync_to_tapping();   /* see cmd_tapload */
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
        /* AUDIT FIX (MEDIUM, commands_tapping.c:93): the cycle count was
         * parsed but never range-checked, and settings.peck_cycles is a
         * uint8_t — so "TAPPECK 1000 500 256" truncated to 0, and 0 means
         * "peck until target depth" rather than "do nothing". An out-of-range
         * count silently became an unbounded peck cycle. The accumulators are
         * also bounded now: they were plain ints being fed an unbounded digit
         * run, which is signed overflow (UB) before any range test could see
         * it. The menu offers 0-99 for this field; match it. */
        long fwd = 0, rev = 0, cyc = 0;
        int i = 8;
        bool bad = false;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            fwd = fwd * 10 + (cmd_buf[i++] - '0');
            if (fwd > 999999L) { bad = true; break; }
        }
        if (i < cmd_idx && cmd_buf[i] == ' ') i++;
        while (!bad && i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            rev = rev * 10 + (cmd_buf[i++] - '0');
            if (rev > 999999L) { bad = true; break; }
        }
        if (i < cmd_idx && cmd_buf[i] == ' ') i++;
        while (!bad && i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            cyc = cyc * 10 + (cmd_buf[i++] - '0');
            if (cyc > 999999L) { bad = true; break; }
        }
        if (!bad &&
            fwd >= TAP_PECK_FWD_MS_MIN && fwd <= TAP_PECK_FWD_MS_MAX &&
            rev >= TAP_PECK_REV_MS_MIN && rev <= TAP_PECK_REV_MS_MAX &&
            cyc >= 0 && cyc <= TAP_PECK_CYCLES_MAX) {
            settings_set_peck_fwd_ms((uint16_t)fwd);
            settings_set_peck_rev_ms((uint16_t)rev);
            settings_set_peck_cycles((uint8_t)cyc);
            settings_sync_to_tapping();
            DEBUG_PRINT("Peck: fwd="); print_num(fwd);
            uart_puts("ms, rev="); print_num(rev);
            uart_puts("ms, cycles="); print_num(cyc); uart_puts("\r\n");
        } else {
            uart_puts("Usage: TAPPECK <fwd_ms> <rev_ms> <cyc>\r\n");
            uart_puts("  fwd: 50-5000ms, rev: 50-2000ms, cyc: 0-99 (0 = until depth)\r\n");
        }
    } else {
        const tapping_settings_t* ts = tapping_get_settings();
        DEBUG_PRINT("Peck: fwd="); print_num(ts->peck_fwd_ms);
        uart_puts("ms, rev="); print_num(ts->peck_rev_ms);
        uart_puts("ms, cycles="); print_num(ts->peck_cycles); uart_puts("\r\n");
    }
}

/* Shared by the completion-action commands. Mirrors completion_action_t. */
static const char* completion_name(uint8_t action) {
    switch (action) {
        case COMPLETION_STOP:          return "STOP";
        case COMPLETION_REVERSE_OUT:   return "REVERSE_OUT";
        case COMPLETION_REVERSE_TIMED: return "REVERSE_TIMED";
        case COMPLETION_RESUME:        return "RESUME";
        default:                       return "?";
    }
}

void cmd_tapact(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx > 7 && cmd_buf[6] == ' ') {
        int val = cmd_buf[7] - '0';
        if (val >= 0 && val <= 2) {
            settings_set_depth_completion_action((uint8_t)val);
            settings_sync_to_tapping();   /* see cmd_tapload */
            uart_puts("Depth action: "); uart_puts(completion_name((uint8_t)val)); uart_puts("\r\n");
        } else {
            uart_puts("0=stop, 1=reverse out, 2=reverse timed\r\n");
        }
    } else {
        const tapping_settings_t* ts = tapping_get_settings();
        uart_puts("Depth action: ");
        uart_puts(completion_name(ts->depth_completion_action));
        uart_puts("\r\n");
    }
}

void cmd_tapthr(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx > 7 && cmd_buf[6] == ' ') {
        int val = cmd_buf[7] - '0';
        if (val >= 0 && val <= 1) {
            /* REVIEW FIX (LOW): writes only the runtime tap_settings store —
             * never marked dirty, never persisted, and silently reverted by the
             * next settings_sync_to_tapping(). The sibling command one line up
             * persists and syncs; this one did neither. There is no
             * settings.tapping field for through-detect, so at minimum say so
             * rather than implying it was saved. */
            tapping_set_through_detect(val);
            uart_puts("(runtime only - not persisted by SAVE)\r\n");
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
            settings_sync_to_tapping();
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
        uart_puts("\r\n[SIM] Depth: "); if (sim_d < 0) uart_putc('-'); print_num((sim_d<0?-sim_d:sim_d)/10); uart_putc('.'); print_num((sim_d<0?-sim_d:sim_d)%10); uart_puts(" mm");
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

    STATE_LOCK();
    const bool armed = g_state.tapping_armed;
    STATE_UNLOCK();

    if (!any_trigger) {
        uart_puts("Enable at least one trigger first (menu, or TAPSET <trigger> 1)\r\n");
    } else if (!armed) {
        /* Refuse rather than silently arm. TAPTEST used to start the spindle
         * and print "Enabled triggers will activate automatically" while
         * g_state.tapping_armed was false, so NOTHING could ever fire: the
         * tapping task disarms on !armed and never leaves TAP_STATE_IDLE. The
         * spindle turned, the message claimed all was well, and the run was
         * dead. Arming is a deliberate operator action (F2), so say so instead
         * of doing it for them. */
        uart_puts("Tapping not ARMED - press F2 or send: ARM 1\r\n");
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
            /* "TAPSIM P" toggles; "TAPSIM P0"/"P1" set absolutely. A toggle is
             * fine for a human at a terminal and useless for an automated
             * test, which has to be able to say "pedal down" without first
             * knowing where it was — the state survives between runs. */
            bool want_toggle = true, want = false;
            if (cmd_idx >= 9 && (cmd_buf[8] == '0' || cmd_buf[8] == '1')) {
                want_toggle = false;
                want = (cmd_buf[8] == '1');
            }
            STATE_LOCK();
            g_state.sim_mode = true;
            g_state.pedal_pressed = want_toggle ? !g_state.pedal_pressed : want;
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
            uart_puts("[SIM] Depth +3mm -> "); if (d < 0) uart_putc('-'); print_num((d<0?-d:d)/10); uart_putc('.'); print_num((d<0?-d:d)%10); uart_puts(" mm\r\n");
        }
        else if (sim_cmd == '-') {
            STATE_LOCK();
            g_state.sim_mode = true;
            g_state.sim_depth -= 30;
            int16_t d = g_state.sim_depth;
            STATE_UNLOCK();
            uart_puts("[SIM] Depth -3mm -> "); if (d < 0) uart_putc('-'); print_num((d<0?-d:d)/10); uart_putc('.'); print_num((d<0?-d:d)%10); uart_puts(" mm\r\n");
        }
        else if (sim_cmd == 'X' || sim_cmd == 'x') {
            STATE_LOCK();
            g_state.sim_mode = false;
            STATE_UNLOCK();
            uart_puts("Simulation OFF (using hardware)\r\n");
        }
        else {
            uart_puts("TAPSIM: P=pedal toggle, P0/P1=set, +=lower, -=lift, X=exit\r\n");
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

/*===========================================================================*/
/* Debug: drive the tapping configuration from the console                    */
/*===========================================================================*/
/* Every tapping trigger and per-trigger action was menu-only, so testing a
 * change on the machine meant an operator standing at the panel turning an
 * encoder. These two commands make a tapping setup scriptable over the serial
 * link. Debug-only: they write settings that decide which way the spindle
 * turns, and CMD_FLAG_DEBUG blocks them at dispatch on a release build. */

typedef enum {
    TK_DEPTH = 0, TK_LDINC, TK_LDSLP, TK_CLUTCH, TK_QUILL, TK_PECK, TK_PEDAL,
    TK_DEPTHEND, TK_QUILLEND, TK_LOADEND, TK_SLIPEND, TK_PECKEND,
    TK_PEDACT, TK_CHIPMS, TK_CLUTACT, TK_CLUTMS,
    TK_REVTIM, TK_THRESH, TK_SPEED, TK_BRAKE,
    /* Not settings — live g_state fields, so a sim sweep can place the quill
     * and the target exactly instead of nudging TAPSIM +/- in 3 mm steps. */
    TK_TARGET, TK_SIMDEPTH
} tap_key_t;

static const struct { const char* name; uint8_t id; uint16_t min, max; } tap_keys[] = {
    /* trigger enables */
    {"depth",    TK_DEPTH,    0, 1},
    {"ldinc",    TK_LDINC,    0, 1},
    {"ldslp",    TK_LDSLP,    0, 1},
    {"clutch",   TK_CLUTCH,   0, 1},
    {"quill",    TK_QUILL,    0, 1},
    {"peck",     TK_PECK,     0, 1},
    {"pedal",    TK_PEDAL,    0, 1},
    /* completion actions: 0=stop 1=revout 2=revtimed 3=resume (depth has no 3) */
    {"depthend", TK_DEPTHEND, 0, 2},
    {"quillend", TK_QUILLEND, 0, 3},
    {"loadend",  TK_LOADEND,  0, 3},
    {"slipend",  TK_SLIPEND,  0, 3},
    {"peckend",  TK_PECKEND,  0, 2},
    /* pedal / clutch / load */
    {"pedact",   TK_PEDACT,   0, 1},      /* 0=hold 1=chipbreak */
    {"chipms",   TK_CHIPMS,   50, 500},
    {"clutact",  TK_CLUTACT,  0, 1},      /* 0=reverse 1=alert */
    {"clutms",   TK_CLUTMS,   50, 500},
    {"revtim",   TK_REVTIM,   50, 2000},
    {"thresh",   TK_THRESH,   10, 100},
    {"speed",    TK_SPEED,    SPEED_MIN_RPM, 500},
    {"brake",    TK_BRAKE,    50, 500},
    {"target",   TK_TARGET,   0, 2000},    /* g_state.target_depth, 0.1 mm */
    {"simdepth", TK_SIMDEPTH, 0, 2000},    /* g_state.sim_depth, 0.1 mm */
};

void cmd_tapset(void) {
    char* buf = get_cmd_buf();
    uint8_t idx = get_cmd_idx();

    if (idx < 8 || buf[6] != ' ') {
        uart_puts("Usage: TAPSET <key> <value>\r\n");
        uart_puts("Triggers (0/1): depth ldinc ldslp clutch quill peck pedal\r\n");
        uart_puts("Ends (0=stop 1=revout 2=revtime 3=resume):\r\n");
        uart_puts("  depthend(0-2) quillend loadend slipend peckend(0-2)\r\n");
        uart_puts("Values: pedact(0=hold,1=chip) chipms clutact(0=rev,1=alert)\r\n");
        uart_puts("        clutms revtim thresh speed brake\r\n");
        uart_puts("Live state: target simdepth (0.1mm units)\r\n");
        uart_puts("TAPCFG shows current values.\r\n");
        return;
    }

    int i = 7, key_start = i;
    while (i < idx && buf[i] != ' ') i++;
    if (i >= idx) { uart_puts("Missing value\r\n"); return; }
    buf[i++] = '\0';

    uint32_t value = 0;
    bool got_digit = false;
    while (i < idx && buf[i] >= '0' && buf[i] <= '9') {
        value = value * 10 + (uint32_t)(buf[i] - '0');
        i++;
        got_digit = true;
        if (value > 65535) { uart_puts("Value too large\r\n"); return; }
    }
    if (!got_digit) { uart_puts("Value must be a number\r\n"); return; }

    for (unsigned k = 0; k < sizeof(tap_keys)/sizeof(tap_keys[0]); k++) {
        if (strcmp(&buf[key_start], tap_keys[k].name) == 0) {
            if (!validate_range((int)value, tap_keys[k].min, tap_keys[k].max, NULL)) return;
            const uint16_t v = (uint16_t)value;
            const bool b = (v != 0);
            switch (tap_keys[k].id) {
                case TK_DEPTH:    settings_set_depth_trigger_enabled(b); break;
                case TK_LDINC:    settings_set_load_increase_enabled(b); break;
                case TK_LDSLP:    settings_set_load_slip_enabled(b); break;
                case TK_CLUTCH:   settings_set_clutch_slip_enabled(b); break;
                case TK_QUILL:    settings_set_quill_trigger_enabled(b); break;
                case TK_PECK:     settings_set_peck_trigger_enabled(b); break;
                case TK_PEDAL:    settings_set_pedal_enabled(b); break;
                case TK_DEPTHEND: settings_set_depth_completion_action((uint8_t)v); break;
                case TK_QUILLEND: settings_set_quill_completion_action((uint8_t)v); break;
                case TK_LOADEND:  settings_set_load_completion_action((uint8_t)v); break;
                case TK_SLIPEND:  settings_set_load_slip_completion_action((uint8_t)v); break;
                case TK_PECKEND:  settings_set_peck_completion_action((uint8_t)v); break;
                case TK_PEDACT:   settings_set_pedal_action((uint8_t)v); break;
                case TK_CHIPMS:   settings_set_pedal_chip_break_ms(v); break;
                case TK_CLUTACT:  settings_set_clutch_action((uint8_t)v); break;
                case TK_CLUTMS:   settings_set_clutch_plateau_ms(v); break;
                case TK_REVTIM:   settings_set_load_increase_reverse_ms(v); break;
                case TK_THRESH:   settings_set_load_increase_threshold((uint8_t)v); break;
                case TK_SPEED:    settings_set_tap_speed(v); break;
                case TK_BRAKE:    settings_set_brake_delay(v); break;
                /* g_state, not settings: set directly, and skip the sync. */
                case TK_TARGET:
                    STATE_LOCK(); g_state.target_depth = (int16_t)v; STATE_UNLOCK();
                    break;
                case TK_SIMDEPTH:
                    STATE_LOCK();
                    g_state.sim_mode = true;
                    g_state.sim_depth = (int16_t)v;
                    STATE_UNLOCK();
                    break;
                default: uart_puts("Unhandled key\r\n"); return;
            }
            /* Push to the runtime store task_tapping reads. Without this the
             * setting is saved and the running detector keeps the old value —
             * the exact drift settings_sync_to_tapping() exists to prevent. */
            settings_sync_to_tapping();
            uart_puts("OK ");
            uart_puts(tap_keys[k].name);
            uart_putc('=');
            print_num((int32_t)value);
            uart_puts("\r\n");
            return;
        }
    }
    uart_puts("Unknown key. TAPSET with no args lists them.\r\n");
}

static void cfg_row(const char* name, int32_t val, const char* note) {
    uart_puts("  ");
    uart_puts(name);
    uart_puts(" = ");
    print_num(val);
    if (note) { uart_puts("  "); uart_puts(note); }
    uart_puts("\r\n");
}

void cmd_tapcfg(void) {
    const settings_t* s = settings_get();
    const tapping_settings_t* t = tapping_get_settings();

    uart_puts("--- Tapping config (settings / live) ---\r\n");
    uart_puts(" Triggers:\r\n");
    cfg_row("depth ", s->tapping.depth_trigger_enabled, NULL);
    cfg_row("ldinc ", s->tapping.load_increase_enabled, NULL);
    cfg_row("ldslp ", s->tapping.load_slip_enabled, NULL);
    cfg_row("clutch", s->tapping.clutch_slip_enabled, NULL);
    cfg_row("quill ", s->tapping.quill_trigger_enabled, NULL);
    cfg_row("peck  ", s->tapping.peck_trigger_enabled, NULL);
    cfg_row("pedal ", s->tapping.pedal_enabled, NULL);
    uart_puts(" Completion actions:\r\n");
    cfg_row("depthend", s->tapping.depth_completion_action,
            completion_name(s->tapping.depth_completion_action));
    cfg_row("quillend", s->tapping.quill_completion_action,
            completion_name(s->tapping.quill_completion_action));
    cfg_row("loadend ", s->tapping.load_completion_action,
            completion_name(s->tapping.load_completion_action));
    cfg_row("slipend ", s->tapping.load_slip_completion_action,
            completion_name(s->tapping.load_slip_completion_action));
    cfg_row("peckend ", s->tapping.peck_completion_action,
            completion_name(s->tapping.peck_completion_action));
    uart_puts(" Values:\r\n");
    cfg_row("pedact", s->tapping.pedal_action,
            s->tapping.pedal_action ? "CHIP_BREAK" : "HOLD");
    cfg_row("chipms", s->tapping.pedal_chip_break_ms, NULL);
    cfg_row("clutact", s->tapping.clutch_action,
            s->tapping.clutch_action ? "ALERT" : "REVERSE");
    cfg_row("clutms", s->tapping.clutch_plateau_ms, NULL);
    cfg_row("revtim", s->tapping.load_increase_reverse_ms, NULL);
    cfg_row("thresh", s->tapping.load_increase_threshold, NULL);
    cfg_row("speed ", s->tapping.speed_rpm, NULL);
    cfg_row("brake ", s->tapping.brake_delay_ms, NULL);

    /* The live copy is what task_tapping actually reads. If these disagree
     * with the rows above, settings_sync_to_tapping() has a gap. */
    uart_puts(" Live (tapping module):\r\n");
    cfg_row("loadend ", t->load_completion_action, NULL);
    cfg_row("pedact  ", t->pedal_action, NULL);
    cfg_row("chipms  ", t->pedal_chip_break_ms, NULL);
    cfg_row("revtim  ", t->load_increase_reverse_ms, NULL);
}


/**
 * @brief ARM [0|1] - read or set the tapping armed flag absolutely.
 *
 * F2 toggles it, which an automated test cannot use: it has to know the
 * current state first, and a mis-sequenced toggle silently disarms and the
 * whole run then does nothing (TapState stays IDLE while everything else looks
 * correct). That failure is invisible without this.
 */
void cmd_arm(void) {
    char* buf = get_cmd_buf();
    uint8_t idx = get_cmd_idx();

    if (idx >= 5 && buf[3] == ' ' && (buf[4] == '0' || buf[4] == '1')) {
        const bool want = (buf[4] == '1');
        STATE_LOCK();
        g_state.tapping_armed = want;
        STATE_UNLOCK();
    }
    STATE_LOCK();
    bool armed = g_state.tapping_armed;
    STATE_UNLOCK();
    uart_puts("Tapping: ");
    uart_puts(armed ? "ARMED" : "DISARMED");
    uart_puts("\r\n");
}

/* Case-insensitive full-token compare. The OFF tests used to match on the
 * first character only, so "SIMLOAD ON" — the obvious thing to type when
 * trying to switch the simulation ON — matched 'O' and switched it OFF. */
static bool arg_is(const char* arg, const char* word) {
    while (*word) {
        char a = *arg++, w = *word++;
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (a != w) return false;
    }
    return *arg == '\0';
}

/**
 * @brief SIMLOAD [0-100|OFF] - fake the motor load percentage.
 *
 * The three load-based tapping triggers (load increase, load slip, clutch
 * slip) key off real cutting load, so until now they could only be exercised
 * by putting a tap in metal. This overrides g_state.motor_load at its single
 * publisher, so the load filter, the learned baseline and the jam detectors
 * all see the simulated figure.
 *
 * Load-increase fires on (load > baseline + threshold) and needs the baseline
 * learned first, so a useful sequence is: set a LOW load, start cutting, let
 * the baseline settle, then raise it.
 *
 * WARNING: this is not inert. A faked spike feeds the jam detectors exactly as
 * a real one does and can trip an emergency stop — which is correct behaviour,
 * and worth expecting rather than being surprised by.
 */
void cmd_simload(void) {
    char* buf = get_cmd_buf();
    uint8_t idx = get_cmd_idx();

    /* >= 9, not >= 8: "SIMLOAD " is itself 8 characters, so idx == 8 means the
     * argument is empty. (It is NOT a stale-byte hazard — serial_console.c
     * NUL-terminates cmd_buf before dispatch — it simply belongs in the usage
     * branch rather than being parsed as a zero-length number.) */
    if (idx >= 9 && buf[7] == ' ') {
        const char* arg = &buf[8];
        if (arg_is(arg, "OFF")) {
            STATE_LOCK(); g_state.sim_load_active = false; STATE_UNLOCK();
            uart_puts("Load sim OFF (using KR from the MCB)\r\n");
            return;
        }
        int val = 0; int i = 8; bool got = false;
        while (i < idx && buf[i] >= '0' && buf[i] <= '9') {
            val = val * 10 + (buf[i++] - '0'); got = true;
            if (val > 999) break;
        }
        /* Fall through to the usage text rather than returning mute: a
         * non-numeric or space-padded argument used to print nothing at all,
         * leaving the operator unable to tell whether the sim had been set. */
        if (!got) {
            uart_puts("Not a number. Usage: SIMLOAD <0-100> | SIMLOAD OFF\r\n");
            return;
        }
        if (!validate_range(val, 0, 100, "%")) return;
        STATE_LOCK();
        g_state.sim_load_active = true;
        g_state.sim_load = (uint8_t)val;
        g_state.motor_load = (uint8_t)val;   /* take effect immediately */
        STATE_UNLOCK();
        uart_puts("Load sim ON: "); print_num(val); uart_puts("%\r\n");
    } else {
        STATE_LOCK();
        bool on = g_state.sim_load_active;
        uint8_t v = g_state.sim_load, live = g_state.motor_load;
        STATE_UNLOCK();
        uart_puts("Load sim: "); uart_puts(on ? "ON " : "OFF ");
        print_num(v); uart_puts("%  live="); print_num(live);
        uart_puts("%\r\nUsage: SIMLOAD <0-100> | SIMLOAD OFF\r\n");
    }
}

/**
 * @brief SIMCV [rpm|OFF] - fake the ACTUAL spindle rpm.
 *
 * Load-slip is a CV overshoot test: actual rpm above
 * (baseline * load_slip_cv_percent / 100), where the baseline is the commanded
 * rpm snapshotted when cutting started. Faking load does nothing for it — it
 * needs a fake actual rpm, which is what this provides.
 */
void cmd_simcv(void) {
    char* buf = get_cmd_buf();
    uint8_t idx = get_cmd_idx();

    /* >= 7 for the same reason as SIMLOAD: "SIMCV " is 6 characters. */
    if (idx >= 7 && buf[5] == ' ') {
        const char* arg = &buf[6];
        if (arg_is(arg, "OFF")) {
            STATE_LOCK(); g_state.sim_cv_active = false; STATE_UNLOCK();
            uart_puts("CV sim OFF (using SV from the MCB)\r\n");
            return;
        }
        int val = 0; int i = 6; bool got = false;
        while (i < idx && buf[i] >= '0' && buf[i] <= '9') {
            val = val * 10 + (buf[i++] - '0'); got = true;
            if (val > 99999) break;
        }
        if (!got) {
            uart_puts("Not a number. Usage: SIMCV <rpm> | SIMCV OFF\r\n");
            return;
        }
        if (!validate_range(val, 0, 6000, "rpm")) return;
        STATE_LOCK();
        g_state.sim_cv_active = true;
        g_state.sim_cv = (uint16_t)val;
        STATE_UNLOCK();
        uart_puts("CV sim ON: "); print_num(val); uart_puts(" rpm\r\n");
    } else {
        STATE_LOCK();
        bool on = g_state.sim_cv_active;
        uint16_t v = g_state.sim_cv;
        STATE_UNLOCK();
        uart_puts("CV sim: "); uart_puts(on ? "ON " : "OFF ");
        print_num(v); uart_puts(" rpm\r\nUsage: SIMCV <rpm> | SIMCV OFF\r\n");
    }
}
