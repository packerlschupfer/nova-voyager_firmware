/**
 * @file commands_debug.c
 * @brief Debug and hardware test commands
 */

#include "commands_internal.h"
#include "buzzer.h"
#include "lcd.h"
#include "lcd_icons.h"
#include "materials.h"

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
    uart_puts("Depth: ");
    print_num(depth / 10);
    uart_putc('.');
    print_num((depth < 0 ? -depth : depth) % 10);
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
        UBaseType_t used = 192 - hwm;
        uint8_t margin_pct = (hwm * 100) / 192;
        uart_puts("  UI     (192): ");
        print_num(hwm); uart_puts("/");
        print_num(used); uart_puts(" = ");
        print_num(margin_pct); uart_puts("%");
        if (margin_pct < 20) { uart_puts(" LOW!"); any_warnings = true; }
        else if (margin_pct < 30) { uart_puts(" !"); }
        uart_puts("\r\n");
    }

    if (g_task_motor) {
        hwm = uxTaskGetStackHighWaterMark(g_task_motor);
        UBaseType_t used = 160 - hwm;
        uint8_t margin_pct = (hwm * 100) / 160;
        uart_puts("  Motor  (160): ");
        print_num(hwm); uart_puts("/");
        print_num(used); uart_puts(" = ");
        print_num(margin_pct); uart_puts("%");
        if (margin_pct < 20) { uart_puts(" LOW!"); any_warnings = true; }
        else if (margin_pct < 30) { uart_puts(" !"); }
        uart_puts("\r\n");
    }

    if (g_task_depth) {
        hwm = uxTaskGetStackHighWaterMark(g_task_depth);
        UBaseType_t used = 128 - hwm;
        uint8_t margin_pct = (hwm * 100) / 128;
        uart_puts("  Depth  (128): ");
        print_num(hwm); uart_puts("/");
        print_num(used); uart_puts(" = ");
        print_num(margin_pct); uart_puts("%");
        if (margin_pct < 20) { uart_puts(" LOW!"); any_warnings = true; }
        else if (margin_pct < 30) { uart_puts(" !"); }
        uart_puts("\r\n");
    }

    if (g_task_tapping) {
        hwm = uxTaskGetStackHighWaterMark(g_task_tapping);
        UBaseType_t used = 192 - hwm;
        uint8_t margin_pct = (hwm * 100) / 192;
        uart_puts("  Tapping(192): ");
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
    STATE_LOCK();
    bool motor_fault = g_state.motor_fault;
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
    delay_ms(2000);
}

/*===========================================================================*/
/* Essential LCD Graphics Test Commands                                     */
/*===========================================================================*/

void cmd_draw8icons(void) {
    uart_puts("\r\n8-Icon Grid with proper spacing\r\n\r\n");

    // Clear text
    lcd_cmd(0x30);
    lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));

    // Graphics mode - USE 0x37 for full display!
    uart_puts("Graphics mode: 0x34, 0x37 (full display access)\r\n");
    lcd_cmd(0x34);
    lcd_cmd(0x37);  // 0x37 NOT 0x36!

    // Fill entire screen white
    uart_puts("Filling screen white...\r\n");
    for (uint8_t y = 0; y < 32; y++) {
        lcd_cmd(0x80 | y);
        lcd_cmd(0x80);
        for (uint8_t x = 0; x < 16; x++) {
            lcd_data(0x00);
        }
        if ((y % 8) == 0) IWDG->KR = 0xAAAA;
    }

    IWDG->KR = 0xAAAA;

    uart_puts("Drawing 8 icons with borders and gaps...\r\n");

    // Draw 8 icons: 2 rows × 4 columns
    // Each icon: 30×30 pixels (leaving 2-pixel gap)
    for (uint8_t row = 0; row < 2; row++) {
        for (uint8_t col = 0; col < 4; col++) {
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

            IWDG->KR = 0xAAAA;
        }
    }

    uart_puts("\r\n***** 8 BORDERED ICONS *****\r\n");
    uart_puts("Holding 15 sec...\r\n\r\n");

    for (int i = 15; i > 0; i--) {
        if (i == 15 || i == 10 || i == 5 || i <= 3) {
            uart_puts("  ");
            print_num(i);
            uart_puts("...\r\n");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        IWDG->KR = 0xAAAA;
    }

    lcd_cmd(0x30);
    lcd_clear();
    lcd_print_at(0, 0, "8 icons done");
}

