/**
 * @file commands.c
 * @brief Console command handlers - Core infrastructure and command table
 *
 * Command handlers are split across multiple files:
 *   - commands.c       - Core, system commands, command table
 *   - commands_motor.c - Motor protocol and control commands
 *   - commands_ui.c    - Menu and UI commands
 *   - commands_tapping.c - Tapping and step drill commands
 *   - commands_debug.c - Debug and hardware test commands
 */

#include "commands_internal.h"
#include "buzzer.h"
#include "diagnostics.h"  // Phase 7: System diagnostics

/*===========================================================================*/
/* Command Buffer (used by command handlers)                                 */
/*===========================================================================*/

#define CMD_BUF_SIZE 32
static char cmd_buf[CMD_BUF_SIZE];
static uint8_t cmd_idx = 0;

// Accessors for other command modules
char* get_cmd_buf(void) { return cmd_buf; }
uint8_t get_cmd_idx(void) { return cmd_idx; }
void set_cmd_idx(uint8_t idx) { cmd_idx = idx; }

/*===========================================================================*/
/* Command Matching Helpers                                                  */
/*===========================================================================*/

// Case-insensitive command prefix match
bool cmd_match(const char* prefix) {
    for (int i = 0; prefix[i] != '\0'; i++) {
        if (i >= cmd_idx) return false;
        char c = cmd_buf[i];
        char p = prefix[i];
        // Convert both to uppercase for comparison
        if (c >= 'a' && c <= 'z') c -= 32;
        if (p >= 'a' && p <= 'z') p -= 32;
        if (c != p) return false;
    }
    return true;
}

// Check if command matches exactly (no extra chars except space/args)
bool cmd_is(const char* cmd) {
    int len = 0;
    while (cmd[len]) len++;
    // Must match prefix and either end there or have space for args
    return cmd_match(cmd) && (cmd_idx == len || (cmd_idx > len && cmd_buf[len] == ' '));
}

// Note: cmd_get_arg_int is defined in serial_console.c

/*===========================================================================*/
/* System Command Handlers                                                   */
/*===========================================================================*/

static void enter_dfu_mode(void) {
    uart_puts("Entering DFU mode...\r\n");
    *((volatile uint32_t*)0x20000000) = 0xDEADBEEF;
    for (volatile int i = 0; i < 100000; i++);
    NVIC_SystemReset();
}

void cmd_dfu(void) {
    enter_dfu_mode();
}

void cmd_reset(void) {
    uart_puts("Resetting...\r\n");
    for (volatile int i = 0; i < 100000; i++);
    NVIC_SystemReset();
}

// Force next boot to be treated as COLD BOOT (for testing)
// Phase 2.5: Boot magic constants now in shared.h

void cmd_coldboot(void) {
    uart_puts("Setting COLD BOOT flag and resetting...\r\n");
    uart_puts("Next boot will show full splash + beeps\r\n");
    *FORCE_COLD_BOOT_MAGIC_ADDR = FORCE_COLD_BOOT_MAGIC_VALUE;
    for (volatile int i = 0; i < 100000; i++);
    NVIC_SystemReset();
}

void cmd_save(void) {
    if (settings_is_dirty()) {
        uart_puts("Saving settings...\r\n");
        if (settings_save()) {
            DEBUG_PRINT("Settings saved to ");
            uart_puts(settings_using_eeprom() ? "EEPROM" : "flash");
            uart_puts(".\r\n");
        } else {
            uart_puts("Save failed!\r\n");
        }
    } else {
        uart_puts("No changes to save.\r\n");
    }
}

void cmd_crashshow(void) {
    extern void crash_dump_display(void);
    crash_dump_display();
}

void cmd_crashclear(void) {
    extern void crash_dump_clear(void);
    crash_dump_clear();
}

void cmd_help(void) {
    // Forward reference to command table (defined later in file)
    extern const cmd_entry_t cmd_table[];

    uart_puts("Nova Voyager FreeRTOS Console " FW_VERSION_STRING "\r\n");
    uart_puts("Available commands:\r\n");

    // Auto-generate from command table (always accurate!)
    int count = 0;
    for (int i = 0; cmd_table[i].name != NULL; i++) {
        // Skip debug commands in production builds
        #ifndef BUILD_DEBUG
        if (cmd_table[i].flags & CMD_FLAG_DEBUG) continue;
        #endif

        if (count > 0) uart_puts(" ");
        uart_puts(cmd_table[i].name);
        count++;

        // Line break every 10 commands for readability
        if (count % 10 == 0) {
            uart_puts("\r\n");
        }
    }

    if (count % 10 != 0) uart_puts("\r\n");

    uart_puts("\r\nTotal: ");
    print_num(count);
    uart_puts(" commands available\r\n");
    uart_puts("Type STATUS for system info\r\n");
}

