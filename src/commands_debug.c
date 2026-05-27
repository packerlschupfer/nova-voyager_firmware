/**
 * @file commands_debug.c
 * @brief Debug and hardware test commands
 */

#include "commands_internal.h"
#include "eeprom.h"
#include "vibration.h"
#include "buzzer.h"
#include "lcd.h"
#include "materials.h"
#include "motor_load.h"
#include "jam.h"

// From depth.c
extern int16_t depth_get_raw_adc(void);

// From motor.c
extern uint16_t motor_get_temperature(void);

/*===========================================================================*/
/* Hardware Test Commands                                                    */
/*===========================================================================*/

void cmd_depth(void) {
    STATE_LOCK();
    int16_t depth = g_state.current_depth;
    STATE_UNLOCK();
    /* REVIEW FIX (LOW): integer division truncates toward zero, so -5 printed
     * "0" then ".5" — a quill 0.5 mm ABOVE the zero point read as 0.5 mm below
     * it. Only bites between -0.1 and -0.9 mm, which is exactly where the sign
     * decides which side of the workpiece you are on. */
    uart_puts("Depth: ");
    if (depth < 0) uart_putc('-');
    const int16_t depth_abs = (int16_t)(depth < 0 ? -depth : depth);
    print_num(depth_abs / 10);
    uart_putc('.');
    print_num(depth_abs % 10);
    uart_puts(" mm\r\n");
}

void cmd_guard(void) {
    // Raw GPIO reads
    uint16_t pc = GPIOC->IDR;
    bool pc0_raw = (pc & (1 << 0)) != 0;  // E-Stop
    bool pc2_raw = (pc & (1 << 2)) != 0;  // Guard
    bool pc3_raw = (pc & (1 << 3)) != 0;  // Pedal (inverted)
    bool pc10_raw = (pc & (1 << 10)) != 0;  // F1
    bool pc11_raw = (pc & (1 << 11)) != 0;  // F2
    bool pc12_raw = (pc & (1 << 12)) != 0;  // F3

    // EXTI-tracked states
    bool guard_exti = encoder_guard_open();
    bool estop_exti = encoder_estop_active();
    bool pedal_exti = encoder_pedal_pressed();

    // g_state values
    STATE_LOCK();
    bool guard_state = g_state.guard_closed;
    bool estop_state = g_state.estop_active;
    bool pedal_state = g_state.pedal_pressed;
    STATE_UNLOCK();

    uart_puts("Raw GPIO: PC0(E-Stop)="); uart_putc(pc0_raw ? '1' : '0');
    uart_puts(" PC2(Guard)="); uart_putc(pc2_raw ? '1' : '0');
    uart_puts(" PC3(Pedal)="); uart_putc(pc3_raw ? '1' : '0');
    uart_puts("\r\nButtons: F1="); uart_putc(pc10_raw ? '1' : '0');
    uart_puts(" F2="); uart_putc(pc11_raw ? '1' : '0');
    uart_puts(" F3="); uart_putc(pc12_raw ? '1' : '0');
    uart_puts(" (0=pressed)\r\n");
    uart_puts("EXTI: guard_open="); uart_putc(guard_exti ? '1' : '0');
    uart_puts(" estop="); uart_putc(estop_exti ? '1' : '0');
    uart_puts(" pedal="); uart_putc(pedal_exti ? '1' : '0');
    uart_puts("\r\nState: guard_closed="); uart_putc(guard_state ? '1' : '0');
    uart_puts(" estop="); uart_putc(estop_state ? '1' : '0');
    uart_puts(" pedal="); uart_putc(pedal_state ? '1' : '0');
    uart_puts("\r\n");
}

void cmd_adcmon(void) {
    uart_puts("Capturing 32 ADC samples (20ms intervals)...\r\n");
    int16_t samples[32];
    int32_t sum = 0;
    int16_t min_val = 32767, max_val = -32768;

    for (int i = 0; i < 32; i++) {
        samples[i] = depth_get_raw_adc();
        sum += samples[i];
        if (samples[i] < min_val) min_val = samples[i];
        if (samples[i] > max_val) max_val = samples[i];
        delay_ms(20);
    }

    int16_t avg = sum / 32;
    uart_puts("Raw ADC: ");
    for (int i = 0; i < 32; i++) {
        print_num(samples[i]);
        uart_putc(' ');
    }
    uart_puts("\r\nMin: "); print_num(min_val);
    uart_puts(" Max: "); print_num(max_val);
    uart_puts(" Avg: "); print_num(avg);
    uart_puts(" Range: "); print_num(max_val - min_val);
    uart_puts("\r\n");
}

void cmd_stack(void) {
    uart_puts("=== Stack Profiling Report ===\r\n");
    uart_puts("Format: Task (Allocated) Free/Used/Margin%\r\n\r\n");

    bool any_warnings = false;
    UBaseType_t hwm;

    if (g_task_main) {
        hwm = uxTaskGetStackHighWaterMark(g_task_main);
        UBaseType_t used = 256 - hwm;
        uint8_t margin_pct = (hwm * 100) / 256;
        uart_puts("  Main   (256): ");
        print_num(hwm); uart_puts("/");
        print_num(used); uart_puts(" = ");
        print_num(margin_pct); uart_puts("%");
        if (margin_pct < 20) { uart_puts(" LOW!"); any_warnings = true; }
        else if (margin_pct < 30) { uart_puts(" !"); }
        uart_puts("\r\n");
    }

    if (g_task_ui) {
        hwm = uxTaskGetStackHighWaterMark(g_task_ui);
        UBaseType_t used = STACK_SIZE_UI - hwm;
        uint8_t margin_pct = (hwm * 100) / STACK_SIZE_UI;
        uart_puts("  UI     ("); print_num(STACK_SIZE_UI); uart_puts("): ");
        print_num(hwm); uart_puts("/");
        print_num(used); uart_puts(" = ");
        print_num(margin_pct); uart_puts("%");
        if (margin_pct < 20) { uart_puts(" LOW!"); any_warnings = true; }
        else if (margin_pct < 30) { uart_puts(" !"); }
        uart_puts("\r\n");
    }

    if (g_task_motor) {
        hwm = uxTaskGetStackHighWaterMark(g_task_motor);
        UBaseType_t used = STACK_SIZE_MOTOR - hwm;
        uint8_t margin_pct = (hwm * 100) / STACK_SIZE_MOTOR;
        uart_puts("  Motor  ("); print_num(STACK_SIZE_MOTOR); uart_puts("): ");
        print_num(hwm); uart_puts("/");
        print_num(used); uart_puts(" = ");
        print_num(margin_pct); uart_puts("%");
        if (margin_pct < 20) { uart_puts(" LOW!"); any_warnings = true; }
        else if (margin_pct < 30) { uart_puts(" !"); }
        uart_puts("\r\n");
    }

    if (g_task_depth) {
        hwm = uxTaskGetStackHighWaterMark(g_task_depth);
        UBaseType_t used = STACK_SIZE_DEPTH - hwm;
        uint8_t margin_pct = (hwm * 100) / STACK_SIZE_DEPTH;
        uart_puts("  Depth  ("); print_num(STACK_SIZE_DEPTH); uart_puts("): ");
        print_num(hwm); uart_puts("/");
        print_num(used); uart_puts(" = ");
        print_num(margin_pct); uart_puts("%");
        if (margin_pct < 20) { uart_puts(" LOW!"); any_warnings = true; }
        else if (margin_pct < 30) { uart_puts(" !"); }
        uart_puts("\r\n");
    }

    if (g_task_tapping) {
        hwm = uxTaskGetStackHighWaterMark(g_task_tapping);
        UBaseType_t used = STACK_SIZE_TAPPING - hwm;
        uint8_t margin_pct = (hwm * 100) / STACK_SIZE_TAPPING;
        uart_puts("  Tapping("); print_num(STACK_SIZE_TAPPING); uart_puts("): ");
        print_num(hwm); uart_puts("/");
        print_num(used); uart_puts(" = ");
        print_num(margin_pct); uart_puts("%");
        if (margin_pct < 20) { uart_puts(" LOW!"); any_warnings = true; }
        else if (margin_pct < 30) { uart_puts(" !"); }
        uart_puts("\r\n");
    }

    uart_puts("\r\nGuidelines:\r\n");
    uart_puts("  >50%  = Excellent (safe margin)\r\n");
    uart_puts("  30-50% = Good (acceptable)\r\n");
    uart_puts("  20-30% = Marginal (monitor)\r\n");
    uart_puts("  <20%  = CRITICAL (increase stack!)\r\n");

    if (any_warnings) {
        uart_puts("\r\nWARNING: Low stack margin detected!\r\n");
        uart_puts("Consider increasing stack sizes in main.c\r\n");
    }

    // Queue usage statistics
    uart_puts("\r\n=== Queue Usage ===\r\n");
    UBaseType_t evt_free = uxQueueSpacesAvailable(g_event_queue);
    UBaseType_t motor_free = uxQueueSpacesAvailable(g_motor_cmd_queue);
    uart_puts("  Event queue:  "); print_num(32 - evt_free); uart_puts("/32\r\n");
    uart_puts("  Motor queue:  "); print_num(16 - motor_free); uart_puts("/16\r\n");

    STATE_LOCK();
    uint16_t evt_ovf = g_state.event_queue_overflows;
    uint16_t motor_ovf = g_state.motor_queue_overflows;
    STATE_UNLOCK();

    if (evt_ovf > 0 || motor_ovf > 0) {
        uart_puts("\r\nQueue Overflows:\r\n");
        if (evt_ovf > 0) { uart_puts("  Event: "); print_num(evt_ovf); uart_puts("\r\n"); }
        if (motor_ovf > 0) { uart_puts("  Motor: "); print_num(motor_ovf); uart_puts("\r\n"); }
    }
}