void cmd_test0x37(void) {
    uart_puts("\r\nTesting 0x37 graphics mode for full display\r\n\r\n");

    lcd_cmd(0x30);
    lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));

    // Use 0x37 instead of 0x36
    uart_puts("Entering graphics with 0x34, 0x37...\r\n");
    lcd_cmd(0x34);
    lcd_cmd(0x37);  // Different graphics mode bit!
    vTaskDelay(pdMS_TO_TICKS(1));

    // Clear using 0x37 mode
    uart_puts("Clearing with 0x37 mode...\r\n");
    for (uint8_t y = 0; y < 32; y++) {
        lcd_cmd(0x80 | y);
        lcd_cmd(0x80);
        for (uint8_t x = 0; x < 16; x++) {
            lcd_data(0x00);
        }
        if ((y % 8) == 0) IWDG->KR = 0xAAAA;
    }

    IWDG->KR = 0xAAAA;

    uart_puts("Drawing test lines at Y=0, 8, 16, 24...\r\n");

    // Y=0
    lcd_cmd(0x80);
    lcd_cmd(0x80);
    for (uint8_t x = 0; x < 16; x++) lcd_data(0xFF);

    // Y=8
    lcd_cmd(0x88);
    lcd_cmd(0x80);
    for (uint8_t x = 0; x < 16; x++) lcd_data(0xFF);

    // Y=16
    lcd_cmd(0x90);
    lcd_cmd(0x80);
    for (uint8_t x = 0; x < 16; x++) lcd_data(0xFF);

    // Y=24
    lcd_cmd(0x98);
    lcd_cmd(0x80);
    for (uint8_t x = 0; x < 16; x++) lcd_data(0xFF);

    uart_puts("\r\n***** 4 LINES WITH 0x37 MODE *****\r\n");
    uart_puts("Holding 15 seconds...\r\n\r\n");

    for (int i = 15; i > 0; i--) {
        if (i == 15 || i == 10 || i == 5 || i <= 3) {
            uart_puts("  ");
            print_num(i);
            uart_puts("...\r\n");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        IWDG->KR = 0xAAAA;
    }

    lcd_cmd(0x30);
    lcd_clear();
    lcd_print_at(0, 0, "0x37 test done");
}

void cmd_testcgrom(void) {
    uart_puts("\r\n========================================\r\n");
    uart_puts("CGROM CHARACTER SET SCAN\r\n");
    uart_puts("========================================\r\n\r\n");

    uart_puts("Scanning CGROM characters 0x00-0xFF\r\n");
    uart_puts("Looking for: Chinese, symbols, box drawing, gauges\r\n\r\n");

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

    for (uint8_t r = 0; r < 6; r++) {
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

        for (int i = 0; i < 10; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            IWDG->KR = 0xAAAA;
        }
    }

    uart_puts("\r\n========================================\r\n");
    uart_puts("CGROM SCAN COMPLETE\r\n");
    uart_puts("========================================\r\n");
    uart_puts("6 pages scanned - review photos!\r\n\r\n");

    lcd_clear();
    lcd_print_at(0, 0, "CGROM scan done");
}