void cmd_status(void) {
    STATE_LOCK();
    uart_puts("State: ");
    switch (g_state.state) {
        case APP_STATE_STARTUP: uart_puts("STARTUP"); break;
        case APP_STATE_IDLE: uart_puts("IDLE"); break;
        case APP_STATE_DRILLING: uart_puts("DRILLING"); break;
        case APP_STATE_TAPPING: uart_puts("TAPPING"); break;
        case APP_STATE_MENU: uart_puts("MENU"); break;
        case APP_STATE_ERROR: uart_puts("ERROR"); break;
        default: uart_puts("UNKNOWN"); break;
    }
    uart_puts("\r\nRPM: ");
    char num[6];
    int rpm = g_state.target_rpm;
    num[0] = '0' + (rpm / 1000) % 10;
    num[1] = '0' + (rpm / 100) % 10;
    num[2] = '0' + (rpm / 10) % 10;
    num[3] = '0' + rpm % 10;
    num[4] = '\0';
    uart_puts(num);
    uart_puts("\r\n");

    // Phase 5.3: Queue depth monitoring
    if (g_event_queue) {
        UBaseType_t evt_msgs = uxQueueMessagesWaiting(g_event_queue);
        UBaseType_t evt_spaces = uxQueueSpacesAvailable(g_event_queue);
        uart_puts("Event Queue: ");
        print_num(evt_msgs);
        uart_puts(" / ");
        print_num(evt_msgs + evt_spaces);
        uart_puts(" (");
        print_num((evt_msgs * 100) / (evt_msgs + evt_spaces));
        uart_puts("% full)\r\n");
    }

    if (g_motor_cmd_queue) {
        UBaseType_t mot_msgs = uxQueueMessagesWaiting(g_motor_cmd_queue);
        UBaseType_t mot_spaces = uxQueueSpacesAvailable(g_motor_cmd_queue);
        uart_puts("Motor Queue: ");
        print_num(mot_msgs);
        uart_puts(" / ");
        print_num(mot_msgs + mot_spaces);
        uart_puts(" (");
        print_num((mot_msgs * 100) / (mot_msgs + mot_spaces));
        uart_puts("% full)\r\n");
    }

    uart_puts("Queue Overflows: ");
    print_num(g_state.motor_queue_overflows);
    uart_puts("\r\n");

    STATE_UNLOCK();
}

// Phase 7: Diagnostic command handlers
void cmd_stats(void) {
    diagnostics_print_report();
}

void cmd_errors(void) {
    diagnostics_print_errors();
}

void cmd_perf(void) {
    diagnostics_print_performance();
}

/*===========================================================================*/
/* External Command Handler Declarations                                     */
/*===========================================================================*/

// Motor commands (commands_motor.c)
extern void cmd_gf(void);
extern void cmd_rs(void);
extern void cmd_mq(void);
extern void cmd_msync(void);
extern void cmd_msave(void);
extern void cmd_mread(void);
extern void cmd_s2(void);
extern void cmd_kr(void);
extern void cmd_cv(void);
extern void cmd_hold(void);
extern void cmd_release(void);
extern void cmd_jog(void);
extern void cmd_svcq(void);
extern void cmd_tc(void);
extern void cmd_ma(void);
extern void cmd_bf(void);
extern void cmd_bn(void);
extern void cmd_gr(void);
extern void cmd_wh(void);
extern void cmd_wl(void);
extern void cmd_uw(void);
extern void cmd_start(void);
extern void cmd_stop(void);
extern void cmd_speed(void);
extern void cmd_power(void);
extern void cmd_mreset(void);
extern void cmd_cvcheck(void);
extern void cmd_eepromtest(void);
extern void cmd_se(void);
extern void cmd_sp(void);
extern void cmd_si(void);
extern void cmd_i3(void);
extern void cmd_i0(void);
extern void cmd_spdrmp(void);
extern void cmd_nc(void);
extern void cmd_ud(void);
extern void cmd_t0(void);
extern void cmd_su(void);
extern void cmd_ts(void);
extern void cmd_uv(void);
extern void cmd_mcbscan(void);  // MCB command discovery scanner

