/**
 * @file commands_motor.c
 * @brief Motor protocol test commands
 */

#include "commands_internal.h"
#include "motor.h"
#include "motor_uart.h"
#include "safety.h"

/*===========================================================================*/
/* Helper Functions                                                          */
/*===========================================================================*/

/**
 * @brief Dump buffer as hex bytes to UART
 * @param buf Buffer to dump
 * @param len Number of bytes to dump
 */
static void dump_hex_buffer(const uint8_t* buf, int len) {
    for (int i = 0; i < len; i++) {
        print_hex_byte(buf[i]);
        uart_putc(' ');
    }
}

/*===========================================================================*/
/* Motor UART Test Functions                                                 */
/*===========================================================================*/

// Motor test - GF (get flags)
/* AUDIT FIX (HIGH, commands_motor.c:38): these helpers drive USART3 through
 * motor_putc()/motor_read_resp() with no lock and without setting
 * motor_scan_mode, so they collided with task_motor's status polling — the
 * console task builds a frame while the motor task is mid-GF, the bytes
 * interleave on the wire, and each side can read the other's reply. The locking
 * work in motor.c/task_motor.c covered the motor_* API and the motor task, but
 * not these raw console helpers, and they are production commands: GF, RS and
 * MQ are all registered with flags 0.
 *
 * The lock goes on the transaction (send + read) rather than on each caller,
 * so a new caller cannot forget it. g_motor_mutex is recursive, so nesting
 * inside a caller that already holds it is fine. */
void motor_test_gf(void) {
    MOTOR_CONTROL_LOCK();
        uart_puts("Query GF (get flags)...\r\n");
        uint8_t pkt[] = {0x04, '0', '0', '1', '1', '1', 'G', 'F', 0x05};
        uart_puts("TX: ");
        dump_hex_buffer(pkt, 9);
        uart_puts("-> ");
        for (int i = 0; i < 9; i++) motor_putc(pkt[i]);
        uint8_t resp[32];
        int len = motor_read_resp(resp, sizeof(resp));
        if (len > 0) {
            dump_hex_buffer(resp, len);
            uart_puts("\r\n");
        } else {
            uart_puts("timeout\r\n");
        }
    MOTOR_CONTROL_UNLOCK();
}

// Motor test - RS (stop)
void motor_test_rs(void) {
    MOTOR_CONTROL_LOCK();
        uart_puts("Command RS (stop)...\r\n");
        uint8_t pkt[16];
        int len = 0;
        pkt[len++] = 0x04; pkt[len++] = '0'; pkt[len++] = '0';
        pkt[len++] = '1'; pkt[len++] = '1'; pkt[len++] = 0x02;
        pkt[len++] = '1'; pkt[len++] = 'R'; pkt[len++] = 'S';
        pkt[len++] = '0'; pkt[len++] = 0x03;
        uint8_t xorsum = 0;
        for (int i = 6; i < len; i++) xorsum ^= pkt[i];
        pkt[len++] = xorsum;
        uart_puts("TX: ");
        dump_hex_buffer(pkt, len);
        uart_puts("-> ");
        for (int i = 0; i < len; i++) motor_putc(pkt[i]);
        uint8_t resp[32];
        int rlen = motor_read_resp(resp, sizeof(resp));
        if (rlen > 0) {
            dump_hex_buffer(resp, rlen);
            uart_puts("\r\n");
        } else {
            uart_puts("timeout\r\n");
        }
    MOTOR_CONTROL_UNLOCK();
}

// Motor test - try command format for parameter query (MQ command)
static void motor_test_mq(uint8_t cmd_h, uint8_t cmd_l) {
    MOTOR_CONTROL_LOCK();
        uart_puts("Query via cmd format: ");
        uart_putc(cmd_h); uart_putc(cmd_l);
        uart_puts("\r\n");

        uint8_t pkt[16];
        int len = 0;
        pkt[len++] = 0x04; pkt[len++] = '0'; pkt[len++] = '0';
        pkt[len++] = '1'; pkt[len++] = '1'; pkt[len++] = 0x02;
        pkt[len++] = '1'; pkt[len++] = cmd_h; pkt[len++] = cmd_l;
        pkt[len++] = 0x03;  // No param, just ETX
        uint8_t xorsum = 0;
        for (int i = 6; i < len; i++) xorsum ^= pkt[i];
        pkt[len++] = xorsum;

        uart_puts("TX: ");
        dump_hex_buffer(pkt, len);
        uart_puts("-> ");
        for (int i = 0; i < len; i++) motor_putc(pkt[i]);
        uint8_t resp[32];
        int rlen = motor_read_resp(resp, sizeof(resp));
        if (rlen > 0) {
            dump_hex_buffer(resp, rlen);
            uart_puts("\r\n");
        } else {
            uart_puts("timeout\r\n");
        }
    MOTOR_CONTROL_UNLOCK();
}

// Motor test - try query format (QQ command)
void motor_test_qq(uint8_t cmd_h, uint8_t cmd_l) {
    MOTOR_CONTROL_LOCK();
        uart_puts("Query format: ");
        uart_putc(cmd_h); uart_putc(cmd_l);
        uart_puts(" -> ");

        // Query format: [0x04][addr][0x31][CMD_H][CMD_L][0x05]
        uint8_t pkt[] = {0x04, '0', '0', '1', '1', '1', cmd_h, cmd_l, 0x05};
        for (int i = 0; i < 9; i++) motor_putc(pkt[i]);
        uint8_t resp[32];
        int rlen = motor_read_resp(resp, sizeof(resp));
        if (rlen > 0) {
            dump_hex_buffer(resp, rlen);
            uart_puts("\r\n");
        } else {
            uart_puts("timeout\r\n");
        }
    MOTOR_CONTROL_UNLOCK();
}

// Motor test - JOG mode (discovered 2026-01-24 in disassembly at 0x801a504)
/* Missed by the first locking pass: JOG is the frame that STARTS THE SPINDLE,
 * and it was the one raw helper still transmitting unserialised. */