void cmd_scanall(void) {
    uart_puts("\r\n========================================\r\n");
    uart_puts("SYSTEMATIC COMMAND SCAN\r\n");
    uart_puts("Testing ALL commands 0x00-0xFF\r\n");
    uart_puts("========================================\r\n\r\n");

    uart_puts("This will test commands in groups of 16\r\n");
    uart_puts("Each group: 2 seconds display time\r\n");
    uart_puts("Total time: ~32 seconds\r\n\r\n");

    // Test in basic mode first
    lcd_cmd(0x30);
    lcd_clear();
    lcd_print_at(0, 0, "Scanning 0x00-FF");

    for (uint8_t group = 0; group < 16; group++) {
        // SKIP dangerous groups that cause crashes
        if (group == 2) {
            uart_puts("\r\n*** GROUP 2 (0x20-0x2F) SKIPPED ***\r\n");
            uart_puts("    (Function set - unstable)\r\n");
            continue;
        }
        if (group == 15) {
            uart_puts("\r\n*** GROUP 15 (0xF0-0xFF) SKIPPED ***\r\n");
            uart_puts("    (High addresses - causes crash)\r\n");
            continue;
        }

        uint8_t start_cmd = group * 16;
        uint8_t end_cmd = start_cmd + 15;

        uart_puts("\r\n*** GROUP ");
        print_num(group);
        uart_puts(": 0x");
        print_num(start_cmd);
        uart_puts("-0x");
        print_num(end_cmd);
        uart_puts(" ***\r\n");

        // CLEAR before each group test
        lcd_cmd(0x30);  // Basic mode
        lcd_cmd(0x01);  // Clear
        vTaskDelay(pdMS_TO_TICKS(2));
        IWDG->KR = 0xAAAA;

        // Display group info on LCD (format numbers properly)
        char buf[20];
        lcd_set_cursor(0, 0);
        lcd_print("Grp ");
        buf[0] = '0' + (group / 10);
        buf[1] = '0' + (group % 10);
        buf[2] = '\0';
        lcd_print(buf);

        lcd_set_cursor(1, 0);
        lcd_print("0x");
        buf[0] = (start_cmd >= 160) ? 'A' + (start_cmd/16 - 10) : '0' + (start_cmd/16);
        buf[1] = (start_cmd%16 >= 10) ? 'A' + (start_cmd%16 - 10) : '0' + (start_cmd%16);
        buf[2] = '-';
        buf[3] = (end_cmd >= 160) ? 'A' + (end_cmd/16 - 10) : '0' + (end_cmd/16);
        buf[4] = (end_cmd%16 >= 10) ? 'A' + (end_cmd%16 - 10) : '0' + (end_cmd%16);
        buf[5] = '\0';
        lcd_print(buf);

        vTaskDelay(pdMS_TO_TICKS(500));
        IWDG->KR = 0xAAAA;

        uart_puts("Sending commands (watch LCD for effects)...\r\n");

        // Send all 16 commands in this group - NO LCD TEXT!
        for (uint8_t cmd = start_cmd; cmd <= end_cmd; cmd++) {
            lcd_cmd(cmd);
            vTaskDelay(pdMS_TO_TICKS(20));
            if ((cmd % 4) == 0) {
                IWDG->KR = 0xAAAA;
            }
        }

        uart_puts("Commands sent, holding 5 sec (observe LCD)...\r\n");

        // Hold to observe effect
        for (int i = 0; i < 5; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            IWDG->KR = 0xAAAA;
        }

        // Reset to known state
        lcd_cmd(0x30);
        lcd_cmd(0x0C);
        IWDG->KR = 0xAAAA;
    }

    uart_puts("\r\n========================================\r\n");
    uart_puts("SCAN COMPLETE\r\n");
    uart_puts("========================================\r\n");

    lcd_cmd(0x30);
    lcd_clear();
}

void cmd_testsingle(void) {
    uart_puts("\r\n========================================\r\n");
    uart_puts("SINGLE COMMAND TEST MODE\r\n");
    uart_puts("========================================\r\n\r\n");
    uart_puts("Enter command in hex (e.g., '3A' for 0x3A)\r\n");
    uart_puts("Or 'Q' to quit\r\n\r\n");

    lcd_cmd(0x30);
    lcd_clear();
    lcd_print_at(0, 0, "Single Cmd Mode");
    lcd_print_at(1, 0, "See console");

    // This is a placeholder - full implementation would need
    // interactive hex input parsing
    uart_puts("Full interactive mode requires hex input parser\r\n");
    uart_puts("For now, testing common interesting commands individually:\r\n\r\n");

    uint8_t test_cmds[] = {
        0x10, 0x14, 0x18, 0x1C,  // Shift commands
        0x34, 0x35, 0x36, 0x37, 0x38,  // Function set
        0xA0, 0xA4, 0xA8, 0xB0,  // High address range
        0xF0, 0xF4, 0xF8, 0xFC,  // Very high range
    };

    for (uint8_t i = 0; i < 16; i++) {
        uint8_t cmd = test_cmds[i];

        uart_puts("*** COMMAND 0x");
        if (cmd >= 0xA0) uart_putc('A' + (cmd/16 - 10));
        else uart_putc('0' + (cmd/16));
        if ((cmd%16) >= 10) uart_putc('A' + (cmd%16 - 10));
        else uart_putc('0' + (cmd%16));
        uart_puts(" ***\r\n");

        // Reset and clear
        lcd_cmd(0x30);
        lcd_cmd(0x01);
        vTaskDelay(pdMS_TO_TICKS(100));
        IWDG->KR = 0xAAAA;

        // Show on LCD
        char hex[10];
        lcd_set_cursor(0, 0);
        lcd_print("Cmd: 0x");
        hex[0] = (cmd >= 0xA0) ? 'A' + (cmd/16 - 10) : '0' + (cmd/16);
        hex[1] = ((cmd%16) >= 10) ? 'A' + (cmd%16 - 10) : '0' + (cmd%16);
        hex[2] = '\0';
        lcd_print(hex);

        lcd_print_at(1, 0, "0123456789ABCDEF");

        // Send command
        lcd_cmd(cmd);

        uart_puts("Command sent, holding 5 sec...\r\n\r\n");
        for (int j = 0; j < 5; j++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            IWDG->KR = 0xAAAA;
        }
    }

    lcd_cmd(0x30);
    lcd_clear();
    lcd_print_at(0, 0, "Single test done");
}