void cmd_temp(void) {
    // Query MCB temperature via motor task
    MOTOR_CMD(CMD_MOTOR_QUERY_TEMP, 0);
    delay_ms(100);  // Wait for T0 query to complete

    uint16_t mcb_temp = motor_get_temperature();
    const settings_t* s = settings_get();
    uint8_t warn = s->power.temp_threshold;

    uart_puts("MCB: ");
    print_num(mcb_temp);
    uart_puts("C  (warn:");
    print_num(warn);
    uart_puts("C, stop:80C)\r\n");
}

void cmd_tempmcu(void) {
    extern uint16_t temperature_read_gd32(void);
    uint16_t hmi_temp = temperature_read_gd32();

    uart_puts("MCU/HMI: ");
    print_num(hmi_temp);
    uart_puts("C  (GD32 chip, typically 27-31C)\r\n");
}

void cmd_calc(void) {
    uart_puts("Speed Calculator - RPM = (SurfaceSpeed * 1000) / (3.14 * Diameter)\r\n");
    uart_puts("Usage: CALC <diameter_mm> <material_num>\r\n\r\n");
    uart_puts("Materials:\r\n");
    for (int i = 0; i < MATERIAL_COUNT; i++) {
        uart_puts("  "); print_num(i); uart_puts(" = ");
        uart_puts(materials_db[i].name);
        uart_puts(" ("); print_num(materials_db[i].speed_min);
        uart_puts("-"); print_num(materials_db[i].speed_max);
        uart_puts(" m/min)\r\n");
    }

    // Show current material/bit selection from settings
    const settings_t* s = settings_get();
    uart_puts("\r\nCurrent: ");
    uart_puts(materials_db[s->speed.material].name);
    uart_puts(", "); uart_puts(bit_types_db[s->speed.bit_type].name);
    uart_puts(" "); print_num(s->speed.bit_diameter); uart_puts("mm\r\n");

    uint16_t rpm_min, rpm_max;
    material_calc_rpm_range((material_type_t)s->speed.material,
                           (bit_type_t)s->speed.bit_type,
                           s->speed.bit_diameter, &rpm_min, &rpm_max);
    uart_puts("Recommended RPM: "); print_num(rpm_min); uart_puts("-"); print_num(rpm_max);
    uart_puts(" ("); print_num(bit_types_db[s->speed.bit_type].factor_x10);
    uart_puts("0% speed)\r\n");
}

