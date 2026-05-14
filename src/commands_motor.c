/**
 * @file commands_motor.c
 * @brief Motor protocol test commands
 */

#include "commands_internal.h"
#include "motor.h"

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
void motor_test_gf(void) {
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
}

// Motor test - RS (stop)
void motor_test_rs(void) {
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
}

// Motor test - try command format for parameter query (MQ command)
static void motor_test_mq(uint8_t cmd_h, uint8_t cmd_l) {
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
}

// Motor test - try query format (QQ command)
void motor_test_qq(uint8_t cmd_h, uint8_t cmd_l) {
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
}

// Motor test - JOG mode (discovered 2026-01-24 in disassembly at 0x801a504)
static void motor_test_jog(void) {
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

void cmd_mq(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();
    if (cmd_idx == 5 && cmd_buf[2] == ' ') {
        motor_test_mq(cmd_buf[3], cmd_buf[4]);
    } else {
        uart_puts("Usage: MQ XX (e.g., MQ GF)\r\n");
    }
}

void cmd_msync(void) {
    uart_puts("Syncing motor settings to MCB...\r\n");
    motor_sync_settings();
    uart_puts("Done. Use MSAVE to persist to MCB EEPROM.\r\n");
}

void cmd_msave(void) {
    uart_puts("Syncing motor settings and saving to MCB EEPROM...\r\n");
    motor_sync_and_save();
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
        uint16_t rpm = 0;
        int i = 3;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            rpm = rpm * 10 + (cmd_buf[i++] - '0');
        }
        if (rpm >= 50 && rpm <= 5000) {
            uart_puts("Sending S2=");
            print_num(rpm);
            uart_puts("\r\n");
            motor_send_speed_2(rpm);
        } else {
            uart_puts("RPM out of range (50-5000)\r\n");
        }
    } else {
        uart_puts("Usage: S2 <rpm>  (e.g., S2 900)\r\n");
    }
}

void cmd_kr(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();
    if (cmd_idx > 3 && cmd_buf[2] == ' ') {
        uint8_t param = 0;
        int i = 3;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            param = param * 10 + (cmd_buf[i++] - '0');
        }
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
    motor_test_jog();
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
    uart_puts("GR (Grip/Brake): ");
    motor_test_qq('G', 'R');
}

void cmd_wh(void) {
    uart_puts("WH (Warning High): ");
    motor_test_qq('W', 'H');
}

void cmd_wl(void) {
    uart_puts("WL (Warning Low): ");
    motor_test_qq('W', 'L');
}

void cmd_uw(void) {
    uart_puts("UW (Under-voltage Warning): ");
    motor_test_qq('U', 'W');
}

void cmd_start(void) {
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
    MOTOR_CMD(CMD_MOTOR_STOP, 0);
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
        uint16_t rpm = 0;
        int i = 6;
        while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
            rpm = rpm * 10 + (cmd_buf[i++] - '0');
        }
        if (rpm >= 50 && rpm <= 3000) {
            uart_puts("Setting speed to ");
            print_num(rpm);
            uart_puts(" RPM\r\n");
            motor_set_speed(rpm);
        } else {
            uart_puts("RPM out of range (50-3000)\r\n");
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
            // Numeric value
            uint8_t val = 0;
            int i = 6;
            while (i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9') {
                val = val * 10 + (cmd_buf[i++] - '0');
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

        // Set power level with SE commit
        motor_set_power_level(level);
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
extern volatile bool motor_scan_mode;

void cmd_mcbscan(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    // Check for optional arguments: MCBSCAN [start_letter] [fast]
    char start = 'A';
    bool fast_mode = false;

    if (cmd_idx > 8 && cmd_buf[7] == ' ') {
        // Parse start letter
        char arg = cmd_buf[8];
        if (arg >= 'A' && arg <= 'Z') {
            start = arg;
        } else if (arg >= 'a' && arg <= 'z') {
            start = arg - 32;  // to uppercase
        } else if (arg == 'F' || arg == 'f') {
            fast_mode = true;
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
    motor_scan_mode = true;
    delay_ms(100);  // Let motor task finish current operation

    uart_puts("Scanning all 2-letter commands from ");
    uart_putc(start);
    uart_puts("A to ZZ\r\n");
    if (fast_mode) {
        uart_puts("FAST mode: only showing responding commands\r\n");
    }
    uart_puts("Press any key to abort.\r\n\r\n");

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
            // Check for abort
            if (uart_getc_nonblocking() >= 0) {
                uart_puts("\r\n*** ABORTED by user ***\r\n");
                aborted = true;
                break;
            }

            tested++;

            // Feed watchdog
            IWDG->KR = 0xAAAA;

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
            if (uart_getc_nonblocking() >= 0) {
                uart_puts("\r\n*** ABORTED by user ***\r\n");
                aborted = true;
                break;
            }

            tested++;
            IWDG->KR = 0xAAAA;

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
    motor_scan_mode = false;

    uart_puts("\r\n=== SCAN COMPLETE ===\r\n");
    uart_puts("Tested: "); print_num(tested); uart_puts(" commands\r\n");
    uart_puts("Found:  "); print_num(found); uart_puts(" responding commands\r\n");
    uart_puts("Timeouts: "); print_num(timeout_count); uart_puts("\r\n");
    DEBUG_PRINT("Motor polling resumed.\r\n");
}