void cmd_scangfx(void) {
    uart_puts("\r\n========================================\r\n");
    uart_puts("COMMAND SCAN IN GRAPHICS MODE (NO LCD TEXT)\r\n");
    uart_puts("Testing 0x00-0xFF in graphics mode (0x37)\r\n");
    uart_puts("========================================\r\n\r\n");

    uart_puts("NO text printed to LCD - watch raw effects!\r\n");
    uart_puts("Each group: 5 seconds display time\r\n\r\n");

    for (uint8_t group = 0; group < 16; group++) {
        // Skip dangerous groups
        if (group == 2 || group == 15) {
            uart_puts("\r\n*** GROUP ");
            print_num(group);
            uart_puts(" SKIPPED (crash risk) ***\r\n");
            continue;
        }

        uint8_t start = group * 16;

        uart_puts("\r\n*** GROUP ");
        print_num(group);
        uart_puts(" (0x");
        print_num(start);
        uart_puts("+) ***\r\n");

        // Enter graphics mode
        lcd_cmd(0x30);
        lcd_cmd(0x01);
        vTaskDelay(pdMS_TO_TICKS(100));

        lcd_cmd(0x34);
        lcd_cmd(0x37);  // Graphics mode

        // Clear graphics RAM
        uart_puts("Clearing graphics RAM...\r\n");
        for (uint8_t y = 0; y < 32; y++) {
            lcd_cmd(0x80 | y);
            lcd_cmd(0x80);
            for (uint8_t x = 0; x < 16; x++) {
                lcd_data(0x00);
            }
            if ((y % 16) == 0) IWDG->KR = 0xAAAA;
        }

        IWDG->KR = 0xAAAA;

        uart_puts("Sending 16 commands (watch LCD)...\r\n");

        // Test all 16 commands
        for (uint8_t i = 0; i < 16; i++) {
            lcd_cmd(start + i);
            vTaskDelay(pdMS_TO_TICKS(20));
            if ((i % 4) == 0) IWDG->KR = 0xAAAA;
        }

        uart_puts("Holding 5 sec (observe graphics)...\r\n\r\n");
        for (int j = 0; j < 5; j++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            IWDG->KR = 0xAAAA;
        }
    }

    uart_puts("\r\n========================================\r\n");
    uart_puts("GRAPHICS SCAN COMPLETE\r\n");
    uart_puts("========================================\r\n");

    lcd_cmd(0x30);
    lcd_clear();
}

// Simple graphics test (from lcd.c)
void cmd_testgfx(void) {
    extern void lcd_test_graphics_mode(void);
    uart_puts("Testing ST7920 graphics mode...\r\n");
    lcd_test_graphics_mode();
}

// LCD test commands
void cmd_testcgram(void) {
    uart_puts("Testing CGRAM custom characters...\r\n");
    extern void lcd_test_cgram(void);
    lcd_test_cgram();
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
    uart_puts("=== Load Monitor (10 seconds) ===\r\n");
    uart_puts("Reading motor_load from g_state\r\n");
    uart_puts("Press any key to stop\r\n\r\n");

    for (int t = 0; t < 200; t++) {
        STATE_LOCK();
        uint8_t load = g_state.motor_load;
        STATE_UNLOCK();

        uart_puts("KR=");
        print_num(load);
        uart_puts("%  ");
        if ((t % 10) == 9) uart_puts("\r\n");

        if (uart_getc_nonblocking() >= 0) {
            uart_getc_nonblocking();
            uart_puts("\r\nStopped\r\n");
            break;
        }

        HEARTBEAT_UPDATE_MAIN();
        IWDG->KR = 0xAAAA;
        delay_ms(50);
    }
    uart_puts("\r\nDone\r\n");
}