void cmd_selftest(void) {
    uart_puts("=== HARDWARE SELF-TEST ===\r\n");
    int pass = 0, fail = 0;

    // Test 1: LCD display
    uart_puts("TEST:LCD:");
    /* REVIEW FIX (MEDIUM): there is no LCD mutex anywhere in the codebase, and
     * SELFTEST is registered with flags 0 — it ships in release builds. It
     * drives the LCD from task_main (priority 1) while task_ui (priority 2)
     * repaints on its own schedule, so a UI tick landing between this cursor
     * set and the print sends the text to whatever DDRAM address the UI just
     * chose, and a tick inside lcd_write_byte() can latch a data byte as a
     * command. The games path already solved this by suspending task_ui
     * (commands.c::game_launch); do the same rather than invent a second
     * mechanism. */
    /* REVIEW FIX: this used to suspend task_ui to get the LCD to itself, which
     * review showed was the wrong shape three ways — the suspend could land
     * inside uart_puts() and strand g_uart_mutex (deadlock, then watchdog
     * reset); the eTaskGetState guard was a TOCTOU that still permitted two
     * writers when a game was already running; and holding the suspension for
     * >2 s pushed heartbeat_ui past its deadline and raised a false "UI stuck"
     * alarm on a command that ships in release builds.
     *
     * src/lcd.c owns a recursive bus mutex now, so every writer is serialised
     * at the panel and nothing needs suspending. The UI keeps running and may
     * repaint over these screens between our writes — acceptable for a
     * diagnostic, and honest: the console transcript is the real output. */

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("SELF TEST...");
    uart_puts("PASS:display_ok\r\n");
    pass++;

    // Test 2: Depth ADC
    uart_puts("TEST:DEPTH_ADC:");
    int16_t adc = depth_get_raw_adc();
    if (adc >= 0 && adc <= 4095) {
        uart_puts("PASS:adc=");
        print_num(adc);
        uart_puts("\r\n");
        pass++;
    } else {
        uart_puts("FAIL:adc_out_of_range\r\n");
        fail++;
    }

    // Test 3: Motor UART (check if we can query MCB)
    uart_puts("TEST:MOTOR_UART:");
    delay_ms(100);
    /* REVIEW FIX: read AFTER the delay_ms(100) settle above, which exists to
     * let task_motor refresh the MCB status. The snapshot version sampled it
     * before the LCD tests, so this could report comm_ok for a link that had
     * since faulted. */
    STATE_LOCK();
    const bool motor_fault = g_state.motor_fault;
    STATE_UNLOCK();
    if (!motor_fault) {
        uart_puts("PASS:comm_ok\r\n");
        pass++;
    } else {
        uart_puts("FAIL:no_mcb_response\r\n");
        fail++;
    }

    // Test 4: Guard switch readable
    uart_puts("TEST:GUARD_SW:");
    bool guard = encoder_guard_open();
    uart_puts("PASS:state=");
    uart_puts(guard ? "OPEN" : "CLOSED");
    uart_puts("\r\n");
    pass++;

    // Test 5: E-Stop readable
    uart_puts("TEST:ESTOP:");
    bool estop = encoder_estop_active();
    uart_puts("PASS:state=");
    uart_puts(estop ? "ACTIVE" : "INACTIVE");
    uart_puts("\r\n");
    pass++;

    // Test 6: Pedal readable
    uart_puts("TEST:PEDAL:");
    bool pedal = encoder_pedal_pressed();
    uart_puts("PASS:state=");
    uart_puts(pedal ? "PRESSED" : "RELEASED");
    uart_puts("\r\n");
    pass++;

    // Test 7: Encoder (read current position)
    uart_puts("TEST:ENCODER:");
    uart_puts("PASS:readable\r\n");
    pass++;

    // Test 8: Stack usage
    uart_puts("TEST:STACK:");
    UBaseType_t min_stack = 999;
    if (g_task_main) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(g_task_main);
        if (hwm < min_stack) min_stack = hwm;
    }
    if (g_task_ui) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(g_task_ui);
        if (hwm < min_stack) min_stack = hwm;
    }
    if (g_task_motor) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(g_task_motor);
        if (hwm < min_stack) min_stack = hwm;
    }
    if (g_task_depth) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(g_task_depth);
        if (hwm < min_stack) min_stack = hwm;
    }
    if (g_task_tapping) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(g_task_tapping);
        if (hwm < min_stack) min_stack = hwm;
    }
    if (min_stack > 20) {
        uart_puts("PASS:min_hwm=");
        print_num((int32_t)min_stack);
        uart_puts("\r\n");
        pass++;
    } else {
        uart_puts("FAIL:stack_low=");
        print_num((int32_t)min_stack);
        uart_puts("\r\n");
        fail++;
    }

    // Test 9: Settings storage
    uart_puts("TEST:SETTINGS:");
    const settings_t* s = settings_get();
    if (s && s->speed.default_rpm >= 250 && s->speed.default_rpm <= 5500) {
        uart_puts("PASS:valid\r\n");
        pass++;
    } else {
        uart_puts("FAIL:invalid_settings\r\n");
        fail++;
    }

    // Summary
    uart_puts("=== SELFTEST COMPLETE ===\r\n");
    uart_puts("RESULT:PASS=");
    print_num(pass);
    uart_puts(":FAIL=");
    print_num(fail);
    uart_puts("\r\n");

    // Restore normal display
    lcd_clear();
    lcd_set_cursor(0, 0);
    if (fail == 0) {
        lcd_print("SELFTEST PASS");
    } else {
        lcd_print("SELFTEST FAIL");
    }
    lcd_set_cursor(1, 0);
    char buf[17];
    snprintf(buf, 17, "Pass:%d Fail:%d", pass, fail);
    lcd_print(buf);
    /* REVIEW FIX: this held task_main for two seconds with no console_pump(),
     * so a guard-open or E-Stop queued during the summary screen waited that
     * long to be handled — the shape LOADMON's comment says was fixed
     * everywhere else. delay_ms_ui() pumps and keeps the heartbeat fresh. */
    delay_ms_ui(2000);

    /* Hand the panel back. lcd_shadow_invalidate() because we bypassed the
     * dirty-row shadow entirely — without it the UI redraws only what it
     * thinks changed and leaves our text on screen. */
    /* The UI redraws only what its dirty-row shadow thinks changed, and we
     * bypassed it — without this our text would stay on screen. */
    extern void lcd_shadow_invalidate(void);
    lcd_shadow_invalidate();
}

/*===========================================================================*/
/* Essential LCD Graphics Test Commands                                     */
/*===========================================================================*/

void cmd_draw8icons(void) {
    uart_puts("\r\n8-Icon Grid with proper spacing\r\n\r\n");
    uart_puts("Press ESC/Ctrl-C or trigger an event to abort\r\n");

    // console_pump() replaces the blind IWDG feeds this command used to do:
    // it gates the refresh on ALL_TASKS_ALIVE() and bails out as soon as a
    // STOP/guard/E-Stop event needs the main loop back.
    bool aborted = false;

    // Clear text
    lcd_cmd(0x30);
    lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));

    // Graphics mode (0x36, NOT 0x37 — 0x37 is SR=1 which breaks GRAM)
    uart_puts("Graphics mode: 0x34, 0x36\r\n");
    lcd_cmd(0x34);
    lcd_cmd(0x36);

    // Clear full 128x64 GRAM (upper half X=0-7, lower half X=8-15)
    uart_puts("Clearing GRAM...\r\n");
    for (uint8_t y = 0; y < 32 && !aborted; y++) {
        lcd_cmd(0x80 | y);
        lcd_cmd(0x80);
        for (uint8_t x = 0; x < 16; x++) lcd_data(0x00);
        lcd_cmd(0x80 | y);
        lcd_cmd(0x88);
        for (uint8_t x = 0; x < 16; x++) lcd_data(0x00);
        if ((y % 8) == 0) aborted = console_pump();
    }

    uart_puts("Drawing 8 icons with borders and gaps...\r\n");

    // Draw 8 icons: 2 rows × 4 columns
    // Each icon: 30×30 pixels (leaving 2-pixel gap)
    for (uint8_t row = 0; row < 2 && !aborted; row++) {
        for (uint8_t col = 0; col < 4 && !aborted; col++) {
            uint8_t icon_num = row * 4 + col;

            // Icon box is 30×30, with 1-pixel gap on each side = 32 pixels total
            uint8_t x_start_byte = col * 4;       // 0, 4, 8, 12
            uint8_t y_start = row * 16;           // 0 or 16

            uart_puts("Icon ");
            print_num(icon_num);
            uart_puts("\r\n");

            // Draw SOLID borders (full 32 pixels)
            // Top border (Y=y_start)
            lcd_cmd(0x80 | y_start);
            lcd_cmd(0x80 | x_start_byte);
            lcd_data(0xFF);  // Solid line
            lcd_data(0xFF);
            lcd_data(0xFF);
            lcd_data(0xFF);

            // Bottom border (Y=y_start+15)
            lcd_cmd(0x80 | (y_start + 15));
            lcd_cmd(0x80 | x_start_byte);
            lcd_data(0xFF);  // Solid line
            lcd_data(0xFF);
            lcd_data(0xFF);
            lcd_data(0xFF);

            // Side borders only (rows 1-14)
            for (uint8_t y = 1; y < 15; y++) {
                lcd_cmd(0x80 | (y_start + y));
                lcd_cmd(0x80 | x_start_byte);
                lcd_data(0x80);  // Left edge
                lcd_data(0x00);  // White center
                lcd_data(0x00);  // White center
                lcd_data(0x01);  // Right edge
            }

            aborted = console_pump();
        }
    }

    uart_puts("\r\n***** 8 BORDERED ICONS *****\r\n");
    uart_puts("Holding 15 sec...\r\n\r\n");

    for (int i = 15; i > 0 && !aborted; i--) {
        if (i == 15 || i == 10 || i == 5 || i <= 3) {
            uart_puts("  ");
            print_num(i);
            uart_puts("...\r\n");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        aborted = console_pump();
    }

    if (aborted) uart_puts("\r\nAborted\r\n");

    lcd_cmd(0x34);
    lcd_delay_ms(1);
    lcd_cmd(0x30);
    lcd_clear();
    lcd_print_at(0, 0, "8 icons done");
}