static void motor_test_jog(void) {
    MOTOR_CONTROL_LOCK();
        uart_puts("Testing JOG mode (JF=3670/3669)...\r\n");

        // Step 1: Send JF=3670 (JOG_START = 0xE56 = 3670)
        uart_puts("1. JF=3670 (JOG START): ");
        uint8_t pkt[16];
        int len = 0;
        pkt[len++] = 0x04; pkt[len++] = '0'; pkt[len++] = '0';
        pkt[len++] = '1'; pkt[len++] = '1'; pkt[len++] = 0x02;
        pkt[len++] = '1'; pkt[len++] = 'J'; pkt[len++] = 'F';
        pkt[len++] = '3'; pkt[len++] = '6'; pkt[len++] = '7'; pkt[len++] = '0';  // 3670
        pkt[len++] = 0x03;
        uint8_t xorsum = 0;
        for (int i = 6; i < len; i++) xorsum ^= pkt[i];
        pkt[len++] = xorsum;
        for (int i = 0; i < len; i++) motor_putc(pkt[i]);
        uint8_t resp[32];
        int rlen = motor_read_resp(resp, sizeof(resp));
        if (rlen > 0) {
            dump_hex_buffer(resp, rlen);
            uart_puts("\r\n");
        } else {
            uart_puts("timeout\r\n");
        }

        // Step 2: Poll GF for bit 3 (0x08) - jog busy flag
        uart_puts("2. Polling GF for bit 3 clear...\r\n");
        for (int poll = 0; poll < 20; poll++) {
            for (volatile int d = 0; d < 50000; d++);  // ~50ms delay
            motor_test_qq('G', 'F');
        }

        // Step 3: Send RS=0 (stop)
        uart_puts("3. RS=0 (STOP): ");
        motor_test_rs();

        // Step 4: Send JF=3669 (JOG_END = 0xE55 = 3669)
        uart_puts("4. JF=3669 (JOG END): ");
        len = 0;
        pkt[len++] = 0x04; pkt[len++] = '0'; pkt[len++] = '0';
        pkt[len++] = '1'; pkt[len++] = '1'; pkt[len++] = 0x02;
        pkt[len++] = '1'; pkt[len++] = 'J'; pkt[len++] = 'F';
        pkt[len++] = '3'; pkt[len++] = '6'; pkt[len++] = '6'; pkt[len++] = '9';  // 3669
        pkt[len++] = 0x03;
        xorsum = 0;
        for (int i = 6; i < len; i++) xorsum ^= pkt[i];
        pkt[len++] = xorsum;
        for (int i = 0; i < len; i++) motor_putc(pkt[i]);
        rlen = motor_read_resp(resp, sizeof(resp));
        if (rlen > 0) {
            dump_hex_buffer(resp, rlen);
            uart_puts("\r\n");
        } else {
            uart_puts("timeout\r\n");
        }

        uart_puts("JOG test complete.\r\n");
    MOTOR_CONTROL_UNLOCK();
}

// Query all validated service mode commands (discovered 2026-01-24)
static void motor_test_service_cmds(void) {
    uart_puts("Querying validated service mode commands...\r\n\r\n");

    const char cmds[][3] = {
        "BF", "BN", "GR",       // Brake commands
        "VR", "VS",             // Voltage commands
        "WH", "WL", "UW",       // Warning/Under-voltage
        "TC", "MA",             // Temperature/Motor Angle
    };

    for (int i = 0; i < 10; i++) {
        uart_puts(cmds[i]); uart_puts(": ");
        motor_test_qq(cmds[i][0], cmds[i][1]);
    }
    uart_puts("\r\nDone.\r\n");
}

/*===========================================================================*/
/* Command Handlers                                                          */
/*===========================================================================*/

void cmd_gf(void) {
    motor_test_gf();
}

void cmd_rs(void) {
    motor_test_rs();
}

/* Defined further down with the MCB scan commands; declared here because the
 * MQ, sync and save commands all need the same guard and the same settle. */
static bool scan_claim_or_refuse(const char* what);
static bool scan_settle_aborted(void);

void cmd_mq(void) {
    /* REVIEW FIX (HIGH): this transmits an arbitrary MCB command frame. Gate
     * and claim it exactly like JOG. */
    if (!safety_can_start_motor()) {
        uart_puts("MQ refused: ");
        uart_puts(safety_refusal_reason());
        uart_puts("\r\n");
        return;
    }
    if (scan_claim_or_refuse("MQ")) {
        return;
    }
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();
    if (cmd_idx == 5 && cmd_buf[2] == ' ') {
        motor_test_mq(cmd_buf[3], cmd_buf[4]);
    } else {
        uart_puts("Usage: MQ XX (e.g., MQ GF)\r\n");
    }
    motor_scan_release();
}

void cmd_msync(void) {
    /* REVIEW FIX: the banner used to print BEFORE this guard, so a refused
     * MSYNC announced "Syncing motor settings to MCB..." and then immediately
     * "MSYNC refused". Guard first, like cmd_msave. */
    /* REVIEW FIX: MSYNC issues the same ~10 motor_send_command() parameter
     * writes as MSAVE and had no envelope at all. motor_send_command() takes
     * and releases g_motor_mutex per command, so task_motor's poll can slot in
     * between two of sync's writes — the exact interleaving the MSAVE comment
     * below says must not happen. Same guard, same envelope. */
    if (scan_claim_or_refuse("MSYNC")) {
        return;
    }
    uart_puts("Syncing motor settings to MCB...\r\n");
    /* 1.5 s, and abortable — a worst-case in-flight poll is ~1.25 s. */
    if (scan_settle_aborted()) return;
    motor_sync_settings();
    motor_scan_release();
    uart_puts("Done. Use MSAVE to persist to MCB EEPROM.\r\n");
}

void cmd_msave(void) {
    /* REVIEW FIX: motor_scan_mode is a bigger hammer than "pause the UART
     * poll". task_motor.c:1229 gates the WHOLE poll block on it, and
     * motor_load_update(), jam_load_update() and jam_update() are called only
     * from inside that block — so raising the flag suspends all four jam
     * detectors as well. motor_sync_and_save() runs ~0.7 s. Every other
     * motor_scan_mode user in this file (cmd_mcbscan, cmd_regscan) calls
     * scan_claim_or_refuse() first for exactly that reason; this envelope
     * was added without it. */
    if (scan_claim_or_refuse("MSAVE")) {
        return;
    }
    uart_puts("Syncing motor settings and saving to MCB EEPROM...\r\n");
    /* The envelope itself: same reason as the menu paths.
     * This writes MCB parameters from the console task while task_motor is
     * polling GF/KR on the same USART3 at 2 Hz; interleaved bytes splice the
     * command lines and motor_save_mcb_params can persist a corrupted value to
     * the MCB's EEPROM. motor_scan_mode is the only thing that pauses that
     * poll — see the identical envelope in menu.c::action_save_settings(). */
    if (scan_settle_aborted()) return;
    motor_sync_and_save();
    motor_scan_release();
    uart_puts("Done.\r\n");
}