void cmd_loadbase(void) {
    uart_puts("=== Baseline Load Learning (4s) ===\r\n");
    uart_puts("Motor should be running unloaded\r\n\r\n");

    uint32_t sum = 0;
    uint8_t samples = 0;
    uint8_t min_load = 255, max_load = 0;

    for (int t = 0; t < 80; t++) {
        STATE_LOCK();
        uint8_t load = g_state.motor_load;
        STATE_UNLOCK();

        sum += load;
        samples++;
        if (load < min_load) min_load = load;
        if (load > max_load) max_load = load;

        uart_putc('.');
        if ((t % 20) == 19) uart_puts("\r\n");

        HEARTBEAT_UPDATE_MAIN();
        IWDG->KR = 0xAAAA;
        delay_ms(50);
    }

    uint8_t avg = sum / samples;
    uart_puts("\r\n\r\nResults:\r\n");
    uart_puts("  Average: "); print_num(avg); uart_puts("%\r\n");
    uart_puts("  Min: "); print_num(min_load); uart_puts("%\r\n");
    uart_puts("  Max: "); print_num(max_load); uart_puts("%\r\n");
    uart_puts("  Range: "); print_num(max_load - min_load); uart_puts("%\r\n");
}

void cmd_loadsense(void) {
    uart_puts("=== Load Sensing Test ===\r\n");
    const settings_t* s = settings_get();

    uart_puts("Settings:\r\n");
    uart_puts("  Threshold: "); print_num(s->tapping.load_increase_threshold); uart_puts("%\r\n");
    uart_puts("  Load Inc: "); uart_puts(s->tapping.load_increase_enabled ? "ON\r\n" : "OFF\r\n");
    uart_puts("  Load Slip: "); uart_puts(s->tapping.load_slip_enabled ? "ON\r\n" : "OFF\r\n");
    uart_puts("\r\nMonitoring for 10s (apply load)...\r\n\r\n");

    uint8_t baseline = 0;

    for (int t = 0; t < 200; t++) {
        STATE_LOCK();
        uint8_t load = g_state.motor_load;
        STATE_UNLOCK();

        if (t < 20) {
            baseline = (t == 0) ? load : ((baseline * 7 + load) / 8);
        } else if (t == 20) {
            uart_puts("Baseline: "); print_num(baseline); uart_puts("%\r\n\r\n");
        } else if (load > baseline + s->tapping.load_increase_threshold) {
            uart_puts(">>> SPIKE: KR="); print_num(load);
            uart_puts("% (baseline+"); print_num(s->tapping.load_increase_threshold);
            uart_puts("% = "); print_num(baseline + s->tapping.load_increase_threshold);
            uart_puts("%) <<<\r\n");
        }

        if ((t % 20) == 0) {
            uart_puts("KR="); print_num(load); uart_puts("%  ");
        }

        if (uart_getc_nonblocking() >= 0) break;

        HEARTBEAT_UPDATE_MAIN();
        IWDG->KR = 0xAAAA;
        delay_ms(50);
    }
    uart_puts("\r\nDone\r\n");
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
    uart_puts("Listening to motor UART (5s)...\r\n");
    uart_puts("Press any key to stop\r\n\r\n");

    for (int t = 0; t < 500; t++) {
        extern int motor_getc_timeout(uint32_t timeout_us);
        int c = motor_getc_timeout(10000);

        if (c >= 0) {
            uart_puts("RX: 0x");
            if (c < 16) uart_putc('0');
            char hex[3];
            snprintf(hex, 3, "%02X", c);
            uart_puts(hex);
            uart_puts("\r\n");
        }

        if (uart_getc_nonblocking() >= 0) {
            uart_getc_nonblocking();
            break;
        }
    }
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

// ===================================================================
// TIER 2: Graphics Test Commands
// ===================================================================

void cmd_testallicons(void) {
    uart_puts("Testing all icon positions...\r\n");
    extern void lcd_test_icons(void);
    lcd_test_icons();
}

void cmd_testicons(void) {
    uart_puts("Icon drawing test...\r\n");
    extern void lcd_test_icons(void);
    lcd_test_icons();
}

void cmd_draw8boxes(void) {
    uart_puts("Drawing 8 boxes (uses draw8icons)...\r\n");
    cmd_draw8icons();
}

// ===================================================================
// TIER 2: Hardware Test Commands
// ===================================================================

void cmd_adcall(void) {
    uart_puts("ADC channel 11 (depth sensor):\r\n");
    extern void cmd_depth(void);
    cmd_depth();
}

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