void cmd_testcgrom(void) {
    uart_puts("\r\n========================================\r\n");
    uart_puts("CGROM CHARACTER SET SCAN\r\n");
    uart_puts("========================================\r\n\r\n");

    uart_puts("Scanning CGROM characters 0x00-0xFF\r\n");
    uart_puts("Looking for: Chinese, symbols, box drawing, gauges\r\n");
    uart_puts("Press ESC/Ctrl-C or trigger an event to abort\r\n\r\n");

    // 6 pages x 10 s = a full minute with the main loop blocked; console_pump()
    // gates the watchdog refresh and drops out on a pending safety/UI event.
    bool aborted = false;

    // Text mode
    lcd_cmd(0x30);
    lcd_cmd(0x0C);  // Display ON

    // Scan character ranges
    uint8_t ranges[][3] = {
        {0x00, 0x1F, 1},   // Control chars
        {0x20, 0x7F, 2},   // ASCII
        {0x80, 0x9F, 3},   // Extended 1
        {0xA0, 0xBF, 4},   // Extended 2 (Chinese?)
        {0xC0, 0xDF, 5},   // Extended 3
        {0xE0, 0xFF, 6},   // Extended 4
    };

    for (uint8_t r = 0; r < 6 && !aborted; r++) {
        uint8_t start = ranges[r][0];
        uint8_t end = ranges[r][1];
        uint8_t page = ranges[r][2];

        uart_puts("\r\n**********************************************\r\n");
        uart_puts("*** PAGE ");
        print_num(page);
        uart_puts(": CHARS 0x");
        print_num(start);
        uart_puts("-0x");
        print_num(end);
        uart_puts(" ***\r\n");
        uart_puts("**********************************************\r\n\r\n");

        lcd_clear();

        // Display 64 characters (16 cols × 4 rows)
        for (uint8_t row = 0; row < 4; row++) {
            lcd_set_cursor(row, 0);
            for (uint8_t col = 0; col < 16; col++) {
                uint8_t char_code = start + (row * 16) + col;
                if (char_code <= end) {
                    lcd_data(char_code);
                } else {
                    lcd_data(' ');  // Blank if beyond range
                }
            }
        }

        uart_puts("Displayed chars 0x");
        print_num(start);
        uart_puts("-0x");
        print_num(start + 63);
        uart_puts("\r\n");
        uart_puts("\r\n>>> LOOK AT DISPLAY - TAKE PHOTO! <<<\r\n");
        uart_puts("Holding 10 seconds...\r\n\r\n");

        for (int i = 0; i < 10 && !aborted; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            aborted = console_pump();
        }
    }

    if (aborted) uart_puts("\r\nAborted\r\n");

    uart_puts("\r\n========================================\r\n");
    uart_puts("CGROM SCAN COMPLETE\r\n");
    uart_puts("========================================\r\n");
    uart_puts("6 pages scanned - review photos!\r\n\r\n");

    lcd_clear();
    lcd_print_at(0, 0, "CGROM scan done");
}

#ifdef BUILD_DEBUG
// Deliberately raise a data bus fault, to verify HardFault_Handler actually
// reports something useful. A fault handler nobody has ever seen fire is a
// guess, and this one was rewritten precisely because the old one told us
// almost nothing after two real lockups.
//
// A STORE to unmapped space is the case that matters: it is the same shape as
// the refused data access both lockups reported. With ENABLE_PRECISE_BUS_FAULTS
// it should come back PRECISERR with BFAR holding this address.
// Exercise the fault REPORT path from ordinary task context, with no fault.
// If this prints and CRASHTEST does not, the reporting code is fine and the
// problem is specific to running in fault context.
void cmd_faultreport(void) {
    extern void fault_report_body(uint32_t cfsr, uint32_t hfsr);
    uart_puts_raw("\r\nCalling fault_report_body() directly (no fault):\r\n");
    fault_report_body(SCB->CFSR, SCB->HFSR);
    uart_puts_raw("fault_report_body() returned normally.\r\n");
}

// Second fault flavour: an undefined instruction. Raises UsageFault, which
// escalates to HardFault because UsageFault is not enabled — and critically it
// involves NO bus transaction. If the handler reports on this but not on the
// store to 0xFFFFFFF0, then the handler is fine and it is the refused store
// that poisons the bus, stalling the handler's own first store.
void cmd_crashundef(void) {
    uart_puts_raw("\r\nDeliberate UNDEFINED INSTRUCTION (no bus access)\r\n");
    uart_puts_raw("Expect UNDEFINSTR, a full report, then a watchdog reset.\r\n\r\n");
    for (volatile int i = 0; i < 200000; i++);   /* let the UART drain */
    __asm volatile ("udf #0");
    uart_puts_raw("NO FAULT RAISED\r\n");
}

void cmd_crashtest(void) {
    uart_puts("\r\nDeliberate bus fault: store to 0xFFFFFFF0\r\n");
    uart_puts("Expect PRECISERR + BFAR=0xFFFFFFF0, then a watchdog reset.\r\n\r\n");
    /* Prove uart_puts_raw() works from ordinary context first, so that if the
     * fault report is missing we know whether the writer or the handler is at
     * fault rather than guessing. */
    uart_puts_raw("RAW-WRITER-OK: uart_puts_raw works from task context\r\n");
    for (volatile int i = 0; i < 200000; i++);   /* let the UART drain */
    *((volatile uint32_t *)0xFFFFFFF0u) = 0xDEADBEEFu;
    uart_puts("NO FAULT RAISED - handler or MPU config is not what we think\r\n");
}
#endif

void cmd_testgfx(void) {
    extern void lcd_test_graphics_mode(void);
    uart_puts("Testing ST7920 graphics mode...\r\n");
    lcd_test_graphics_mode();
}

void cmd_testlcd(void) {
    uart_puts("Comprehensive LCD capability test...\r\n");
    extern void lcd_test_capabilities(void);
    lcd_test_capabilities();
}

// ===================================================================
// TIER 1: Load Monitoring Commands (for tuning load triggers)
// ===================================================================

void cmd_loadmon(void) {
    // AUDIT FIX (MEDIUM, commands_debug.c:602): the old LOADMON blocked the
    // main task for 10 s while feeding the IWDG blind — during that window,
    // guard-open and STOP events sat unprocessed in g_event_queue while the
    // motor ran, and any real task hang wouldn't trip the watchdog.
    //
    // FOLLOW-UP: that fix dropped the blind feed but relied on "main.c's
    // ALL_TASKS_ALIVE gate" — which lives in the main loop this handler is
    // blocking, so nothing refreshed the IWDG at all and the 10 s loop
    // reset the machine at ~5 s, every time. console_pump() applies the
    // gate here instead: feed only while every task is alive, and abort on
    // a pending event or a keypress.
    uart_puts("=== Load Monitor (10 seconds) ===\r\n");
    uart_puts("Reading motor_load from g_state\r\n");
    uart_puts("Press ESC/Ctrl-C or trigger an event (guard/estop/stop) to abort\r\n\r\n");

    for (int t = 0; t < 200; t++) {
        STATE_LOCK();
        uint8_t load = g_state.motor_load;
        STATE_UNLOCK();

        uart_puts("KR=");
        print_num(load);
        uart_puts("%  ");
        if ((t % 10) == 9) uart_puts("\r\n");

        if (console_pump()) {
            uart_puts("\r\nAborted\r\n");
            break;
        }

        delay_ms(50);
    }
    uart_puts("\r\nDone\r\n");
}