void cmd_mread(void) {
    uart_puts("Reading MCB parameters...\r\n");

    // Request parameter read via motor task queue
    MOTOR_CMD(CMD_MOTOR_READ_PARAMS, 0);

    // Wait for motor task to complete read and populate shared state
    delay_ms(500);  // Give motor task time to read all parameters

    // Read results from shared state
    mcb_params_t params;
    STATE_LOCK();
    params.pulse_max = g_state.mcb_params.pulse_max;
    params.adv_max = g_state.mcb_params.adv_max;
    params.ir_gain = g_state.mcb_params.ir_gain;
    params.ir_offset = g_state.mcb_params.ir_offset;
    params.cur_lim = g_state.mcb_params.cur_lim;
    params.spd_rmp = g_state.mcb_params.spd_rmp;
    params.trq_rmp = g_state.mcb_params.trq_rmp;
    params.voltage_kp = g_state.mcb_params.voltage_kp;
    params.voltage_ki = g_state.mcb_params.voltage_ki;
    params.valid = g_state.mcb_params.valid;
    bool success = params.valid;
    STATE_UNLOCK();

    if (success) {
        uart_puts("PulseMax: "); print_num(params.pulse_max); uart_puts("\r\n");
        uart_puts("AdvMax:   "); print_num(params.adv_max); uart_puts("\r\n");
        uart_puts("IRGain:   "); print_num(params.ir_gain); uart_puts("\r\n");
        uart_puts("IROffset: "); print_num(params.ir_offset); uart_puts("\r\n");
        uart_puts("CurLim:   "); print_num(params.cur_lim); uart_puts("%\r\n");
        uart_puts("SpdRmp:   "); print_num(params.spd_rmp); uart_puts("\r\n");
        uart_puts("TrqRmp:   "); print_num(params.trq_rmp); uart_puts("\r\n");
        uart_puts("V_Kp:     "); print_num(params.voltage_kp); uart_puts("\r\n");
        uart_puts("V_Ki:     "); print_num(params.voltage_ki); uart_puts("\r\n");
    } else {
        uart_puts("Failed to read MCB params\r\n");
    }
}

void cmd_s2(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();
    if (cmd_idx > 3 && cmd_buf[2] == ' ') {
        // AUDIT FIX (MEDIUM, commands_motor.c:415): wide accumulator + overflow guard
        uint32_t rpm = 0;
        int i = 3;
        bool overflow = false;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            rpm = rpm * 10 + (cmd_buf[i++] - '0');
            if (rpm > 65535) { overflow = true; break; }
        }
        if (overflow) rpm = SPEED_MAX_RPM + 1;   // force range-check failure
        /* REVIEW FIX (MEDIUM): validated only against the compile-time
         * SPEED_MAX_RPM, never the operator's own settings.speed.max_limit —
         * the same gap cmd_speed() was fixed for, and worse here: S2 is the
         * MCB's reset-fallback speed, so `S2 5500` on a machine capped at
         * 800 RPM means the spindle can come back up at 5500 after an MCB
         * reset, with nothing in the HMI having asked for it. */
        const uint16_t s2_cap = settings_get()->speed.max_limit;
        if (rpm > s2_cap) {
            uart_puts("S2 above the configured max (");
            print_num(s2_cap);
            uart_puts(") - raise Speed>Max first\r\n");
            return;
        }
        if (rpm >= SPEED_MIN_RPM && rpm <= SPEED_MAX_RPM) {
            uart_puts("Sending S2=");
            print_num(rpm);
            uart_puts("\r\n");
            motor_send_speed_2(rpm);
        } else {
            uart_puts("RPM out of range (");
            print_num(SPEED_MIN_RPM); uart_puts("-"); print_num(SPEED_MAX_RPM);
            uart_puts(")\r\n");
        }
    } else {
        uart_puts("Usage: S2 <rpm>  (e.g., S2 900)\r\n");
    }
}

void cmd_kr(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();
    if (cmd_idx > 3 && cmd_buf[2] == ' ') {
        // AUDIT FIX (MEDIUM, commands_motor.c:415): wide accumulator + overflow guard.
        // Old code let "KR 256" wrap to 0 and pass the <=100 check.
        uint32_t param = 0;
        int i = 3;
        bool overflow = false;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            param = param * 10 + (cmd_buf[i++] - '0');
            if (param > 255) { overflow = true; break; }
        }
        if (overflow) param = 101;   // force range-check failure
        if (param <= 100) {
            uart_puts("Sending KR=");
            print_num(param);
            uart_puts("\r\n");
            motor_send_keep_running(param);
        } else {
            uart_puts("Parameter out of range (0-100)\r\n");
        }
    } else {
        uart_puts("Usage: KR <param>  (e.g., KR 20)\r\n");
    }
}

void cmd_cv(void) {
    uint16_t actual = motor_get_actual_rpm();
    const motor_status_t* status = motor_get_status();
    DEBUG_PRINT("Current Velocity (CV feedback):\r\n");
    uart_puts("  Actual RPM: ");
    print_num(actual);
    uart_puts("\r\n");
    uart_puts("  Target RPM: ");
    print_num(status->target_speed);
    uart_puts("\r\n");
}

void cmd_hold(void) {
    uart_puts("Starting spindle hold...\r\n");
    motor_spindle_hold();
}

void cmd_release(void) {
    uart_puts("Releasing spindle hold...\r\n");
    motor_spindle_release();
}

void cmd_jog(void) {
    /* REVIEW FIX (HIGH): JOG is the frame that STARTS THE SPINDLE, and this
     * went straight down USART3 with no safety gate and no scan claim — so it
     * never passed local_motor_start()'s in-task re-check either. It also never
     * calls motor_hardware_enable(), which is exactly the blind spot
     * scripts/check-safety-gate.sh documents about itself: a command that only
     * sends torque frames over UART is invisible to it. With PD4 already high —
     * the guard and E-Stop handlers deliberately raise it for the spindle hold
     * — typing JOG spun the spindle under an engaged E-Stop. Same shape as the
     * ALIGN bypass that shipped in v0.1.0, in a second command.
     *
     * Debug builds only, but that is where someone is leaning over the machine
     * with the guard open. */
    if (!safety_can_start_motor()) {
        uart_puts("JOG refused: ");
        uart_puts(safety_refusal_reason());
        uart_puts("\r\n");
        return;
    }
    if (scan_claim_or_refuse("JOG")) {
        return;
    }
    motor_test_jog();
    motor_scan_release();
}