// UI commands (commands_ui.c)
extern void cmd_menu(void);
extern void cmd_up(void);
extern void cmd_dn(void);
extern void cmd_ok(void);
extern void cmd_f1(void);
extern void cmd_f2(void);
extern void cmd_f3(void);
extern void cmd_f4(void);
extern void cmd_beep(void);
extern void cmd_buzz(void);
#ifdef BUILD_DEBUG
extern void cmd_enc(void);
#endif

// Tapping commands (commands_tapping.c)
extern void cmd_tapload(void);
extern void cmd_taprev(void);
extern void cmd_tappeck(void);
extern void cmd_tapact(void);
extern void cmd_tapthr(void);
extern void cmd_tapbrk(void);
extern void cmd_tap(void);
extern void cmd_drill(void);
extern void cmd_drillcfg(void);
#ifdef BUILD_DEBUG
extern void cmd_taptest(void);
extern void cmd_tapstop(void);
extern void cmd_tapsim(void);
#endif

// Debug/test commands (commands_debug.c)
// Hardware test commands
extern void cmd_depth(void);
extern void cmd_guard(void);
extern void cmd_adcmon(void);
extern void cmd_stack(void);
extern void cmd_temp(void);
extern void cmd_tempmcu(void);
extern void cmd_calc(void);
extern void cmd_selftest(void);

// Essential LCD graphics test commands
extern void cmd_draw8icons(void);
extern void cmd_draw8boxes(void);
extern void cmd_test0x37(void);
extern void cmd_testcgrom(void);
extern void cmd_testcgram(void);
extern void cmd_testlcd(void);
extern void cmd_testallicons(void);
extern void cmd_testicons(void);
extern void cmd_scanall(void);
extern void cmd_testsingle(void);
extern void cmd_scangfx(void);
extern void cmd_pauseui(void);
extern void cmd_resumeui(void);
extern void cmd_testgfx(void);

// Load monitoring commands
extern void cmd_loadmon(void);
extern void cmd_loadbase(void);
extern void cmd_loadsense(void);

// Protocol discovery commands
extern void cmd_scan(void);
extern void cmd_listen(void);
extern void cmd_gscan(void);

// Hardware test commands
extern void cmd_adcall(void);
extern void cmd_i2c(void);
#ifdef BUILD_DEBUG
extern void cmd_adcall(void);
extern void cmd_setdbg(void);
extern void cmd_qq(void);
extern void cmd_scan(void);
extern void cmd_listen(void);
extern void cmd_gscan(void);
extern void cmd_test(void);
extern void cmd_testcl(void);
extern void cmd_testload(void);
extern void cmd_tests0(void);
extern void cmd_testht(void);
extern void cmd_testlp(void);
extern void cmd_loadmon(void);
extern void cmd_loadbase(void);
extern void cmd_loadsense(void);
#endif

/*===========================================================================*/
/* Command Table                                                             */
/*===========================================================================*/