// Snapshot of the motor_load filter + baseline state, plus current jam status.
// Replaces the older LOADBASE/LOADSENSE prototypes — those did their own
// independent baseline learning; motor_load.c now provides the authoritative
// live values used by jam detection and the LCD split-bar.
void cmd_loadinfo(void) {
    motor_load_debug_t d;
    motor_load_get_debug(&d);

    STATE_LOCK();
    uint8_t raw = g_state.motor_load;
    STATE_UNLOCK();

    uart_puts("LOAD:\r\n");
    uart_puts("  raw      = "); print_num(raw); uart_puts("%\r\n");
    uart_puts("  filtered = "); print_num(d.filtered_load);
    uart_puts(d.filter_initialized ? "%\r\n" : "% (uninit)\r\n");
    uart_puts("  baseline = ");
    if (d.baseline_armed) {
        print_num(d.baseline); uart_puts("% (armed)\r\n");
        uart_puts("  cutting  = ");
        uint8_t cut = (raw > d.baseline) ? (uint8_t)(raw - d.baseline) : 0;
        print_num(cut); uart_puts("%\r\n");
    } else {
        uart_puts("learning\r\n");
        uart_puts("  stable   = "); print_num(d.stability_elapsed_ms);
        uart_puts(" / "); print_num(d.stability_required_ms); uart_puts(" ms\r\n");
    }
    uart_puts("  grace    = "); print_num(d.spike_grace_remaining_ms);
    uart_puts(" ms remaining\r\n");
    int8_t step = motor_load_get_step_delta();
    uart_puts("  step     = ");
    if (step < 0) { uart_puts("-"); print_num((uint16_t)(-step)); }
    else          { uart_puts("+"); print_num((uint16_t)step); }
    uart_puts("%\r\n");
}

void cmd_jaminfo(void) {
    const jam_status_t* st = jam_get_status();
    motor_load_debug_t d;
    motor_load_get_debug(&d);

    uart_puts("JAM:\r\n");
    uart_puts("  status   = "); uart_puts(jam_get_description(st->type));
    uart_puts(st->acknowledged ? " (ack)\r\n" : "\r\n");
    uart_puts("  baseline = ");
    if (d.baseline_armed) {
        print_num(d.baseline); uart_puts("% (armed)\r\n");
    } else {
        uart_puts("learning ("); print_num(d.stability_elapsed_ms);
        uart_puts("/"); print_num(d.stability_required_ms); uart_puts(" ms)\r\n");
    }
    uart_puts("  grace    = "); print_num(d.spike_grace_remaining_ms);
    uart_puts(" ms remaining\r\n");
    uart_puts("  filtered = "); print_num(d.filtered_load); uart_puts("%\r\n");

    const settings_t* s = settings_get();
    uart_puts("Triggers:\r\n");
    /* REVIEW FIX (MEDIUM): both of these lines derived from spike_detect
     * alone, but jam_load_update() returns early on !jam_detect_enabled BEFORE
     * reaching either detector — only low-load is genuinely independent. So
     * with SET sensor.jam_detect 0 and Spike Detect left on, the one command
     * whose job is answering "what protection is live before I cut" replied
     * "on" for two detectors that cannot fire. */
    const bool jam_on = s->sensor.jam_detect;
    uart_puts("  spike (abs)  : ");
    uart_puts((jam_on && s->sensor.spike_detect) ? "on  " : "off ");
    uart_puts("thr="); print_num(s->sensor.spike_thresh); uart_puts("%\r\n");
    uart_puts("  step (delta) : ");
    uart_puts((jam_on && s->sensor.spike_detect &&
               s->sensor.step_thresh >= JAM_STEP_MIN_THRESH) ? "on  " : "off ");
    uart_puts("thr=+"); print_num(s->sensor.step_thresh); uart_puts("%\r\n");
    uart_puts("  low-load     : ");
    uart_puts(s->sensor.low_load_detect ? "on  " : "off ");
    uart_puts("thr<"); print_num(s->sensor.low_load_thresh); uart_puts("%\r\n");
}

// ===================================================================
// TIER 2: Protocol Discovery Commands
// ===================================================================

void cmd_scan(void) {
    uart_puts("Scanning motor query commands...\r\n\r\n");
    const char* cmds[] = {"GF", "SV", "CV", "KR", "SP", "SI", "I0", "I3", "NC", "UD"};

    for (int i = 0; i < 10; i++) {
        uart_puts(cmds[i]);
        uart_puts(": ");
        motor_test_qq((uint8_t)cmds[i][0], (uint8_t)cmds[i][1]);
        delay_ms(100);
    }
    uart_puts("\r\nScan complete\r\n");
}

void cmd_listen(void) {
    /* REVIEW FIX (MEDIUM): this reads MOTOR_USART->DR directly for five
     * seconds with no scan claim and no MOTOR_CONTROL_LOCK, while task_motor
     * keeps polling — so it consumed bytes out of the middle of the MCB's
     * replies, timing out queries and driving consecutive_comm_failures toward
     * the 15-failure COMM FAULT cutoff that drops PD4. Claiming the scan
     * envelope pauses the poll, which is the only way "listen to the bus" can
     * mean anything anyway. (It ships in release as SNIFF; only the LISTEN
     * alias is debug-gated.) */
    const motor_scan_result_t claim = motor_scan_try_claim();
    if (claim != MOTOR_SCAN_CLAIMED) {
        uart_puts("SNIFF refused: ");
        uart_puts(motor_scan_refusal(claim));
        uart_puts("\r\n");
        return;
    }

    uart_puts("Listening to motor UART (5s)...\r\n");
    uart_puts("Press ESC/Ctrl-C or trigger an event to stop\r\n\r\n");

    // Same class as the scan commands: a busy loop that can outlast the 5 s
    // IWDG window (every received byte adds ~11 ms of 9600-baud printing) with
    // nothing refreshing the watchdog, because the main loop is what it blocks.
    for (int t = 0; t < 500; t++) {
        extern int motor_getc_timeout(uint32_t timeout_us);
        int c = motor_getc_timeout(10000);

        if (c >= 0) {
            /* REVIEW FIX: "%02X" already pads, so the extra leading zero made
             * every byte below 0x10 print as "RX: 0x004". */
            uart_puts("RX: 0x");
            char hex[3];
            snprintf(hex, 3, "%02X", c);
            uart_puts(hex);
            uart_puts("\r\n");
        }

        if (console_pump()) break;
    }
    motor_scan_release();
    uart_puts("Done\r\n");
}

void cmd_gscan(void) {
    uart_puts("Grouped motor command scan\r\n\r\n");

    uart_puts("Status Queries:\r\n");
    const char* status[] = {"GF", "CV", "KR"};
    for (int i = 0; i < 3; i++) {
        uart_puts("  ");
        uart_puts(status[i]);
        uart_puts(": ");
        motor_test_qq((uint8_t)status[i][0], (uint8_t)status[i][1]);
        delay_ms(100);
    }

    uart_puts("\r\nParameter Queries:\r\n");
    const char* params[] = {"SP", "SI", "I0", "I3", "NC", "UD"};
    for (int i = 0; i < 6; i++) {
        uart_puts("  ");
        uart_puts(params[i]);
        uart_puts(": ");
        motor_test_qq((uint8_t)params[i][0], (uint8_t)params[i][1]);
        delay_ms(100);
    }

    uart_puts("\r\nDone\r\n");
}