void cmd_svcq(void) {
    motor_test_service_cmds();
}

void cmd_tc(void) {
    uart_puts("TC (Temperature Calibration): ");
    motor_test_qq('T', 'C');
}

void cmd_ma(void) {
    uart_puts("MA (Motor Angle): ");
    motor_test_qq('M', 'A');
}

void cmd_bf(void) {
    uart_puts("BF (Brake Forward): ");
    motor_test_qq('B', 'F');
}

void cmd_bn(void) {
    uart_puts("BN (Brake Normal): ");
    motor_test_qq('B', 'N');
}

void cmd_gr(void) {
    /* Was motor_test_qq('G','R'), i.e. the QUERY frame — which GR never
     * answers, so this command reported nothing useful for as long as it has
     * existed. GR replies to a COMMAND frame; motor_read_gr() sends one.
     *
     * The old label said "Grip/Brake" while config.h calls GR an RPS sensor
     * bitmask. Neither is confirmed, so the output states the raw byte and
     * lets the reader decide rather than asserting a meaning. */
    uint8_t raw[8] = {0};
    size_t got = 0;
    const int16_t v = motor_read_gr_ex(raw, sizeof(raw), &got);
    if (v < 0) {
        uart_puts("GR: no value (");
        print_num((int32_t)got);
        uart_puts(" bytes:");
        for (size_t i = 0; i < got && i < sizeof(raw); i++) {
            uart_putc(' ');
            print_num(raw[i]);
        }
        uart_puts(")\r\n");
        return;
    }
    uart_puts("GR: ");
    print_num(v);
    uart_puts("  bits ");
    for (int b = 7; b >= 0; b--) uart_putc((v & (1 << b)) ? '1' : '0');
    uart_puts("  (low 3 bits A=");
    uart_putc((v & 0x01) ? '1' : '0');
    uart_puts(" B=");
    uart_putc((v & 0x02) ? '1' : '0');
    uart_puts(" C=");
    uart_putc((v & 0x04) ? '1' : '0');
    uart_puts(")\r\n");
}

void cmd_wh(void) {
    uart_puts("WH (Warning High): ");
    motor_test_qq('W', 'H');
}

void cmd_wl(void) {
    uart_puts("WL (Warning Low): ");
    motor_test_qq('W', 'L');
}

void cmd_start(void) {
    // AUDIT FIX (CRITICAL, commands_motor.c:386): the console START used to
    // bypass the E-Stop / guard / ERROR interlocks that the physical START
    // button enforces. Now every start path goes through the same gate.
    if (!safety_can_start_motor()) {
        uart_puts("START refused: ");
        uart_puts(safety_refusal_reason());
        uart_puts("\r\n");
        return;
    }
    uart_puts("Starting motor...\r\n");
    STATE_LOCK();
    uint16_t rpm = g_state.target_rpm;
    g_state.state = APP_STATE_DRILLING;
    g_state.motor_running = true;
    g_state.motor_forward = true;
    STATE_UNLOCK();
    MOTOR_CMD(CMD_MOTOR_SET_SPEED, rpm);
    MOTOR_CMD(CMD_MOTOR_FORWARD, 0);
    DEBUG_PRINT("Motor running\r\n");
}

void cmd_stop(void) {
    uart_puts("Stopping motor...\r\n");
    /* REVIEW FIX (MEDIUM): same shape as task_tapping's stops — the plain
     * MOTOR_CMD gives up silently on a full queue while the lines below
     * publish IDLE and the console prints "Motor stopped". STOP is the one
     * console command an operator issues because they want the spindle to
     * stop; it has to be the send that falls back to a hardware cutoff. */
    MOTOR_CMD_SEND_CRITICAL(CMD_MOTOR_STOP, 0);
    STATE_LOCK();
    g_state.state = APP_STATE_IDLE;
    g_state.motor_running = false;
    STATE_UNLOCK();
    DEBUG_PRINT("Motor stopped\r\n");
}

void cmd_speed(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();
    if (cmd_idx > 6 && cmd_buf[5] == ' ') {
        // AUDIT FIX (MEDIUM, commands_motor.c:415): accumulate into uint32_t
        // and reject on overflow BEFORE narrowing to uint16_t. Old code let
        // "SPEED 68036" wrap to 2500, pass the 50-5500 range check, and
        // silently jump the running motor.
        uint32_t rpm = 0;
        int i = 6;
        bool overflow = false;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            rpm = rpm * 10 + (cmd_buf[i++] - '0');
            if (rpm > 65535) { overflow = true; break; }
        }
        if (overflow || rpm > SPEED_MAX_RPM) {
            uart_puts("RPM out of range (");
            print_num(SPEED_MIN_RPM); uart_puts("-"); print_num(SPEED_MAX_RPM);
            uart_puts(")\r\n");
            return;
        }
        /* REVIEW FIX (HIGH): validated only against the compile-time
         * SPEED_MAX_RPM (5500), never against the operator's own
         * settings.speed.max_limit — which the encoder path honours
         * (events.c) and settings_set_speed() clamps to. A 800 RPM ceiling set
         * for a hole saw could be overridden with `SPEED 5500`, and because
         * cmd_start re-reads g_state.target_rpm the over-limit value survived a
         * STOP/START cycle. */
        const uint16_t cap = settings_get()->speed.max_limit;
        if (rpm > cap) {
            uart_puts("RPM above the configured max (");
            print_num(cap);
            uart_puts(") - raise Speed>Max first\r\n");
            return;
        }
        if (rpm >= SPEED_MIN_RPM && rpm <= SPEED_MAX_RPM) {
            uart_puts("Setting speed to ");
            print_num(rpm);
            uart_puts(" RPM\r\n");
            STATE_LOCK();
            g_state.target_rpm = rpm;
            STATE_UNLOCK();
            /* The console SPEED command is the operator choosing, same as the
             * encoder — it takes the identical path through g_state. */
            settings_note_operator_speed((uint16_t)rpm, HAL_GetTick());
            MOTOR_CMD(CMD_MOTOR_SET_SPEED, rpm);
        } else {
            uart_puts("RPM out of range (");
            print_num(SPEED_MIN_RPM); uart_puts("-"); print_num(SPEED_MAX_RPM);
            uart_puts(")\r\n");
        }
    } else {
        const motor_status_t* status = motor_get_status();
        DEBUG_PRINT("Current target: ");
        print_num(status->target_speed);
        uart_puts(" RPM\r\nUsage: SPEED <rpm>\r\n");
    }
}