const cmd_entry_t cmd_table[] = {
    // System commands
    {"DFU",        cmd_dfu,        0},
    {"RESET",      cmd_reset,      0},
    {"COLDBOOT",   cmd_coldboot,   0},
    {"SAVE",       cmd_save,       0},
    {"CRASHSHOW",  cmd_crashshow,  0},
    {"CRASHCLEAR", cmd_crashclear, 0},
    {"HELP",       cmd_help,       0},
    {"STATUS",     cmd_status,     0},
    {"STATS",      cmd_stats,      0},     // Phase 7: Diagnostic report
    {"ERRORS",     cmd_errors,     0},     // Phase 7: Error summary
    {"PERF",       cmd_perf,       0},     // Phase 7: Performance metrics
#ifdef BUILD_DEBUG
    {"ENC",     cmd_enc,     CMD_FLAG_DEBUG},
#endif

    // Motor commands
    {"START",   cmd_start,   0},
    {"STOP",    cmd_stop,    0},
    {"SPEED",   cmd_speed,   0},
    {"GF",      cmd_gf,      0},
    {"RS",      cmd_rs,      0},
    {"MQ",      cmd_mq,      0},
    {"MSYNC",   cmd_msync,   0},
    {"MSAVE",   cmd_msave,   0},
    {"MREAD",   cmd_mread,   0},
    {"S2",      cmd_s2,      0},
    {"KR",      cmd_kr,      0},
    {"CV",      cmd_cv,      0},
    {"HOLD",    cmd_hold,    0},
    {"RELEASE", cmd_release, 0},

    // Motor power/reset commands (discovered 2026-01-25)
    {"POWER",   cmd_power,   0},
    {"MRESET",  cmd_mreset,  0},
    {"CVCHECK", cmd_cvcheck, 0},

    // Motor protocol test commands (discovered 2026-01-24)
    {"JOG",     cmd_jog,     0},
    {"SVCQ",    cmd_svcq,    0},
    {"TC",      cmd_tc,      0},
    {"MA",      cmd_ma,      0},
    {"BF",      cmd_bf,      0},
    {"BN",      cmd_bn,      0},
    {"GR",      cmd_gr,      0},
    {"WH",      cmd_wh,      0},
    {"WL",      cmd_wl,      0},

    // Service mode parameters (discovered 2026-01-25)
    {"EEPROMTEST", cmd_eepromtest, 0},  // Test EEPROM save sequence
    {"SE",      cmd_se,      0},        // Set Enable (commit param)
    {"SP",      cmd_sp,      0},        // Kprop query (NOT save params!)
    {"SI",      cmd_si,      0},        // Kint query
    {"I3",      cmd_i3,      0},        // IR Offset
    {"I0",      cmd_i0,      0},        // IR Gain
    {"SPDRMP",  cmd_spdrmp,  0},        // Speed Ramp (DN command)
    {"NC",      cmd_nc,      0},        // Speed Advance Max
    {"UD",      cmd_ud,      0},        // DC Bus Voltage
    {"T0",      cmd_t0,      0},        // Heatsink Temp
    {"SU",      cmd_su,      0},        // PulseMax
    {"TS",      cmd_ts,      0},        // Undervoltage Stop
    {"UV",      cmd_uv,      0},        // Undervoltage Run
    {"MCBSCAN", cmd_mcbscan, 0},        // MCB command discovery scanner

    // Menu/UI commands
    {"MENU",    cmd_menu,    0},
    {"UP",      cmd_up,      0},
    {"DN",      cmd_dn,      0},
    {"OK",      cmd_ok,      0},
    {"F1",      cmd_f1,      0},
    {"F2",      cmd_f2,      0},
    {"F3",      cmd_f3,      0},
    {"F4",      cmd_f4,      0},
    {"BEEP",    cmd_beep,    0},
    {"BUZZ",    cmd_buzz,    0},

    // Hardware test commands
    {"DEPTH",   cmd_depth,   0},
    {"GUARD",   cmd_guard,   0},
    {"STACK",   cmd_stack,   0},
    {"TEMP",    cmd_temp,    0},
    {"TEMPMCU", cmd_tempmcu, 0},
    {"CALC",    cmd_calc,    0},

    // LCD graphics test commands
    {"PAUSEUI", cmd_pauseui, 0},           // Suspend UI task for testing
    {"RESUMEUI", cmd_resumeui, 0},         // Resume UI task
    {"TESTGFX", cmd_testgfx, 0},           // Basic graphics test
    {"DRAW8ICONS", cmd_draw8icons, 0},     // Draw 8 bordered icons
    {"DRAW8BOXES", cmd_draw8boxes, 0},     // Draw 8 bordered boxes
    {"TEST37", cmd_test0x37, 0},           // Test 0x37 graphics mode
    {"CGROM", cmd_testcgrom, 0},           // Scan CGROM character set
    {"TESTCGRAM", cmd_testcgram, 0},       // Test CGRAM custom chars
    {"TESTLCD", cmd_testlcd, 0},           // Comprehensive LCD test
    {"TESTALLICONS", cmd_testallicons, 0}, // Test all icon positions
    {"TESTICONS", cmd_testicons, 0},       // Icon drawing tests
    {"SCANALL", cmd_scanall, 0},           // Scan all commands 0x00-0xFF
    {"TESTSINGLE", cmd_testsingle, 0},     // Test individual commands
    {"SCANGFX", cmd_scangfx, 0},           // Scan commands in graphics mode

    // Load monitoring commands (for tuning load triggers)
    {"LOADMON", cmd_loadmon, 0},           // Monitor motor load continuously
    {"LOADBASE", cmd_loadbase, 0},         // Learn baseline load
    {"LOADSENSE", cmd_loadsense, 0},       // Test load sensing detection

    // Protocol discovery commands
    {"SCAN", cmd_scan, 0},                 // Scan motor query commands
    {"LISTEN", cmd_listen, 0},             // Listen to motor UART
    {"GSCAN", cmd_gscan, 0},               // Grouped command scan

    // Hardware test commands
    {"ADCALL", cmd_adcall, 0},             // Read all ADC channels
    {"I2C", cmd_i2c, 0},                   // I2C bus scan

    // Step drill commands
    {"DRILLCFG", cmd_drillcfg, 0},
    {"DRILL",    cmd_drill,    0},

    // Tapping commands (order matters - longer prefixes first)
    {"TAPLOAD", cmd_tapload, 0},
    {"TAPREV",  cmd_taprev,  0},
    {"TAPPECK", cmd_tappeck, 0},
    {"TAPACT",  cmd_tapact,  0},
    {"TAPTHR",  cmd_tapthr,  0},
    {"TAPBRK",  cmd_tapbrk,  0},
#ifdef BUILD_DEBUG
    {"TAPTEST", cmd_taptest, CMD_FLAG_DEBUG},
    {"TAPSTOP", cmd_tapstop, CMD_FLAG_DEBUG},
    {"TAPSIM",  cmd_tapsim,  CMD_FLAG_DEBUG},
#endif
    {"TAP",     cmd_tap,     0},

    // Debug-only commands (only include commands that actually exist)
#ifdef BUILD_DEBUG
    {"SCAN",    cmd_scan,    CMD_FLAG_DEBUG},
    {"LISTEN",  cmd_listen,  CMD_FLAG_DEBUG},
    {"GSCAN",   cmd_gscan,   CMD_FLAG_DEBUG},
    {"LOADMON", cmd_loadmon, CMD_FLAG_DEBUG},
    {"LOADBASE", cmd_loadbase, CMD_FLAG_DEBUG},
    {"LOADSENSE", cmd_loadsense, CMD_FLAG_DEBUG},
#endif

    {NULL, NULL, 0}  // Sentinel
};