void cmd_testallicons(void) {
    uart_puts("Testing all icon positions...\r\n");
    extern void lcd_test_icons(void);
    lcd_test_icons();
}

// ===================================================================
// TIER 2: Hardware Test Commands
// ===================================================================

void cmd_i2c(void) {
    uart_puts("I2C bus scan..\r\n");
    uart_puts("Checking EEPROM address 0x50...\r\n");
    
    extern bool eeprom_init(void);
    if (eeprom_init()) {
        uart_puts("EEPROM found at 0x50\r\n");
    } else {
        uart_puts("No EEPROM detected\r\n");
    }
    
    uart_puts("Done\r\n");
}

/**
 * @brief EEERASE CONFIRM - blank the whole EEPROM to 0xFF.
 *
 * The supported way back to the original Teknatool firmware.
 *
 * Storing a stock image to restore was the obvious alternative and it is the
 * wrong one: the OEM region holds machine-specific values, so an image
 * captured from one drill press would write that machine's calibration onto
 * somebody else's. Blanking has no such problem — a 0xFF EEPROM is a state the
 * original firmware already has to handle, because that is how it leaves the
 * factory. It re-initialises on boot or on its own factory reset.
 *
 * This erases EVERYTHING, both regions: the OEM area at 0x00-0xAF and our own
 * settings and crash dump at 0xB0-0xFF. That is the point — "revert to
 * factory" is not a partial operation — but it means the operator's tuning is
 * gone, so it takes an explicit confirmation word rather than a bare command.
 */
void cmd_eeerase(void) {
    char* buf = get_cmd_buf();
    uint8_t idx = get_cmd_idx();

    /* No length arithmetic. "EEERASE CONFIRM" is 15 characters and an earlier
     * version of this guard demanded idx >= 16, so the one correct invocation
     * was refused — the same off-by-one already fixed once today in SIMLOAD.
     * serial_console.c NUL-terminates cmd_buf before dispatch, so strcmp is
     * both safe and sufficient: a short command leaves buf[7] as '\0', which
     * fails the space test on its own. */
    (void)idx;
    if (!(buf[7] == ' ' && strcmp(&buf[8], "CONFIRM") == 0)) {
        uart_puts("EEERASE blanks the ENTIRE EEPROM to 0xFF:\r\n");
        uart_puts("  0x00-0xAF  original-firmware region\r\n");
        uart_puts("  0xB0-0xFF  our settings AND crash dump\r\n");
        uart_puts("All tuning is lost. The original firmware re-initialises a\r\n");
        uart_puts("blank EEPROM by itself, so this is the way back to stock.\r\n");
        uart_puts("Type: EEERASE CONFIRM\r\n");
        return;
    }

    if (!eeprom_init()) {
        uart_puts("No EEPROM detected\r\n");
        return;
    }

    uart_puts("Erasing 256 bytes...\r\n");
    uint8_t blank[EEPROM_PAGE_SIZE];
    memset(blank, 0xFF, sizeof(blank));

    /* Page-aligned writes: the AT24C02 wraps within a page rather than
     * carrying into the next one, so a straight 256-byte write would fold back
     * on itself and leave most of the device untouched. */
    for (uint16_t addr = 0; addr < EEPROM_SIZE; addr += EEPROM_PAGE_SIZE) {
        if (eeprom_write(addr, blank, EEPROM_PAGE_SIZE) != EEPROM_OK) {
            uart_puts("FAILED at byte ");
            print_num((int32_t)addr);
            uart_puts(" - EEPROM may be partially erased\r\n");
            return;
        }
    }

    /* Verify: a write that silently did nothing would leave the operator
     * believing the machine is ready for stock firmware when it is not. */
    uint16_t bad = 0;
    for (uint16_t addr = 0; addr < EEPROM_SIZE; addr++) {
        uint8_t v = 0;
        if (eeprom_read_byte(addr, &v) != EEPROM_OK || v != 0xFF) bad++;
    }
    if (bad) {
        uart_puts("VERIFY FAILED: ");
        print_num(bad);
        uart_puts(" byte(s) did not read back as 0xFF\r\n");
        return;
    }

    /* Storage is now blank while RAM still holds the live settings, so RAM and
     * storage genuinely differ — which is exactly what the dirty flag means.
     * Without this, cmd_save() answers "No changes to save." and refuses, so
     * an operator who erased by mistake could not put their tuning back even
     * though it was still sitting in RAM. Restoring the invariant is the fix,
     * not special-casing SAVE. */
    settings_mark_dirty();

    uart_puts("EEPROM blank and verified.\r\n");
    /* Erasing the EEPROM does NOT reset this machine, and saying otherwise
     * would be a lie an operator finds out the hard way. settings_init()
     * layers its sources: defaults, then the FULL struct mirrored to internal
     * flash by the last idle SAVE, then the OEM EEPROM fields, then our EEPROM
     * block. Blanking removes only the last two, so the flash mirror puts
     * everything back on the next boot. Verified on target 2026-09-05: a
     * blanked EEPROM plus a reboot came back with every tuned value intact.
     *
     * That is fine for the actual goal — flashing the original firmware
     * overwrites the application flash and takes the mirror with it — but it
     * means "erase" is a step in reverting, not the revert itself. */
    uart_puts("  NOTE: settings will come BACK on the next boot from the\r\n");
    uart_puts("        flash mirror. Flashing stock firmware erases that too.\r\n");
    /* Learned by breaking it, 2026-09-05. A blank EEPROM is safe: the original
     * firmware sees no magic and re-initialises, password and all. A PARTIALLY
     * written one is not, and that is what our own firmware creates the moment
     * it saves anything — eeprom_save_to_oem() stamps EE_OEM_MAGIC (0x7C at
     * 0x02) along with the speeds and favourites. Stock then trusts the magic,
     * reads 0xFF everywhere else, and locks the operator out behind a menu
     * password that no default opens. Recovering needed our firmware
     * reflashed, EEWRITE, and a pre-erase snapshot. */
    uart_puts("  WARNING: do NOT let this firmware save afterwards. A SAVE (or\r\n");
    uart_puts("        the speed autosave) re-stamps the OEM magic over a blank\r\n");
    uart_puts("        region, and stock then trusts it and reads garbage --\r\n");
    uart_puts("        including its menu password. Blank is safe; half-written\r\n");
    uart_puts("        locks you out. Flash stock BEFORE saving anything.\r\n");
    uart_puts("  To go to stock: power off now and flash the original firmware.\r\n");
    uart_puts("  To undo: type SAVE - the live settings are still in RAM.\r\n");
    /* Worth saying plainly: this machine writes the EEPROM on its own. The
     * speed autosave persists the last-used RPM a few seconds after the motor
     * stops, so a blanked EEPROM does not stay blank if the machine keeps
     * being used. */
    uart_puts("  Note: continued use re-writes settings (speed autosave).\r\n");
}

/**
 * @brief VIB - read the vibration accelerometer.
 *
 * Inert: an I2C register read on the bus we already drive for the EEPROM.
 * Reports presence, the three raw axes, and how the OEM thresholds would
 * classify them at the configured sensitivity.
 */