void cmd_power(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    // Parse argument if provided
    if (cmd_idx > 6 && cmd_buf[5] == ' ') {
        motor_power_t level = MOTOR_POWER_HIGH;  // Default

        // Check for level name (case insensitive)
        char arg = cmd_buf[6];
        if (arg == 'L' || arg == 'l') {
            level = MOTOR_POWER_LOW;
        } else if (arg == 'M' || arg == 'm') {
            if (cmd_idx > 7 && (cmd_buf[7] == 'A' || cmd_buf[7] == 'a')) {
                level = MOTOR_POWER_MAX;  // MAX
            } else {
                level = MOTOR_POWER_MED;  // MED
            }
        } else if (arg == 'H' || arg == 'h') {
            level = MOTOR_POWER_HIGH;
        } else if (arg >= '0' && arg <= '9') {
            /* AUDIT FIX (LOW, commands_motor.c:492): accumulated into a
             * uint8_t, so "POWER 276" wrapped to 20 and silently selected LOW
             * instead of being rejected — the same overflow class already
             * fixed in cmd_speed above. Accumulate wide and reject. */
            uint32_t val = 0;
            int i = 6;
            bool overflow = false;
            while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
                val = val * 10 + (uint32_t)(cmd_buf[i++] - '0');
                if (val > 1000) { overflow = true; break; }
            }
            if (overflow) {
                uart_puts("Invalid power level. Use LOW/MED/HIGH/MAX or 20/50/70/100\r\n");
                return;
            }
            if (val == 20) level = MOTOR_POWER_LOW;
            else if (val == 50) level = MOTOR_POWER_MED;
            else if (val == 70) level = MOTOR_POWER_HIGH;
            else if (val == 100) level = MOTOR_POWER_MAX;
            else {
                uart_puts("Invalid power level. Use LOW/MED/HIGH/MAX or 20/50/70/100\r\n");
                return;
            }
        } else {
            uart_puts("Invalid power level. Use LOW/MED/HIGH/MAX\r\n");
            return;
        }

        /* REVIEW FIX: the result was discarded, so a failed commit or a CL
         * readback mismatch printed its warning inside motor_set_power_level()
         * and then POWER itself said nothing — the operator's last line was the
         * level they asked for. */
        if (!motor_set_power_level(level)) {
            uart_puts("POWER: level NOT applied - see the warning above\r\n");
        }
    } else {
        // No argument - show current level and usage
        DEBUG_PRINT("Motor Power Levels (discovered 2026-01-25):\r\n");
        uart_puts("  LOW  (20%): Light materials - WARNING: may stall at low RPM!\r\n");
        uart_puts("  MED  (50%): General drilling\r\n");
        uart_puts("  HIGH (70%): Heavy-duty (factory default)\r\n");
        uart_puts("  MAX (100%): Full torque\r\n");
        uart_puts("\r\nUsage: POWER [LOW|MED|HIGH|MAX]\r\n");
        uart_puts("       POWER [20|50|70|100]\r\n");
    }
}

void cmd_mreset(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    // Require confirmation argument "CONFIRM" for safety
    if (cmd_idx >= 14 && cmd_buf[7] == 'C' && cmd_buf[8] == 'O' &&
        cmd_buf[9] == 'N' && cmd_buf[10] == 'F' && cmd_buf[11] == 'I' &&
        cmd_buf[12] == 'R' && cmd_buf[13] == 'M') {

        uart_puts("\r\n");
        uart_puts("**************************************************\r\n");
        uart_puts("*    MCB FACTORY RESET - ERASING EEPROM          *\r\n");
        uart_puts("*    DO NOT POWER OFF DURING RESET!              *\r\n");
        uart_puts("**************************************************\r\n\r\n");

        // Perform factory reset
        bool success = motor_factory_reset();

        if (success) {
            uart_puts("\r\nFactory reset complete.\r\n");
            DEBUG_PRINT("Motor parameters restored to factory defaults.\r\n");
            uart_puts("Run MREAD to verify settings.\r\n");
        } else {
            uart_puts("\r\nFactory reset may have failed.\r\n");
            uart_puts("Power cycle the drill press and try again.\r\n");
        }
    } else {
        DEBUG_PRINT("MCB Factory Reset (discovered 2026-01-25)\r\n");
        uart_puts("=========================================\r\n");
        uart_puts("This command resets the motor controller EEPROM to factory defaults.\r\n\r\n");
        uart_puts("WARNING: This will ERASE all motor tuning parameters!\r\n");
        uart_puts("         You will need to re-tune IR gain, PID values, etc.\r\n\r\n");
        uart_puts("To confirm, type: MRESET CONFIRM\r\n");
    }
}

void cmd_cvcheck(void) {
    uart_puts("CV Confidence Check (3x rapid query)...\r\n");
    uint16_t avg_cv = motor_cv_confidence_check();
    uart_puts("Average CV: ");
    print_num(avg_cv);
    uart_puts(" RPM\r\n");
}

// Test EEPROM save sequence (discovered 2026-01-25)
// SE command takes parameter NAME (command code) as its value!
// E.g., SE=I3 sends motor_send_command(CMD_SE, 0x4933)
void cmd_eepromtest(void) {
    uart_puts("\r\n");
    uart_puts("=== EEPROM SAVE TEST (discovered 2026-01-25) ===\r\n");
    uart_puts("Testing: I3 (IR Offset) = 5\r\n\r\n");

    // Step 1: Query current I3 value
    uart_puts("1. Query I3 initial value: ");
    int32_t initial = motor_read_param(CMD_I3);
    print_num(initial);
    uart_puts("\r\n");

    // Step 2: Set I3=5
    uart_puts("2. Set I3=5...\r\n");
    motor_send_command(CMD_I3, 5);
    for (volatile int d = 0; d < 50000; d++);  // Brief delay

    // Step 3: Commit with SE=I3 (SE takes param CODE as value!)
    uart_puts("3. Commit SE=I3 (SE=0x4933)...\r\n");
    motor_send_command(CMD_SE, CMD_I3);  // SE with I3's command code
    for (volatile int d = 0; d < 50000; d++);

    // Step 4: Verify in RAM
    uart_puts("4. Verify I3 in RAM: ");
    int32_t ram_val = motor_read_param(CMD_I3);
    print_num(ram_val);
    uart_puts("\r\n");

    // Step 5: RS=1 x3 (EEPROM prep - like factory reset uses)
    uart_puts("5. EEPROM prep (RS=1 x3)...\r\n");
    for (int i = 0; i < 3; i++) {
        motor_send_command(CMD_STOP, 1);  // RS=1
        for (volatile int d = 0; d < 100000; d++);
    }

    // Step 6: EE=0 (Execute EEPROM write)
    uart_puts("6. Execute EEPROM write (EE=0)...\r\n");
    motor_send_command(CMD_EE, 0);
    for (volatile int d = 0; d < 500000; d++);  // Wait for EEPROM write

    // Step 7: RS=1 to finalize (like factory reset)
    uart_puts("7. Finalize (RS=1)...\r\n");
    motor_send_command(CMD_STOP, 1);
    for (volatile int d = 0; d < 100000; d++);

    // Step 8: Verify after EEPROM write
    uart_puts("8. Verify I3 after EEPROM: ");
    int32_t final_val = motor_read_param(CMD_I3);
    print_num(final_val);
    uart_puts("\r\n\r\n");

    if (final_val == 5) {
        uart_puts("SUCCESS: I3 saved to EEPROM!\r\n");
    } else {
        uart_puts("FAILED: I3 did not persist (got ");
        print_num(final_val);
        uart_puts(", expected 5)\r\n");
        uart_puts("\r\nPossible issues:\r\n");
        uart_puts("  - EE command may need different parameters\r\n");
        uart_puts("  - RS=1 sequence may be wrong\r\n");
        uart_puts("  - MCB EEPROM may be write-protected\r\n");
    }

    uart_puts("\r\nTest complete. Power cycle to verify persistence.\r\n");
}

