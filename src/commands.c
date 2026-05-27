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
#include "diagnostics.h"
#include "eeprom.h"
#include "hex_format.h"
#include "dfu.h"
#include "safety.h"
#include "lcd.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

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

// Hand-off contract with nova-voyager_bootloader: it re-enters DFU when it
// finds this value at this address after a reset (bootloader src/main.c:66-67,
// address reserved by its linker as _dfu_magic and ASSERT-guarded against its
// own .bss, so the value survives the bootloader zeroing BSS).
//
// This used to write 0xDEADBEEF to 0x20000000 and matched neither the address
// nor the value, so DFU never actually entered DFU mode — it just reset back
// into the application. Verified on hardware 2026-08-29. 0x20000000 is also
// the base of SRAM, i.e. it was scribbling on live .data on the way out.

static void enter_dfu_mode(void) {
    uart_puts("Entering DFU mode...\r\n");

    /* DFU only exists on the 72 MHz bootloader. The 120 MHz build cannot derive
     * the 48 MHz USB needs, and as of nova-voyager_bootloader v0.1.0 it refuses
     * DFU outright rather than enumerating a broken device — so the magic word
     * below is written, the board resets, and the application simply starts
     * again with nothing to show for it.
     *
     * We cannot detect which bootloader is installed; the firmware only knows
     * its own clock. USE_120MHZ is a good proxy because the product pairing is
     * 120 MHz firmware with the 120 MHz bootloader, but it is a heuristic, so
     * the wording says "if" rather than asserting. Without this the command is
     * a silent no-op on the shipping configuration. */
#if USE_120MHZ
    uart_puts("NOTE: DFU needs the 72 MHz bootloader (nova_bootloader).\r\n");
    uart_puts("      If this board has nova_bootloader_120, it will just\r\n");
    uart_puts("      reboot — no USB device will appear. Flash the 72 MHz\r\n");
    uart_puts("      pair first, or use SWD.\r\n");
#endif

    dfu_reboot_into_bootloader();
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

void cmd_dump(void) {
    const settings_t* s = settings_get();
    uart_puts("=== SETTINGS DUMP ===\r\n");

    // Speed
    uart_puts("speed.default="); print_num(s->speed.default_rpm); uart_puts("\r\n");
    uart_puts("speed.max="); print_num(s->speed.max_limit); uart_puts("\r\n");

    // Motor
    uart_puts("motor.profile="); print_num(s->motor.profile); uart_puts("\r\n");
    uart_puts("motor.speed_ramp="); print_num(s->motor.speed_ramp); uart_puts("\r\n");
    uart_puts("motor.torque_ramp="); print_num(s->motor.torque_ramp); uart_puts("\r\n");
    uart_puts("motor.current_limit="); print_num(s->motor.current_limit); uart_puts("\r\n");
    uart_puts("motor.speed_kprop="); print_num(s->motor.speed_kprop); uart_puts("\r\n");
    uart_puts("motor.speed_kint="); print_num(s->motor.speed_kint); uart_puts("\r\n");
    uart_puts("motor.voltage_kp="); print_num(s->motor.voltage_kp); uart_puts("\r\n");
    uart_puts("motor.voltage_ki="); print_num(s->motor.voltage_ki); uart_puts("\r\n");
    uart_puts("motor.ir_gain="); print_num(s->motor.ir_gain); uart_puts("\r\n");
    uart_puts("motor.ir_offset="); print_num(s->motor.ir_offset); uart_puts("\r\n");

    // Power
    uart_puts("power.output="); print_num(s->power.power_output); uart_puts("\r\n");
    uart_puts("power.temp_threshold="); print_num(s->power.temp_threshold); uart_puts("\r\n");
    uart_puts("power.dc_bus="); print_num(s->power.dc_bus_voltage); uart_puts("\r\n");

    // Sensor
    uart_puts("sensor.jam_detect="); print_num(s->sensor.jam_detect); uart_puts("\r\n");
    uart_puts("sensor.spike_detect="); print_num(s->sensor.spike_detect); uart_puts("\r\n");
    uart_puts("sensor.spike_thresh="); print_num(s->sensor.spike_thresh); uart_puts("\r\n");
    uart_puts("sensor.step_thresh="); print_num(s->sensor.step_thresh); uart_puts("\r\n");
    uart_puts("sensor.low_load_detect="); print_num(s->sensor.low_load_detect); uart_puts("\r\n");
    uart_puts("sensor.low_load_thresh="); print_num(s->sensor.low_load_thresh); uart_puts("\r\n");
    uart_puts("sensor.vibration="); print_num(s->sensor.vibration_sensitivity); uart_puts("\r\n");
    uart_puts("sensor.guard_check="); print_num(s->sensor.guard_check_enabled); uart_puts("\r\n");
    uart_puts("sensor.overload="); print_num(s->sensor.overload_threshold); uart_puts("\r\n");

    // Tapping triggers
    uart_puts("tap.depth="); print_num(s->tapping.depth_trigger_enabled); uart_puts("\r\n");
    uart_puts("tap.load_inc="); print_num(s->tapping.load_increase_enabled); uart_puts("\r\n");
    uart_puts("tap.load_slip="); print_num(s->tapping.load_slip_enabled); uart_puts("\r\n");
    uart_puts("tap.clutch="); print_num(s->tapping.clutch_slip_enabled); uart_puts("\r\n");
    uart_puts("tap.quill="); print_num(s->tapping.quill_trigger_enabled); uart_puts("\r\n");
    uart_puts("tap.peck="); print_num(s->tapping.peck_trigger_enabled); uart_puts("\r\n");
    uart_puts("tap.pedal="); print_num(s->tapping.pedal_enabled); uart_puts("\r\n");

    // Favorites
    for (int i = 0; i < NUM_FAVORITE_SPEEDS; i++) {
        if (s->speed.favorite[i] > 0) {
            uart_puts("speed.fav"); uart_putc('0' + i); uart_puts("=");
            print_num(s->speed.favorite[i]); uart_puts("\r\n");
        }
    }

    uart_puts("=== END ===\r\n");
}

void cmd_save(void) {
    if (settings_is_dirty()) {
        uart_puts("Saving settings...\r\n");
        /* REVIEW FIX: "Save failed!" covered three different outcomes that
         * need different actions from the operator. */
        switch (settings_save()) {
            case SETTINGS_SAVE_OK:
                /* REVIEW FIX: this was DEBUG_PRINT, which expands to ((void)0)
                 * whenever NDEBUG is set — i.e. in every release build — so
                 * the operator saw a bare "EEPROM." with no leading text while
                 * all three failure branches printed properly. */
                uart_puts("Settings saved to ");
                uart_puts(settings_using_eeprom() ? "EEPROM" : "flash");
                uart_puts(".\r\n");
                break;
            case SETTINGS_SAVE_DEFERRED:
                uart_puts("Saved to EEPROM. Flash mirror deferred - stop the "
                          "motor and SAVE again to store the rest.\r\n");
                break;
            case SETTINGS_SAVE_BLOCKED:
                uart_puts("NOT saved: this unit stores settings in flash, "
                          "which needs the motor stopped.\r\n");
                break;
            case SETTINGS_SAVE_ERROR:
            default:
                uart_puts("Save failed: storage did not accept the data.\r\n");
                break;
        }
    } else {
        uart_puts("No changes to save.\r\n");
    }
}

// GET <key> — print the current value of one setting. Same keys as SET.
// Use DUMP for a full listing.
/* One key->value reader, shared by GET and by SET's confirmation line. SET used
 * to echo the value the operator typed rather than the value that was stored,
 * which is wrong wherever a setter clamps. */
static bool settings_read_by_key(const char* key, uint16_t* out) {
    const settings_t* s = settings_get();
    if (!s || !out) return false;

    if      (strcmp(key, "speed.default") == 0)          *out = s->speed.default_rpm;
    else if (strcmp(key, "speed.max") == 0)              *out = s->speed.max_limit;
    else if (strcmp(key, "sensor.jam_detect") == 0)      *out = s->sensor.jam_detect;
    else if (strcmp(key, "sensor.spike_detect") == 0)    *out = s->sensor.spike_detect;
    else if (strcmp(key, "sensor.spike_thresh") == 0)    *out = s->sensor.spike_thresh;
    else if (strcmp(key, "sensor.step_thresh") == 0)     *out = s->sensor.step_thresh;
    else if (strcmp(key, "sensor.low_load_detect") == 0) *out = s->sensor.low_load_detect;
    else if (strcmp(key, "sensor.low_load_thresh") == 0) *out = s->sensor.low_load_thresh;
    else if (strcmp(key, "sensor.overload") == 0)        *out = s->sensor.overload_threshold;
    else return false;

    return true;
}

void cmd_get(void) {
    char* buf = get_cmd_buf();
    uint8_t idx = get_cmd_idx();

    if (idx < 5 || buf[3] != ' ') {
        uart_puts("Usage: GET <key>  (same keys as SET, or use DUMP)\r\n");
        return;
    }

    int i = 4;
    int key_start = i;
    while (i < idx && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n') i++;
    buf[i] = '\0';
    const char* key = &buf[key_start];

    uint16_t v;
    if (!settings_read_by_key(key, &v)) {
        uart_puts("Unknown key: ");
        uart_puts(key);
        uart_puts("\r\n");
        return;
    }

    uart_puts(key);
    uart_puts("=");
    print_num(v);
    uart_puts("\r\n");
}

// SET <key> <value> — apply a value to a known setting in RAM. Use SAVE
// afterward to persist to EEPROM. Keys mirror DUMP output. Boolean fields
// accept 0/1. Range-checking lives in the corresponding settings_set_*.
/* Keys whose setter takes uint8_t need their own bound — the shared parse
 * guard can only reject above 65535. */
static bool reject_u8(const char* key) {
    uart_puts("Value too large for ");
    uart_puts(key);
    uart_puts(" (max 255)\r\n");
    return false;
}

void cmd_set(void) {
    char* buf = get_cmd_buf();
    uint8_t idx = get_cmd_idx();

    if (idx < 6 || buf[3] != ' ') {
        uart_puts("Usage: SET <key> <value>\r\n");
        uart_puts("Keys: speed.default speed.max sensor.jam_detect\r\n");
        uart_puts("      sensor.spike_detect sensor.spike_thresh\r\n");
        uart_puts("      sensor.step_thresh sensor.low_load_detect\r\n");
        uart_puts("      sensor.low_load_thresh sensor.overload\r\n");
        uart_puts("Run DUMP to see current values.\r\n");
        return;
    }

    // Walk past "SET ", find end of key
    int i = 4;
    int key_start = i;
    while (i < idx && buf[i] != ' ') i++;
    if (i >= idx) {
        uart_puts("Missing value\r\n");
        return;
    }
    buf[i++] = '\0';  // null-terminate key in-place

    /* Parse unsigned decimal value.
     *
     * AUDIT FIX (MEDIUM): reject overflow BEFORE the setter narrows. The
     * comment here used to claim this covered "SET sensor.step_thresh 300"
     * storing 44 — REVIEW FIX (HIGH): it never did. The only bound was 65535,
     * which is right for the uint16_t setters and useless for the three that
     * take uint8_t: 300 stored 44 and 256 stored 0, the latter DISABLING the
     * step-delta jam detector, while the console printed back the value the
     * operator typed. Each narrowing key now states its own bound. */
    uint32_t value = 0;
    bool got_digit = false;
    bool overflow = false;
    while (i < idx && buf[i] >= '0' && buf[i] <= '9') {
        value = value * 10 + (buf[i] - '0');
        i++;
        got_digit = true;
        if (value > 65535) { overflow = true; break; }
    }
    if (!got_digit) {
        uart_puts("Value must be a non-negative integer\r\n");
        return;
    }
    if (overflow) {
        uart_puts("Value too large (max 65535)\r\n");
        return;
    }

    const char* key = &buf[key_start];
    bool ok = true;

    if (strcmp(key, "speed.default") == 0) {
        settings_set_speed((uint16_t)value);
    } else if (strcmp(key, "speed.max") == 0) {
        settings_set_max_speed((uint16_t)value);
    } else if (strcmp(key, "sensor.jam_detect") == 0) {
        settings_set_jam_detect(value != 0);
    } else if (strcmp(key, "sensor.spike_detect") == 0) {
        settings_set_spike_detect(value != 0);
    } else if (strcmp(key, "sensor.spike_thresh") == 0) {
        settings_set_spike_thresh((uint16_t)value);
    } else if (strcmp(key, "sensor.step_thresh") == 0) {
        if (value > 255) { ok = reject_u8(key); } else settings_set_step_thresh((uint8_t)value);
    } else if (strcmp(key, "sensor.low_load_detect") == 0) {
        settings_set_low_load_detect(value != 0);
    } else if (strcmp(key, "sensor.low_load_thresh") == 0) {
        if (value > 255) { ok = reject_u8(key); } else settings_set_low_load_thresh((uint8_t)value);
    } else if (strcmp(key, "sensor.overload") == 0) {
        if (value > 255) { ok = reject_u8(key); } else settings_set_overload_threshold((uint8_t)value);
    } else {
        uart_puts("Unknown key: ");
        uart_puts(key);
        uart_puts("\r\n(try SET for usage, DUMP for keys)\r\n");
        ok = false;
    }

    if (ok) {
        /* REVIEW FIX (MEDIUM): this echoed the PARSED value, not the stored
         * one, while several setters clamp silently — settings_set_speed() pins
         * to speed.max_limit, step_thresh floors at JAM_STEP_MIN_THRESH,
         * spike_thresh at 20. So `SET speed.default 3000` on a machine capped
         * at 800 answered "=3000", and `SET sensor.step_thresh 0` — the
         * documented way to disable the step detector — answered "=0" while 5
         * was stored and the detector stayed armed. Read it back. */
        uint16_t stored16 = (uint16_t)value;
        (void)settings_read_by_key(key, &stored16);
        const uint32_t stored = stored16;
        uart_puts("Set ");
        uart_puts(key);
        uart_puts("=");
        print_num((uint16_t)stored);
        if (stored != value) {
            uart_puts(" (clamped from ");
            print_num((uint16_t)value);
            uart_putc(')');
        }
        uart_puts(" (use SAVE to persist)\r\n");
    }
}

/* ALIGN has one more refusal condition than the shared safety gate: it is
 * meaningless and destructive on a turning spindle (see align_machine_busy()
 * in motor.c), and safety_can_start_motor() cannot test motor_running without
 * refusing ordinary starts. */
static const char* align_refusal_reason(void) {
    if (g_state.motor_running ||
        g_state.state == APP_STATE_DRILLING ||
        g_state.state == APP_STATE_TAPPING) {
        return "machine is running - STOP first";
    }
    return safety_refusal_reason();
}

void cmd_align(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    if (cmd_idx >= 7 && cmd_buf[5] == ' ') {
        char arg = cmd_buf[6];
        if (arg >= 'A' && arg <= 'C') {
            uint8_t phase = arg - 'A';
            if (!motor_set_align_phase(phase)) {
                uart_puts("ALIGN refused: ");
                uart_puts(align_refusal_reason());
                uart_puts("\r\n");
                return;
            }
            uart_puts("Phase ");
            uart_putc(arg);
            uart_puts(": ");
            delay_ms(100);
            int8_t gr = motor_read_align_sensors();
            /* REVIEW FIX (MEDIUM): motor_read_param() returns -1 on timeout
             * or parse failure, and (int8_t)-1 is 0xFF — so all three bit
             * tests passed and this printed "RPS:ABC", i.e. all three rotor
             * sensors healthy, exactly when the link was dead. That inverts
             * the diagnostic during a procedure that energizes the windings. */
            uart_puts("GR=");
            print_num(gr);
            if (gr < 0) {
                uart_puts(" RPS:--- (no reply)\r\n");
            } else {
                uart_puts(" RPS:");
                uart_putc((gr & 1) ? 'A' : '.');
                uart_putc((gr & 2) ? 'B' : '.');
                uart_putc((gr & 4) ? 'C' : '.');
                uart_puts("\r\n");
            }
            return;
        }
    }

    // AUDIT FIX (HIGH, commands.c:311): "ALIGN OFF" was unparseable — the
    // check was off by one (cmd_buf[6]=='F' but 'O' sits at index 6 with
    // ALIGN + space + OFF). Motor windings were left energized at CL=20%
    // indefinitely because the documented exit path was unreachable.
    // Also accept lowercase 'off' since command matching is case-insensitive
    // for the verb.
    if (cmd_idx >= 9 &&
        (cmd_buf[6] == 'O' || cmd_buf[6] == 'o') &&
        (cmd_buf[7] == 'F' || cmd_buf[7] == 'f') &&
        (cmd_buf[8] == 'F' || cmd_buf[8] == 'f')) {
        /* REVIEW FIX (HIGH): this ran unconditionally, with no gate and no
         * check that an align session was ever entered. motor_exit_align()
         * sends VR=0, CL=0, VS=0, CL=100 and drops PD4 — so typing ALIGN OFF
         * with no align active (a stale line in a pasted script, say) while the
         * guard is open tore down the SAFETY SPINDLE HOLD: PD4 low, the
         * coasting spindle released, CL left at 100%, and spindle_hold_active
         * still true so motor_is_spindle_hold_active() kept reporting a hold
         * that no longer existed.
         *
         * An align session IS the scan claim taken by align_gate_ok(). If we do
         * not hold it, there is nothing to exit. */
        if (!motor_scan_held_by_caller()) {
            uart_puts("ALIGN OFF: not in alignment - nothing to exit\r\n");
            return;
        }
        uart_puts("Exiting alignment\r\n");
        motor_exit_align();
        return;
    }

    if (cmd_idx == 5) {
        // AUDIT FIX (CRITICAL, commands.c:730): ALIGN energizes the windings
        // via PD4 and so used to defeat the E-Stop and guard interlocks. The
        // gate now lives in motor_enter_align()/motor_set_align_phase(); this
        // just reports the refusal.
        if (!motor_enter_align(0)) {
            uart_puts("ALIGN refused: ");
            uart_puts(align_refusal_reason());
            uart_puts("\r\n");
            return;
        }
        uart_puts("Entering alignment (Phase A)\r\n");
        delay_ms(100);
        int8_t gr = motor_read_align_sensors();
        /* REVIEW FIX (MEDIUM): same bug as the ALIGN A|B|C branch above —
         * motor_read_param() returns -1 on timeout, (int8_t)-1 is 0xFF, so all
         * three bit tests passed and a dead MCB link printed "RPS:ABC", i.e.
         * all three rotor sensors healthy, right after energizing the
         * windings. The guard was added there and missed here. */
        uart_puts("GR=");
        print_num(gr);
        if (gr < 0) {
            uart_puts(" RPS:--- (no reply)\r\n");
            return;
        }
        uart_puts(" RPS:");
        uart_putc((gr & 1) ? 'A' : '.');
        uart_putc((gr & 2) ? 'B' : '.');
        uart_putc((gr & 4) ? 'C' : '.');
        uart_puts("\r\n");
        uart_puts("Use: ALIGN A/B/C to switch, ALIGN OFF to exit\r\n");
        return;
    }

    uart_puts("Usage: ALIGN [A|B|C|OFF]\r\n");
}

// Hex-dump all 256 EEPROM bytes to the console (assumes EEPROM already init'd).
// Shared by the EEDUMP command and the read-only build's boot backup.
void eeprom_hexdump_console(void) {
    for (uint16_t row = 0; row < 256; row += 16) {
        // Address
        uart_putc(hex_digit((row >> 4) & 0xF));
        uart_putc(hex_digit(row & 0xF));
        uart_puts(": ");
        // Hex bytes
        uint8_t line[16];
        for (int i = 0; i < 16; i++) {
            uint8_t b = 0xFF;
            eeprom_read_byte(row + i, &b);
            line[i] = b;
            uart_putc(hex_digit((b >> 4) & 0xF));
            uart_putc(hex_digit(b & 0xF));
            uart_putc(' ');
        }
        uart_puts(" |");
        // ASCII
        for (int i = 0; i < 16; i++) {
            uart_putc((line[i] >= 0x20 && line[i] < 0x7F) ? line[i] : '.');
        }
        uart_puts("|\r\n");
    }
}

void cmd_eedump(void) {
    extern bool eeprom_init(void);
    if (!eeprom_init()) {
        uart_puts("No EEPROM detected\r\n");
        return;
    }
    uart_puts("=== EEPROM DUMP (256 bytes) ===\r\n");
    eeprom_hexdump_console();
}

#ifdef BUILD_GAMES
typedef void (*game_fn_t)(void);
static void game_task_wrapper(void *param);

void game_launch(void (*game)(void)) {
    // AUDIT FIX (HIGH, commands.c:379): the old order was suspend-then-create.
    // When invoked from the System menu (menu.c:836-842) the caller IS the UI
    // task, so vTaskSuspend(g_task_ui) self-suspended immediately and the
    // xTaskCreate below never ran — heartbeat_ui went stale, main stopped
    // feeding the IWDG, and the machine watchdog-reset ~5 s after picking a
    // game/showcase menu item on GAMES/DEMO builds. Console-path had a
    // milder version: no game, UI heartbeat continued because UI wasn't the
    // caller. Fix: create the game task FIRST (priority 1 < UI's 2, so it
    // won't preempt until UI yields), then self-suspend. game_task_wrapper
    // resumes the UI at exit.
    g_state.game_mode_active = true;
    TaskHandle_t h = NULL;
    if (xTaskCreate(game_task_wrapper, "Game", 512, (void*)game, 1, &h) != pdPASS) {
        g_state.game_mode_active = false;
        return;
    }
    vTaskSuspend(g_task_ui);
}

static void game_task_wrapper(void *param) {
    game_fn_t game = (game_fn_t)param;
    game();
    vTaskResume(g_task_ui);
    g_state.game_mode_active = false;
    lcd_clear();
    uart_puts("Game exited\r\n");
    vTaskDelete(NULL);
}

void cmd_game(void) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    game_fn_t game = NULL;
    if (cmd_idx >= 6 && cmd_buf[4] == ' ') {
        char g = cmd_buf[5];
        extern void pong_run(void);
        extern void snake_run(void);
        extern void penguin_run(void);
        extern void beerquill_run(void);
        if (g == 'P' || g == 'p' || g == '1')      game = pong_run;
        else if (g == 'S' || g == 's' || g == '2')  game = snake_run;
        else if (g == 'N' || g == 'n' || g == '3')  game = penguin_run;
        else if (g == 'B' || g == 'b' || g == '4')  game = beerquill_run;
#ifdef BUILD_READONLY
        else if (g == 'D' || g == 'd' || g == '5')  { extern void showcase_run(void); game = showcase_run; }
#endif
    }

    if (!game) {
#ifdef BUILD_READONLY
        uart_puts("Usage: GAME P|S|N|B (games) | D (Showcase)\r\n");
#else
        uart_puts("Usage: GAME P (Pong) | S (Snake) | N (Penguin) | B (BeerQuill)\r\n");
#endif
        return;
    }

    game_launch(game);
}
#endif // BUILD_GAMES

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
    uart_puts("\r\nRPM target: ");
    print_num(g_state.target_rpm);
    uart_puts("  actual: ");
    print_num(g_state.current_rpm);
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
#if defined(BUILD_READONLY) || defined(BUILD_DEBUG)
extern void cmd_diag(void);     // read-only live MCB health summary
extern void cmd_regscan(void);  // curated MCB register scan
extern void cmd_listen(void);   // raw MCB UART sniffer (SNIFF)
#endif

// UI commands (commands_ui.c)
extern void cmd_menu(void);
extern void cmd_lcd(void);
extern void cmd_tapset(void);
extern void cmd_arm(void);
extern void cmd_eeerase(void);
extern void cmd_eewrite(void);
extern void cmd_vib(void);
extern void cmd_vibraw(void);
extern void cmd_i2cscan(void);
extern void cmd_vibpwr(void);
extern void cmd_i2calt(void);
extern void cmd_simload(void);
extern void cmd_simcv(void);
extern void cmd_tapcfg(void);
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

// Load monitoring
extern void cmd_loadmon(void);
#ifdef BUILD_DEBUG
extern void cmd_crashtest(void);
extern void cmd_faultreport(void);
extern void cmd_crashundef(void);
#endif
extern void cmd_loadinfo(void);
extern void cmd_jaminfo(void);

// Hardware diagnostics
extern void cmd_i2c(void);

#ifdef BUILD_DEBUG
// LCD test commands (use lcd_research project for comprehensive testing)
extern void cmd_draw8icons(void);
extern void cmd_testcgrom(void);
extern void cmd_testlcd(void);
extern void cmd_testallicons(void);
extern void cmd_testgfx(void);
// Protocol discovery
extern void cmd_scan(void);
extern void cmd_listen(void);
extern void cmd_gscan(void);
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
    {"DUMP",       cmd_dump,       0},
    {"GET",        cmd_get,        0},
    {"SET",        cmd_set,        0},
    {"VIB",        cmd_vib,        0},
    {"VIBRAW",     cmd_vibraw,     CMD_FLAG_DEBUG},
    {"I2CSCAN",    cmd_i2cscan,    0},
    {"VIBPWR",     cmd_vibpwr,     CMD_FLAG_DEBUG},
    {"I2CALT",     cmd_i2calt,     CMD_FLAG_DEBUG},
    {"EEDUMP",     cmd_eedump,     0},
    /* Release-available on purpose: this is how an operator gets back to the
     * original firmware. Guarded by an explicit CONFIRM word, not by a flag. */
    {"EEERASE",    cmd_eeerase,    0},
    {"EEWRITE",    cmd_eewrite,    CMD_FLAG_DEBUG},
    {"ALIGN",      cmd_align,      0},
#ifdef BUILD_GAMES
    {"GAME",       cmd_game,       0},
#endif
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
    /* REVIEW FIX (HIGH): was flags 0, i.e. shipped in release builds.
     * motor_test_mq() assembles a full [SOH]..[STX][unit][cmd][ETX][XOR]
     * COMMAND frame from two arbitrary console characters — "MQ ST" is a
     * start, "MQ RS" a stop, "MQ EE" an EEPROM execute — straight down USART3
     * with no safety gate and no scan claim. Same bypass class as JOG, which
     * is debug-only; this was not. */
    {"MQ",      cmd_mq,      CMD_FLAG_DEBUG},
    {"MSYNC",   cmd_msync,   0},
    {"MREAD",   cmd_mread,   0},
    {"S2",      cmd_s2,      0},
    {"KR",      cmd_kr,      0},
    {"CV",      cmd_cv,      0},
    {"HOLD",    cmd_hold,    0},
    {"RELEASE", cmd_release, 0},
    {"POWER",   cmd_power,   0},
    {"CVCHECK", cmd_cvcheck, 0},
    {"UD",      cmd_ud,      0},
    {"T0",      cmd_t0,      0},

#if defined(BUILD_READONLY) || defined(BUILD_DEBUG)
    // Read-only MCB diagnostics (demo + debug builds)
    {"DIAG",    cmd_diag,    0},
    {"REGSCAN", cmd_regscan, 0},
    {"SNIFF",   cmd_listen,  0},
#endif

#ifdef BUILD_DEBUG
    // Dangerous MCB operations
    {"MSAVE",   cmd_msave,   CMD_FLAG_DEBUG},
    {"MRESET",  cmd_mreset,  CMD_FLAG_DEBUG},
    {"SE",      cmd_se,      CMD_FLAG_DEBUG},
    {"EEPROMTEST", cmd_eepromtest, CMD_FLAG_DEBUG},
    {"MCBSCAN", cmd_mcbscan, CMD_FLAG_DEBUG},

    // MCB register queries (service mode)
    {"JOG",     cmd_jog,     CMD_FLAG_DEBUG},
    {"SVCQ",    cmd_svcq,    CMD_FLAG_DEBUG},
    {"TC",      cmd_tc,      CMD_FLAG_DEBUG},
    {"MA",      cmd_ma,      CMD_FLAG_DEBUG},
    {"BF",      cmd_bf,      CMD_FLAG_DEBUG},
    {"BN",      cmd_bn,      CMD_FLAG_DEBUG},
    {"GR",      cmd_gr,      CMD_FLAG_DEBUG},
    {"WH",      cmd_wh,      CMD_FLAG_DEBUG},
    {"WL",      cmd_wl,      CMD_FLAG_DEBUG},
    {"SP",      cmd_sp,      CMD_FLAG_DEBUG},
    {"SI",      cmd_si,      CMD_FLAG_DEBUG},
    {"I3",      cmd_i3,      CMD_FLAG_DEBUG},
    {"I0",      cmd_i0,      CMD_FLAG_DEBUG},
    {"SPDRMP",  cmd_spdrmp,  CMD_FLAG_DEBUG},
    {"NC",      cmd_nc,      CMD_FLAG_DEBUG},
    {"SU",      cmd_su,      CMD_FLAG_DEBUG},
    {"TS",      cmd_ts,      CMD_FLAG_DEBUG},
    {"UV",      cmd_uv,      CMD_FLAG_DEBUG},
#endif

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

    // Hardware test commands
    {"DEPTH",   cmd_depth,   0},
    {"GUARD",   cmd_guard,   0},
    {"STACK",   cmd_stack,   0},
    {"TEMP",    cmd_temp,    0},
    {"TEMPMCU", cmd_tempmcu, 0},
    {"CALC",    cmd_calc,    0},

    // Hardware diagnostics
    {"I2C",     cmd_i2c,     0},
    {"ADCMON",  cmd_adcmon,  0},
    {"SELFTEST", cmd_selftest, 0},

    // Load monitoring
    {"LOADMON",  cmd_loadmon,  0},
#ifdef BUILD_DEBUG
    {"CRASHTEST", cmd_crashtest, CMD_FLAG_DEBUG},
    {"FAULTREPORT", cmd_faultreport, CMD_FLAG_DEBUG},
    {"CRASHUNDEF", cmd_crashundef, CMD_FLAG_DEBUG},
#endif
    {"LOADINFO", cmd_loadinfo, 0},
    {"JAMINFO",  cmd_jaminfo,  0},

    // Step drill
    {"DRILLCFG", cmd_drillcfg, 0},
    {"DRILL",    cmd_drill,    0},

    // Tapping (order matters - longer prefixes first)
    {"TAPLOAD", cmd_tapload, 0},
    {"TAPREV",  cmd_taprev,  0},
    {"TAPPECK", cmd_tappeck, 0},
    {"TAPACT",  cmd_tapact,  0},
    {"TAPTHR",  cmd_tapthr,  0},
    {"TAPBRK",  cmd_tapbrk,  0},
    {"TAP",     cmd_tap,     0},

    /* LCD is deliberately NOT debug-only. It is read-only, costs a few hundred
     * bytes, and answers "what is the panel actually showing" — which is the
     * first question worth asking about a machine misbehaving in a workshop,
     * and unanswerable over the wire without it. Being blind to the panel made
     * the release-build verification on 2026-08-31 materially harder. A field
     * diagnostic belongs in the build that goes to the field. */
    {"LCD",     cmd_lcd,     0},

#ifdef BUILD_DEBUG
    // Tapping test
    {"ARM",     cmd_arm,     CMD_FLAG_DEBUG},
    {"SIMLOAD", cmd_simload, CMD_FLAG_DEBUG},
    {"SIMCV",   cmd_simcv,   CMD_FLAG_DEBUG},
    {"TAPSET",  cmd_tapset,  CMD_FLAG_DEBUG},
    {"TAPCFG",  cmd_tapcfg,  CMD_FLAG_DEBUG},
    {"TAPTEST", cmd_taptest, CMD_FLAG_DEBUG},
    {"TAPSTOP", cmd_tapstop, CMD_FLAG_DEBUG},
    {"TAPSIM",  cmd_tapsim,  CMD_FLAG_DEBUG},

    // LCD test (use lcd_research project for comprehensive testing)
    {"TESTGFX",  cmd_testgfx,  CMD_FLAG_DEBUG},
    {"DRAW8ICONS", cmd_draw8icons, CMD_FLAG_DEBUG},
    {"CGROM",    cmd_testcgrom,  CMD_FLAG_DEBUG},
    {"TESTLCD",  cmd_testlcd,   CMD_FLAG_DEBUG},
    {"TESTALLICONS", cmd_testallicons, CMD_FLAG_DEBUG},

    // Raw hardware
    {"BUZZ",    cmd_buzz,    CMD_FLAG_DEBUG},

    // Protocol discovery
    {"SCAN",    cmd_scan,    CMD_FLAG_DEBUG},
    {"LISTEN",  cmd_listen,  CMD_FLAG_DEBUG},
    {"GSCAN",   cmd_gscan,   CMD_FLAG_DEBUG},
#endif

    {NULL, NULL, 0}  // Sentinel
};

#define CMD_TABLE_SIZE (sizeof(cmd_table) / sizeof(cmd_table[0]) - 1)

/*===========================================================================*/
/* Internal Command Processing                                               */
/*===========================================================================*/

/* REVIEW FIX: a stale duplicate of process_serial_char() lived here — dead
 * (serial_console.c owns the live one, which check_serial_commands() calls)
 * and behind it: it lacked the discarding_line paste-injection fix. It
 * compiled silently only because build_flags_base carried
 * -Wno-unused-function, which beat the -Wall added later; both are gone now. */