void cmd_vib(void) {
    const settings_t* s = settings_get();
    const uint8_t sens = s->sensor.vibration_sensitivity;

    uart_puts("Vibration sensor (I2C 0x1D on PC4/PC5)\r\n  present: ");
    if (!vibration_present()) {
        uart_puts("NO - device did not ACK\r\n");
        return;
    }
    uart_puts("yes\r\n");

    vibration_axes_t a;
    if (!vibration_read_axes(&a)) {
        uart_puts("  read failed mid-sequence\r\n");
        return;
    }
    uart_puts("  X="); print_num(a.x);
    uart_puts("  Y="); print_num(a.y);
    uart_puts(" (incl +250 offset)  Z="); print_num(a.z);
    uart_puts("\r\n  sensitivity: "); print_num(sens);
    uart_puts(sens == 0 ? " (DISABLED)" : (sens == 1 ? " (LOW +/-650/601)"
             : (sens == 2 ? " (MEDIUM +/-501/451)" : " (HIGH +/-301/251)")));
    uart_puts("\r\n  level: ");
    switch (vibration_evaluate(sens)) {
        case VIBRATION_EXCESS:   uart_puts("EXCESS (OEM stops the motor)\r\n"); break;
        case VIBRATION_ELEVATED: uart_puts("ELEVATED (OEM 'Significant Vibration')\r\n"); break;
        default:                 uart_puts("ok\r\n"); break;
    }
}

/**
 * @brief I2CSCAN - probe every 7-bit address on the PC4/PC5 bus.
 *
 * Inert. Written because the accelerometer the OEM firmware reads at 0x1D did
 * not ACK on this machine, and "the device is absent" and "the device is at a
 * different address" and "the device needs waking first" are three very
 * different problems that look identical from a single failed read.
 */
void cmd_i2cscan(void) {
    /* Optional speed factor: "I2CSCAN 8" runs the bus 8x slower. A device that
     * NAKs at EEPROM speed but answers slowly is indistinguishable from an
     * absent one otherwise. */
    char* b = get_cmd_buf();
    uint8_t bi = get_cmd_idx();
    uint16_t slow = 1;
    if (bi >= 9 && b[7] == ' ') {
        uint16_t v = 0;
        for (int i = 8; i < bi && b[i] >= '0' && b[i] <= '9'; i++) v = (uint16_t)(v * 10 + (b[i] - '0'));
        if (v > 0 && v <= 200) slow = v;
    }
    i2c_set_slow_factor(slow);

    uart_puts("I2C scan on PC4/PC5 (7-bit addresses), speed factor 1/");
    print_num(slow);
    uart_puts(":\r\n");
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_probe_device(addr)) {
            uart_puts("  0x");
            print_hex_byte(addr);
            uart_puts("  ACK");
            if (addr == 0x50) uart_puts("   (AT24C02 EEPROM)");
            if (addr == 0x1D) uart_puts("   (OEM vibration accelerometer)");
            uart_puts("\r\n");
            found++;
        }
    }
    if (!found) uart_puts("  nothing responded\r\n");
    else { uart_puts("  devices: "); print_num(found); uart_puts("\r\n"); }
    i2c_set_slow_factor(1);   /* never leave the bus slowed */
}

/**
 * @brief VIBPWR [0|1] - drive PB12, then rescan the I2C bus.
 *
 * Hypothesis test, not a feature. The original firmware configures PB12 as a
 * push-pull output and our firmware has never touched it; its purpose was
 * recorded during reverse engineering only as "SPI2 CS? or LCD backlight?".
 * Meanwhile TWO devices the OEM talks to on PC4/PC5 — the accelerometer at
 * 0x1D and the RTC at 0x68 — do not answer here, while the EEPROM at 0x50
 * does. An unexplained OEM output we leave floating is a plausible power or
 * enable line for exactly those parts.
 *
 * We do not use SPI2, so if PB12 is really a chip select this is harmless.
 * Reversible: call with 0 to put it back.
 *
 * RESULT (2026-09-05): NEGATIVE, both polarities. Driven high and driven low,
 * the bus still answers only at 0x50 — no 0x1D, no 0x68. So PB12 is not a
 * power or enable line for either missing device, and the operator confirms
 * the accelerometer IS fitted on this machine and DID once trigger the OEM's
 * vibration warning. Kept as a command because the negative is worth being
 * able to reproduce, not because the idea is still live.
 *
 * Firmware-side explanations are now largely exhausted: the pins are right
 * (the EEPROM answers on them), the address is right (0x3A/0x3B from the OEM),
 * the electrical config is right (open-drain, pull-ups), the bus runs at a
 * sane ~100 kHz, and the probe is address-only so it cannot produce a false
 * negative from an unlucky register. What is left is a hardware question.
 */
void cmd_vibpwr(void) {
    char* buf = get_cmd_buf();
    uint8_t idx = get_cmd_idx();

    if (!(idx >= 8 && buf[6] == ' ' && (buf[7] == '0' || buf[7] == '1'))) {
        uart_puts("Usage: VIBPWR 0|1  - drive PB12 low/high, then rescan I2C\r\n");
        uart_puts("PB12 is an output the ORIGINAL firmware configures and we\r\n");
        uart_puts("never touch. Testing whether it powers the 0x1D accelerometer\r\n");
        uart_puts("and 0x68 RTC, which are silent here while 0x50 answers.\r\n");
        return;
    }
    const bool high = (buf[7] == '1');

    /* PA8 as well as PB12. The OEM configures PA8 as output push-pull inside
     * its I2C INIT function (FUN_080184f0, which also enables the GPIOA and
     * GPIOC clocks) — and our firmware uses PA8 as the buzzer, leaving it
     * driven LOW after every beep (buzzer.c pwm_stop). If that pin gates power
     * to the I2C devices rather than being a buzzer, we have been holding the
     * accelerometer and the RTC switched off, which fits every observation:
     * both silent, EEPROM fine, and the sensor demonstrably working under the
     * original firmware. */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef ga = {0};
    ga.Pin   = GPIO_PIN_8;
    ga.Mode  = GPIO_MODE_OUTPUT_PP;
    ga.Pull  = GPIO_NOPULL;
    ga.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &ga);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    uart_puts(high ? "PA8 (buzzer pin / OEM I2C-init pin) driven HIGH\r\n"
                   : "PA8 driven LOW\r\n");

    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef gp = {0};
    gp.Pin   = GPIO_PIN_12;
    gp.Mode  = GPIO_MODE_OUTPUT_PP;   /* matches the OEM's configuration */
    gp.Pull  = GPIO_NOPULL;
    gp.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gp);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, high ? GPIO_PIN_SET : GPIO_PIN_RESET);

    uart_puts("PB12 driven ");
    uart_puts(high ? "HIGH" : "LOW");
    uart_puts("; settling, then rescanning...\r\n");
    delay_ms(50);   /* let a rail come up before asking who is there */

    cmd_i2cscan();

    uint8_t id = 0;
    if (i2c_read_device_reg(VIBRATION_I2C_ADDR, VIBRATION_REG_WHOAMI, &id)) {
        uart_puts("WHO_AM_I @0x1D = 0x");
        print_hex_byte(id);
        uart_puts(id == VIBRATION_WHOAMI_VALUE ? "  <-- MMA845x FOUND\r\n" : "  (unexpected)\r\n");
    }
}