// Send raw SE command with parameter code
void cmd_se(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx >= 5 && cmd_buf[2] == ' ') {
        // Parse 2-char parameter name (e.g., "SE CL" or "SE I3")
        uint8_t h = cmd_buf[3];
        uint8_t l = cmd_buf[4];
        uint16_t param_code = ((uint16_t)h << 8) | l;

        uart_puts("Sending SE=");
        uart_putc(h);
        uart_putc(l);
        uart_puts(" (0x");
        print_hex_byte(h);
        print_hex_byte(l);
        uart_puts(")...\r\n");

        motor_send_command(CMD_SE, param_code);
        uart_puts("Done.\r\n");
    } else {
        uart_puts("SE (Set Enable) - Commit parameter change to RAM\r\n");
        uart_puts("Usage: SE <param>  (e.g., SE CL, SE I3)\r\n");
        uart_puts("\r\nDiscovered 2026-01-25: SE takes the parameter's\r\n");
        uart_puts("command code as its value (e.g., SE=I3 sends 0x4933)\r\n");
    }
}

// Query new service mode parameters (discovered 2026-01-25)
void cmd_sp(void) {
    uart_puts("SP (Kprop/Proportional Gain): ");
    int32_t val = motor_read_param(0x5350);  // SP
    print_num(val);
    uart_puts(" (100% = 1000)\r\n");
}

void cmd_si(void) {
    uart_puts("SI (Kint/Integral Gain): ");
    int32_t val = motor_read_param(CMD_SI);
    print_num(val);
    uart_puts(" (50% = 500)\r\n");
}

void cmd_i3(void) {
    uart_puts("I3 (IR Offset): ");
    int32_t val = motor_read_param(CMD_I3);
    print_num(val);
    uart_puts("\r\n");
}

void cmd_i0(void) {
    uart_puts("I0 (IR Gain): ");
    int32_t val = motor_read_param(CMD_I0);
    print_num(val);
    uart_puts("\r\n");
}

void cmd_spdrmp(void) {
    uart_puts("DN (Speed Ramp): ");
    int32_t val = motor_read_param(0x444E);  // DN = 0x444E
    print_num(val);
    uart_puts("\r\n");
}

void cmd_nc(void) {
    uart_puts("NC (Speed Advance Max): ");
    int32_t val = motor_read_param(CMD_NC);
    print_num(val);
    uart_puts("\r\n");
}

void cmd_ud(void) {
    uart_puts("UD (DC Bus Voltage): ");
    int32_t val = motor_read_param(CMD_UD);
    print_num(val);
    uart_puts(" V\r\n");
}

void cmd_t0(void) {
    uart_puts("T0 (Heatsink Temp): ");
    int32_t val = motor_read_param(CMD_T0);
    print_num(val);
    uart_puts(" C\r\n");
}

void cmd_su(void) {
    uart_puts("SU (PulseMax): ");
    int32_t val = motor_read_param(CMD_SU);
    print_num(val);
    uart_puts("\r\n");
}

void cmd_ts(void) {
    uart_puts("TS (Undervoltage Stop): ");
    int32_t val = motor_read_param(CMD_TS);
    print_num(val);
    uart_puts(" V\r\n");
}

void cmd_uv(void) {
    uart_puts("UV (Undervoltage Run): ");
    int32_t val = motor_read_param(CMD_UV);
    print_num(val);
    uart_puts(" V\r\n");
}

// MCB Command Scanner - systematically tests all 2-letter commands
// This discovers all valid MCB commands by querying each possible combination

// From task_motor.c - pause motor polling during scan

// A raw-path scan blocks the main task (and therefore event handling) for tens
// of seconds. Neither scan may run with the spindle turning: a STOP press would
// sit unread in g_event_queue until the scan finished.
/* Claim the MCB envelope for a console command, or explain why not.
 *
 * REVIEW FIX: the block that used to live here still described the
 * claim-then-back-out algorithm ("the flag is raised FIRST and lowered again if
 * the sample says no") that motor_scan_try_claim() was written to REMOVE — so
 * anyone reading this file alone was told the invariant was the broken one.
 * The claim is atomic inside task_motor.c now; this is only the message.
 *
 * @return true if refused. false if claimed — caller must motor_scan_release().
 */
static bool scan_claim_or_refuse(const char* what) {
    const motor_scan_result_t r = motor_scan_try_claim();
    if (r == MOTOR_SCAN_CLAIMED) {
        return false;
    }
    /* REVIEW FIX: every failure used to print "machine is running — STOP
     * first", so a console MSYNC during a front-panel Save Settings told the
     * operator to stop an already-stopped spindle. */
    uart_puts(what);
    uart_puts(" refused: ");
    uart_puts(motor_scan_refusal(r));
    uart_puts("\r\n");
    return true;
}