#define CMD_TABLE_SIZE (sizeof(cmd_table) / sizeof(cmd_table[0]) - 1)

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

void commands_init(void) {
    // Nothing to initialize yet
}

static void process_serial_char(int c);

void commands_check_serial(void) {
    // Read ALL available characters to prevent dropping at 9600 baud
    int c;
    while ((c = uart_getc_nonblocking()) >= 0) {
        process_serial_char(c);
    }
}

void commands_process_char(int c) {
    process_serial_char(c);
}

const cmd_entry_t* commands_get_table(void) {
    return cmd_table;
}

uint32_t commands_get_table_size(void) {
    return CMD_TABLE_SIZE;
}

/*===========================================================================*/
/* Internal Command Processing                                               */
/*===========================================================================*/

static void process_serial_char(int c) {
    // Echo character
    uart_putc((char)c);

    // Handle backspace
    if (c == 0x7F || c == 0x08) {
        if (cmd_idx > 0) cmd_idx--;
        return;
    }

    // Handle newline - process command using table lookup
    if (c == '\r' || c == '\n') {
        uart_puts("\r\n");
        cmd_buf[cmd_idx] = '\0';

        if (cmd_idx == 0) {
            // Empty command - show help
            uart_puts("Commands: DFU, RESET, HELP, STATUS\r\n");
        } else {
            // Search command table for matching command
            bool found = false;
            for (int i = 0; cmd_table[i].name != NULL; i++) {
                if (cmd_is(cmd_table[i].name)) {
                    cmd_table[i].handler();
                    found = true;
                    break;
                }
            }
            if (!found) {
                uart_puts("Unknown: ");
                uart_puts(cmd_buf);
                uart_puts("\r\nType HELP for commands\r\n");
            }
        }

        cmd_idx = 0;
        uart_puts("> ");
        return;
    }

    // Add character to buffer
    if (cmd_idx < sizeof(cmd_buf) - 1) {
        cmd_buf[cmd_idx++] = (char)c;
    }
}