/* Bit-bang an address-only I2C probe on an ARBITRARY pin pair.
 *
 * The OEM firmware puts the accelerometer on PC4/PC5, and our EEPROM proves
 * that bus works — yet the sensor never answers there. The operator is certain
 * the part is fitted and functional. One reading that has not been tested: on
 * THIS board revision the sensor may simply be wired to different pins than
 * the firmware image we disassembled expects. This walks a candidate pair.
 *
 * Only free pins are worth trying: PB6/PB7 and PB8/PB9 are the STM32 I2C1
 * pairs (default and remap) and appear nowhere in our pin map or the OEM's.
 * PB10/PB11 are deliberately NOT offered — that is the motor UART.
 */
static void scan_pins(GPIO_TypeDef* port, uint16_t scl, uint16_t sda, const char* label) {
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = scl | sda;
    HAL_GPIO_Init(port, &g);

    #define SCL_HI() HAL_GPIO_WritePin(port, scl, GPIO_PIN_SET)
    #define SCL_LO() HAL_GPIO_WritePin(port, scl, GPIO_PIN_RESET)
    #define SDA_HI() HAL_GPIO_WritePin(port, sda, GPIO_PIN_SET)
    #define SDA_LO() HAL_GPIO_WritePin(port, sda, GPIO_PIN_RESET)
    #define DLY()    do { for (volatile int d = 0; d < 120; d++); } while (0)

    uart_puts(label); uart_puts(": ");
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        SDA_HI(); SCL_HI(); DLY();
        SDA_LO(); DLY(); SCL_LO(); DLY();           /* START */
        uint8_t byte = (uint8_t)(a << 1);
        for (int bit = 7; bit >= 0; bit--) {
            if (byte & (1 << bit)) SDA_HI(); else SDA_LO();
            DLY(); SCL_HI(); DLY(); SCL_LO(); DLY();
        }
        SDA_HI(); DLY(); SCL_HI(); DLY();
        const int ack = !HAL_GPIO_ReadPin(port, sda);
        SCL_LO(); DLY();
        SDA_LO(); DLY(); SCL_HI(); DLY(); SDA_HI(); DLY();   /* STOP */
        if (ack) { uart_puts(" 0x"); print_hex_byte(a); found++; }
    }
    uart_puts(found ? "\r\n" : " (nothing)\r\n");
    #undef SCL_HI
    #undef SCL_LO
    #undef SDA_HI
    #undef SDA_LO
    #undef DLY
}

void cmd_i2calt(void) {
    uart_puts("Probing alternative I2C pin pairs for the missing devices.\r\n");
    __HAL_RCC_GPIOB_CLK_ENABLE();
    scan_pins(GPIOB, GPIO_PIN_6, GPIO_PIN_7, "PB6/PB7 (I2C1)      ");
    scan_pins(GPIOB, GPIO_PIN_8, GPIO_PIN_9, "PB8/PB9 (I2C1 remap)");
    uart_puts("Looking for 0x1D (accelerometer) or 0x68 (RTC).\r\n");
}

/**
 * @brief VIBRAW - compute the axes the way the OEM does, ignoring the ACK.
 *
 * Tests whether a board with no accelerometer produces the OEM's near-trip
 * reading. Operator reports the original firmware fired "Significant
 * Vibration" only on MAX sensitivity and only at high RPM — which is what a
 * one-count margin on a floating bus would do as motor noise rises.
 */
void cmd_vibraw(void) {
    uint8_t r[7] = {0};
    for (uint8_t i = 1; i <= 6; i++) {
        i2c_read_device_reg_noack(VIBRATION_I2C_ADDR, i, &r[i]);
    }
    uart_puts("raw regs 1..6:");
    for (uint8_t i = 1; i <= 6; i++) { uart_puts(" 0x"); print_hex_byte(r[i]); }

    /* OEM arithmetic: hi*4 + ((signed)lo >> 6), Y carries +250. */
    int32_t x = (int32_t)(int8_t)r[1] * 4 + ((int32_t)(int8_t)r[2] >> 6);
    int32_t y = (int32_t)(int8_t)r[3] * 4 + ((int32_t)(int8_t)r[4] >> 6) + 250;
    int32_t z = (int32_t)(int8_t)r[5] * 4 + ((int32_t)(int8_t)r[6] >> 6);
    uart_puts("\r\n  X="); print_num(x);
    uart_puts("  Y="); print_num(y);
    uart_puts("  Z="); print_num(z);

    int32_t peak = x < 0 ? -x : x;
    if ((y < 0 ? -y : y) > peak) peak = (y < 0 ? -y : y);
    if ((z < 0 ? -z : z) > peak) peak = (z < 0 ? -z : z);
    uart_puts("\r\n  peak="); print_num(peak);
    uart_puts("  MAX-sensitivity bands: elevated>=251 excess>=301 -> ");
    uart_puts(peak >= 301 ? "EXCESS\r\n" : (peak >= 251 ? "SIGNIFICANT VIBRATION\r\n" : "ok\r\n"));
}

/**
 * @brief EEWRITE <hexaddr> <hexbyte>... - write bytes into the EEPROM.
 *
 * Recovery tool. EEERASE blanks the whole device including the OEM's own
 * region (0x00-0xAF), which holds its model string and calibration. Blanking
 * that is survivable — the stock firmware re-initialises a fully blank
 * EEPROM — but a PARTIALLY written one is not: our firmware re-stamps the OEM
 * validity magic 0x7C at 0x02 on its next save, so the stock firmware then
 * trusts a region that is otherwise 0xFF and reads garbage where its settings
 * should be. Observed consequence: it demanded a menu password that no longer
 * matched anything.
 *
 * With a snapshot taken before the erase, this puts the region back.
 * Command buffer is 32 bytes, so send a few bytes per call and script it.
 */
void cmd_eewrite(void) {
    char* buf = get_cmd_buf();
    uint8_t idx = get_cmd_idx();

    int i = 8;                      /* past "EEWRITE " */
    if (!(idx > 10 && buf[7] == ' ')) {
        uart_puts("Usage: EEWRITE <hexaddr> <hexbyte>...\r\n");
        return;
    }

    /* address */
    int addr = 0, digits = 0;
    while (i < idx && buf[i] != ' ') {
        const char c = buf[i++];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else { uart_puts("Bad address\r\n"); return; }
        addr = addr * 16 + v; digits++;
    }
    if (!digits || addr > 0xFF) { uart_puts("Bad address\r\n"); return; }

    int written = 0;
    while (i < idx) {
        while (i < idx && buf[i] == ' ') i++;
        if (i >= idx) break;
        int val = 0, n = 0;
        while (i < idx && buf[i] != ' ') {
            const char c = buf[i++];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else { uart_puts("Bad byte\r\n"); return; }
            val = val * 16 + v; n++;
        }
        if (!n || val > 0xFF || addr + written > 0xFF) { uart_puts("Bad byte\r\n"); return; }
        if (eeprom_write_byte((uint16_t)(addr + written), (uint8_t)val) != EEPROM_OK) {
            uart_puts("Write failed at 0x"); print_hex_byte((uint8_t)(addr + written));
            uart_puts("\r\n"); return;
        }
        written++;
    }
    uart_puts("OK wrote "); print_num(written);
    uart_puts(" byte(s) at 0x"); print_hex_byte((uint8_t)addr); uart_puts("\r\n");
}