// Let task_motor finish any in-flight poll before we take over the motor UART.
// AUDIT FIX (LOW, commands_motor.c:750): a worst-case in-flight poll is
// 5xMOTOR_RESPONSE_TIMEOUT_MS ~= 1.25 s, so the settle is 1.5 s.
//
// That sits well inside the 5 s IWDG window and so needs no pump to stay alive
// — but it is still 1.5 s of a blocked main task, so pump anyway to give the
// operator (and a queued safety event) a way out of it. Clears motor_scan_mode
// on abort so the caller can just return.
static bool scan_settle_aborted(void) {
    /* REVIEW FIX: was a hardcoded 15 x 100 ms while shared.h's
     * MOTOR_SCAN_SETTLE_MS cites THIS function as the authority for its value —
     * one number in two places, so changing the constant would have left the
     * console paths on the old settle. */
    for (int i = 0; i < (int)(MOTOR_SCAN_SETTLE_MS / 100); i++) {
        delay_ms(100);
        if (console_pump()) {
            motor_scan_release();
            uart_puts("\r\n*** ABORTED ***\r\n");
            return true;
        }
    }
    return false;
}

void cmd_mcbscan(void) {
    if (scan_claim_or_refuse("MCBSCAN")) return;

    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    // Check for optional arguments: MCBSCAN [start_letter] [fast]
    char start = 'A';
    bool fast_mode = false;

    if (cmd_idx > 8 && cmd_buf[7] == ' ') {
        // Parse start letter
        /* REVIEW FIX: the 'F'/'f' branch was unreachable — both letter ranges
         * above already match it, so "MCBSCAN F" scanned from register F at
         * full speed instead of fast-scanning from A. Test for the fast flag
         * first; a scan genuinely starting at F is "MCBSCAN F F". */
        char arg = cmd_buf[8];
        if (arg == 'F' || arg == 'f') {
            fast_mode = true;
        } else if (arg >= 'A' && arg <= 'Z') {
            start = arg;
        } else if (arg >= 'a' && arg <= 'z') {
            start = arg - 32;  // to uppercase
        }
        // Check for second arg
        if (cmd_idx > 10 && cmd_buf[9] == ' ') {
            if (cmd_buf[10] == 'F' || cmd_buf[10] == 'f') {
                fast_mode = true;
            }
        }
    }

    uart_puts("\r\n");
    uart_puts("=== MCB COMMAND SCANNER ===\r\n");
    uart_puts("Pausing motor task polling...\r\n");

    // Pause motor task polling to prevent conflicts
    // AUDIT FIX (LOW, commands_motor.c:750): the old 100 ms handshake was
    // shorter than a worst-case in-flight motor-task poll (5×MOTOR_RESPONSE_TIMEOUT
    // ≈ 1.25 s). 1500 ms covers the worst case.
    if (scan_settle_aborted()) return;

    uart_puts("Scanning all 2-letter commands from ");
    uart_putc(start);
    uart_puts("A to ZZ\r\n");
    if (fast_mode) {
        uart_puts("FAST mode: only showing responding commands\r\n");
    }
    uart_puts("Press ESC/Ctrl-C or trigger an event (guard/estop/stop) to abort.\r\n\r\n");

    int found = 0;
    int tested = 0;
    int timeout_count = 0;
    bool aborted = false;  // MISRA C:2012 Rule 14.4 - flag instead of goto

    // Scan AA through ZZ, plus A0-Z9
    for (char c1 = start; c1 <= 'Z' && !aborted; c1++) {
        // Show progress
        uart_puts("Scanning ");
        uart_putc(c1);
        uart_puts("x commands...\r\n");

        // First scan letters (XA-XZ)
        for (char c2 = 'A'; c2 <= 'Z' && !aborted; c2++) {
            // Gated watchdog refresh + abort on keypress or pending event
            if (console_pump()) {
                uart_puts("\r\n*** ABORTED ***\r\n");
                aborted = true;
                break;
            }

            tested++;

            // Send query
            uint8_t pkt[] = {0x04, '0', '0', '1', '1', '1', (uint8_t)c1, (uint8_t)c2, 0x05};
            for (int i = 0; i < 9; i++) motor_putc(pkt[i]);

            // Wait for response with short timeout
            uint8_t resp[32];
            int rlen = motor_read_resp(resp, sizeof(resp));

            if (rlen > 0) {
                // Got a response - check if it's NAK or valid data
                if (rlen == 1 && resp[0] == 0x15) {
                    // NAK - command not recognized, skip
                    if (!fast_mode) {
                        uart_putc(c1); uart_putc(c2); uart_puts(": NAK\r\n");
                    }
                } else if (rlen == 1 && resp[0] == 0x06) {
                    // ACK - command accepted (no data response)
                    found++;
                    uart_puts(">>> ");
                    uart_putc(c1); uart_putc(c2);
                    uart_puts(": ACK (accepted)\r\n");
                } else if (rlen >= 3 && resp[0] == 0x00 && resp[2] == 0x02) {
                    // Valid data response
                    found++;
                    uart_puts(">>> ");
                    uart_putc(c1); uart_putc(c2);
                    uart_puts("=");
                    // Extract value (between STX and ETX)
                    for (int i = 5; i < rlen && resp[i] != 0x03; i++) {
                        if (resp[i] >= 0x20 && resp[i] <= 0x7E) {
                            uart_putc(resp[i]);
                        }
                    }
                    uart_puts("\r\n");
                } else {
                    // Some other response
                    found++;
                    uart_puts(">>> ");
                    uart_putc(c1); uart_putc(c2);
                    uart_puts(": ");
                    for (int i = 0; i < rlen; i++) {
                        print_hex_byte(resp[i]);
                        uart_putc(' ');
                    }
                    uart_puts("\r\n");
                }
            } else {
                // Timeout
                timeout_count++;
                if (!fast_mode) {
                    uart_putc(c1); uart_putc(c2); uart_puts(": timeout\r\n");
                }
            }

            // Brief delay between queries
            delay_ms(20);
        }

        // Then scan digits (X0-X9)
        for (char c2 = '0'; c2 <= '9' && !aborted; c2++) {
            if (console_pump()) {
                uart_puts("\r\n*** ABORTED ***\r\n");
                aborted = true;
                break;
            }

            tested++;

            uint8_t pkt[] = {0x04, '0', '0', '1', '1', '1', (uint8_t)c1, (uint8_t)c2, 0x05};
            for (int i = 0; i < 9; i++) motor_putc(pkt[i]);

            uint8_t resp[32];
            int rlen = motor_read_resp(resp, sizeof(resp));

            if (rlen > 0 && !(rlen == 1 && resp[0] == 0x15)) {
                if (rlen == 1 && resp[0] == 0x06) {
                    found++;
                    uart_puts(">>> ");
                    uart_putc(c1); uart_putc(c2);
                    uart_puts(": ACK\r\n");
                } else if (rlen >= 3 && resp[0] == 0x00 && resp[2] == 0x02) {
                    found++;
                    uart_puts(">>> ");
                    uart_putc(c1); uart_putc(c2);
                    uart_puts("=");
                    for (int i = 5; i < rlen && resp[i] != 0x03; i++) {
                        if (resp[i] >= 0x20 && resp[i] <= 0x7E) {
                            uart_putc(resp[i]);
                        }
                    }
                    uart_puts("\r\n");
                } else if (rlen > 0) {
                    found++;
                    uart_puts(">>> ");
                    uart_putc(c1); uart_putc(c2);
                    uart_puts(": ");
                    for (int i = 0; i < rlen; i++) {
                        print_hex_byte(resp[i]);
                        uart_putc(' ');
                    }
                    uart_puts("\r\n");
                }
            }

            delay_ms(20);
        }
    }

    // Restore motor task polling
    motor_scan_release();

    uart_puts("\r\n=== SCAN COMPLETE ===\r\n");
    uart_puts("Tested: "); print_num(tested); uart_puts(" commands\r\n");
    uart_puts("Found:  "); print_num(found); uart_puts(" responding commands\r\n");
    uart_puts("Timeouts: "); print_num(timeout_count); uart_puts("\r\n");
    DEBUG_PRINT("Motor polling resumed.\r\n");
}

#if defined(BUILD_READONLY) || defined(BUILD_DEBUG)
/*===========================================================================*/
/* Read-only MCB diagnostics (demo/debug builds)                             */
/*===========================================================================*/

// One-shot live health summary. Reads the values the motor task already polls
// into g_state (GF/CV/KR/UD/F0) — no extra UART traffic, no collision with the
// task's polling, always consistent with what the UI shows.
void cmd_diag(void) {
    extern uint16_t temp_get_mcb(void);

    STATE_LOCK();
    bool running  = g_state.motor_running;
    bool fwd      = g_state.motor_forward;
    uint16_t crpm = g_state.current_rpm;
    uint16_t trpm = g_state.target_rpm;
    uint16_t load = g_state.motor_load;
    uint16_t volts = g_state.dc_bus_voltage;
    uint8_t fault = g_state.fault_code;
    bool mfault   = g_state.motor_fault;
    STATE_UNLOCK();
    uint16_t temp = temp_get_mcb();

    uart_puts("=== MCB DIAG (live) ===\r\n");
    uart_puts("  State : ");
    uart_puts(running ? (fwd ? "RUN FWD" : "RUN REV") : "STOPPED");
    uart_puts("\r\n");
    uart_puts("  RPM   : "); print_num(crpm); uart_puts(" / "); print_num(trpm);
    uart_puts(" (act/tgt)\r\n");
    uart_puts("  Load  : "); print_num(load);  uart_puts(" %\r\n");
    uart_puts("  DC bus: "); print_num(volts); uart_puts(" V\r\n");
    uart_puts("  Temp  : "); print_num(temp);  uart_puts(" C\r\n");
    uart_puts("  Fault : "); print_num(fault);
    uart_puts(mfault ? " (FAULT)\r\n" : "\r\n");
    uart_puts("=== END DIAG ===\r\n");
}

// Curated MCB register scan (ported from motor_test). Raw-path query under the
// motor_scan_mode envelope so the motor task's periodic poll doesn't collide.
void cmd_regscan(void) {
    if (scan_claim_or_refuse("REGSCAN")) return;

    typedef struct { char h; char l; const char* desc; } reg_t;
    static const reg_t regs[] = {
        {'G','F',"flags"},     {'F','0',"fault"},     {'G','V',"version"},   {'G','R',"brake"},
        {'S','V',"speed set"}, {'C','V',"speed act"}, {'S','2',"speed2"},
        {'K','R',"load KR"},   {'L','P',"load LP"},   {'L','D',"load thr"},  {'C','L',"cur lim"},
        {'I','U',"IR gain"},   {'O','V',"IR offset"}, {'I','0',"IR p0"},     {'I','3',"IR p3"},
        {'P','U',"pulse max"}, {'U','D',"DC bus V"},  {'V','P',"volt Kp"},   {'V','I',"volt Ki"},
        {'V','R',"volt ramp"}, {'T','0',"temp base"}, {'T','H',"temp hi"},   {'T','S',"temp sens"},
        {'S','R',"spd ramp"},  {'T','R',"trq ramp"},  {'S','A',"adv max"},   {'M','A',"motor ang"},
        {'B','R',"brake mode"},{'M','R',"motor rdy"}, {'F','D',"fault det"}, {'N','C',"norm chk"},
        {'S','P',"Kprop"},     {'S','I',"Kint"},
        {'S','0',"prof S0"},   {'S','1',"prof S1"},   {'S','3',"prof S3"},   {'S','4',"prof S4"},
        {'S','5',"prof S5"},   {'S','6',"prof S6"},   {'S','9',"prof S9"},
        {'U','H',"UV high"},   {'U','L',"UV low"},    {'U','V',"UV value"},  {'U','W',"UV warn"},
    };
    const int nregs = (int)(sizeof(regs) / sizeof(regs[0]));

    uart_puts("\r\n=== MCB REGISTER SCAN ===\r\n");
    if (scan_settle_aborted()) return;

    int responded = 0, silent = 0;
    bool aborted = false;
    for (int i = 0; i < nregs && !aborted; i++) {
        if (console_pump()) {
            uart_puts("\r\n*** ABORTED ***\r\n");
            aborted = true;
            break;
        }
        motor_uart_flush_rx();
        uint8_t pkt[] = {0x04, '0', '0', '1', '1', '1',
                         (uint8_t)regs[i].h, (uint8_t)regs[i].l, 0x05};
        for (int b = 0; b < 9; b++) motor_putc(pkt[b]);
        uint8_t resp[32];
        int rlen = motor_read_resp(resp, sizeof(resp));
        if (rlen > 0) {
            uart_puts("  ");
            uart_putc(regs[i].h); uart_putc(regs[i].l);
            uart_puts(" "); uart_puts(regs[i].desc); uart_puts(": ");
            dump_hex_buffer(resp, rlen);
            uart_puts("\r\n");
            responded++;
        } else {
            silent++;
        }
    }

    motor_scan_release();
    uart_puts("\r\n"); print_num(responded); uart_puts(" responded, ");
    print_num(silent); uart_puts(" silent\r\n=== SCAN COMPLETE ===\r\n");
}
#endif  // BUILD_READONLY || BUILD_DEBUG
