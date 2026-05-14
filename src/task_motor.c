/**
 * @file task_motor.c
 * @brief Motor Task - FreeRTOS Coordination and Control Sequences
 *
 * MODULE: Motor Coordination Task
 * LAYER: Application (FreeRTOS task level)
 * THREAD SAFETY: Single task instance, coordinates with other tasks via queues
 *
 * Responsibilities:
 * - Motor command queue processing
 * - Periodic status polling (adaptive: 2Hz idle, 20Hz running)
 * - Motor control sequences (start/stop with safety checks)
 * - MCB initialization and boot handshake
 * - Safety monitoring (jam, temperature, communication failures)
 * - Spindle hold maintenance
 *
 * Dependencies:
 * - motor_uart.c: UART hardware layer
 * - motor_protocol.c: Protocol building/parsing
 * - jam.c, temperature.c: Safety monitoring modules
 *
 * Task Priority: 4 (highest - motor control is time-critical)
 * Stack: 192 bytes (verified via stack analysis)
 * Poll Rate: 2Hz idle, 20Hz running
 */

#include "shared.h"
#include "settings.h"
#include "motor.h"
#include "buzzer.h"
#include "spindle_hold.h"
#include "temperature.h"
#include "jam.h"
#include "utilities.h"
#include "motor_protocol.h"
#include "motor_uart.h"
#include "diagnostics.h"
#include "stm32f1xx_hal.h"
#include <string.h>
#include <stdio.h>

// External UART helper functions (from serial_console.c)
extern void uart_puts(const char* s);
extern void uart_putc(char c);
extern void print_num(int32_t n);

// UART layer now in motor_uart.c

/*===========================================================================*/
/* MCB Scan Mode                                                              */
/*===========================================================================*/

// When true, motor task skips all MCB communication (for MCBSCAN command)

// Pauses status polling during MCBSCAN command to prevent UART conflicts
// Uses volatile for atomic bool read/write (sufficient on ARM Cortex-M4)
volatile bool motor_scan_mode = false;

/*===========================================================================*/
/* Protocol Constants                                                         */
/*===========================================================================*/

// Query format: [0x04][addr][0x31][CMD_H][CMD_L][0x05]
// Command format: [0x04][addr][0x02][0x31][CMD_H][CMD_L][param...][0x03][XOR]


// Use CMD_STOP, CMD_JOG, CMD_START, CMD_SET_SPEED, CMD_GET_FLAGS, etc. from config.h
#define CMD_FORWARD     CMD_JOG  // Alias: JF with param 0x6AA
#define CMD_REVERSE     CMD_JOG  // Alias: JF with param 0x6AB
#define CMD_GET_CV      CMD_CURRENT_VELOCITY  // Alias for clarity

#define PARAM_FORWARD   0x06AA
#define PARAM_REVERSE   0x06AB

/*===========================================================================*/
/* Jam Detection */
/*===========================================================================*/

// Jam detection now handled by jam.c module
// Constants: JAM_TIMEOUT_MS, JAM_LOAD_THRESHOLD moved to jam.c

/*===========================================================================*/
/* Temperature Monitoring */
/*===========================================================================*/

// Temperature monitoring now handled by temperature.c module
// Constants: TEMP_WARNING_DEFAULT, TEMP_HYSTERESIS moved to temperature.c
#define TEMP_SHUTDOWN_DEFAULT   80  // Default shutdown threshold (°C)

/*===========================================================================*/
/* Communication Timeout Constants (H5 safety fix)                            */
/*===========================================================================*/

#define MAX_COMM_FAILURES       5   // 5 consecutive failures triggers fault
// Note: MOTOR_RESPONSE_TIMEOUT_MS defined in config.h (100ms)

/*===========================================================================*/
/* Private Variables                                                          */
/*===========================================================================*/

// Task-local state
static uint8_t rx_buffer[MOTOR_UART_BUFFER_SIZE];
static uint8_t rx_index = 0;
static uint16_t target_speed_local = 0;
static bool motor_enabled = false;
static bool direction_forward = true;

// Communication failure tracking
static uint8_t consecutive_comm_failures = 0;

// Temperature monitoring state
// Variables: current_temp, temp_warning_active moved to temperature.c module

// [TASK_LOCAL] Only accessed within motor task
// Voltage monitoring state
static bool voltage_warning_active = false;

// [TASK_LOCAL] Only accessed within motor task
// MCB firmware version (queried via GV command at boot)
static char mcb_version[16] = "unknown";

// UART hardware layer moved to motor_uart.c
// Use motor_uart_*() functions for all UART operations

/*===========================================================================*/
/* External Debug Functions                                                   */
/*===========================================================================*/

extern void uart_puts(const char* s);
extern void uart_putc(char c);

/*===========================================================================*/
/* Protocol Functions                                                         */
/*===========================================================================*/

// Helper function for hex output (forward declaration)
static char hex_digit(uint8_t n);



static bool send_query(uint16_t cmd) {
    motor_uart_flush_rx();

    // Build query packet using protocol layer
    uint8_t packet[PROTO_MAX_PACKET_SIZE];
    size_t len = protocol_build_query(cmd, packet);

    // Send packet with timeout handling
    for (size_t i = 0; i < len; i++) {
        if (!motor_uart_send_byte(packet[i])) {
            DEBUG_PRINT("[QUERY] TX timeout, cmd=0x");
            DEBUG_PRINTC(hex_digit((cmd >> 12) & 0xF));
            DEBUG_PRINTC(hex_digit((cmd >> 8) & 0xF));
            DEBUG_PRINTC(hex_digit((cmd >> 4) & 0xF));
            DEBUG_PRINTC(hex_digit(cmd & 0xF));
            uart_puts("\r\n");
            diagnostics_protocol_error(2);
            return false;
        }
    }

    diagnostics_protocol_sent(true);
    return true;  // Success
}

// Simple hex digit conversion (no snprintf)
static char hex_digit(uint8_t n) {
    return (n < 10) ? ('0' + n) : ('A' + n - 10);
}


// Protocol building now handled by motor_protocol module
// This function focuses on UART transmission with timeout handling
static bool send_command(uint16_t cmd, uint16_t param) {
    motor_uart_flush_rx();

    // Build command packet using protocol layer
    uint8_t packet[PROTO_MAX_PACKET_SIZE];
    size_t len = protocol_build_command(cmd, param, packet);

    // Debug: print packet bytes (disabled for cleaner output)
    #if 0
    uart_puts("TX:");
    for (int i = 0; i < len; i++) {
        uart_puts(" ");
        uint8_t b = packet[i];
        char hi = (b >> 4) < 10 ? '0' + (b >> 4) : 'A' + (b >> 4) - 10;
        char lo = (b & 0xF) < 10 ? '0' + (b & 0xF) : 'A' + (b & 0xF) - 10;
        uart_putc(hi);
        uart_putc(lo);
    }
    uart_puts("\r\n");
    #endif

    // Send packet with timeout handling
    for (int i = 0; i < len; i++) {
        if (!motor_uart_send_byte(packet[i])) {
            DEBUG_PRINT("[CMD] TX timeout, cmd=0x");
            DEBUG_PRINTC(hex_digit((cmd >> 12) & 0xF));
            DEBUG_PRINTC(hex_digit((cmd >> 8) & 0xF));
            DEBUG_PRINTC(hex_digit((cmd >> 4) & 0xF));
            DEBUG_PRINTC(hex_digit(cmd & 0xF));
            uart_puts(" param=");
            print_num(param);
            uart_puts("\r\n");
            diagnostics_protocol_error(2);
            return false;  // TX timeout
        }
    }

    diagnostics_protocol_sent(false);
    return true;  // Success
}

// Response parsing helpers moved to motor_protocol.c
// Use protocol_find_stx() and protocol_parse_and_validate()

/**
 * @brief Update CV (current velocity) state from parsed RPM value
 * @param rpm Validated RPM value (0 to SPEED_MAX_RPM)
 *
 * Updates both motor_set_actual_rpm() and g_state.current_rpm
 * Safety: STATE_LOCK/UNLOCK handled internally
 */
static void update_cv_state(uint16_t rpm) {
    motor_set_actual_rpm(rpm);
    STATE_LOCK();
    g_state.current_rpm = rpm;
    STATE_UNLOCK();
}

/**
 * @brief Update KR (motor load) state from parsed load percentage
 * @param load Validated load percentage (0 to 100)
 *
 * Updates g_state.motor_load
 * Safety: STATE_LOCK/UNLOCK handled internally
 */
static void update_kr_state(uint8_t load) {
    STATE_LOCK();
    g_state.motor_load = load;
    STATE_UNLOCK();
}

/**
 * @brief Update SV (target speed) state from parsed RPM value
 * @param rpm Validated RPM value (1 to SPEED_MAX_RPM)
 *
 * Updates g_state.target_rpm
 * Safety: STATE_LOCK/UNLOCK handled internally
 */
static void update_sv_state(uint16_t rpm) {
    STATE_LOCK();
    g_state.target_rpm = rpm;
    STATE_UNLOCK();
}

static bool wait_response(uint32_t timeout_ms) {
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    rx_index = 0;
    memset(rx_buffer, 0, sizeof(rx_buffer));
    bool found_etx = false;

    while ((xTaskGetTickCount() - start) < timeout) {
        if (motor_uart_rx_available()) {
            uint8_t b = motor_uart_read_byte();
            if (rx_index < sizeof(rx_buffer) - 1) {
                rx_buffer[rx_index++] = b;
            }
            // Check for end of response (ETX = 0x03)
            if (b == 0x03) {
                found_etx = true;

                // Check if this is a CV or KR response before returning
                // CV format: STX + unit + 'C' + 'V' + RPM + ETX
                // KR format: STX + unit + 'K' + 'R' + LOAD% + ETX
                // Need at least STX + unit + cmd_H + cmd_L + ETX = 5 bytes.
                // Use i + 5 <= rx_index to avoid size_t underflow when rx_index < 5.
                for (size_t i = 0; i + 5 <= rx_index; i++) {
                    if (rx_buffer[i] == 0x02) {  // STX

                        // Check for CV response (Current Velocity)
                        if (rx_buffer[i + 2] == 'C' && rx_buffer[i + 3] == 'V') {
                            int16_t rpm;
                            if (protocol_parse_and_validate(rx_buffer, i, rx_index, 0, SPEED_MAX_RPM, &rpm)) {
                                update_cv_state((uint16_t)rpm);
                            }

                            // Reset buffer and keep waiting
                            rx_index = 0;
                            memset(rx_buffer, 0, sizeof(rx_buffer));
                            found_etx = false;
                            break;
                        }

                        // Check for KR response (motor load percentage)
                        if (rx_buffer[i + 2] == 'K' && rx_buffer[i + 3] == 'R') {
                            int16_t load;
                            if (protocol_parse_and_validate(rx_buffer, i, rx_index, 0, 100, &load)) {
                                update_kr_state((uint8_t)load);
                            }

                            // Reset buffer and keep waiting
                            rx_index = 0;
                            memset(rx_buffer, 0, sizeof(rx_buffer));
                            found_etx = false;
                            break;
                        }
                    }
                }

                // If still found_etx (wasn't a CV response), return success
                if (found_etx) {
                    return true;
                }
            }
        }
        vTaskDelay(1);
    }

    return rx_index > 0;  // Got some data (timeout)
}

/*===========================================================================*/
/* Motor Control Functions                                                    */
/*===========================================================================*/

/**
 * @brief Post-stop re-sync sequence
 *
 * After motor stops, original firmware re-synchronizes HMI↔MCB state:
 * RS×2 → JF=1706 → SV query/confirm → JF=1706 → S2? → CL?
 *
 * This catches any state changes (voltage sag, thermal protection, parameter drift)
 * and ensures consistent state before next start.
 */
static void local_motor_post_stop_sync(void) {
    // Best-effort sync - log timeouts but continue

    // Double stop (matches original firmware pattern)
    send_command(CMD_STOP, 0);  // Ignore return - best effort
    delay_ms(5);
    send_command(CMD_STOP, 0);
    delay_ms(5);

    // Reset to forward direction
    send_command(CMD_FORWARD, PARAM_FORWARD);
    delay_ms(5);

    // Query and confirm current speed setting
    motor_uart_flush_rx();
    if (!send_query(CMD_SET_SPEED)) {
        uart_puts("[SYNC] SV query timeout\r\n");
    }
    if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
        // Parse SV response to get MCB's current speed setting
        size_t offset = 0;
        for (size_t i = 0; i < rx_index && i < 3; i++) {
            if (rx_buffer[i] == 0x02) { offset = i; break; }
        }
        if (rx_index >= offset + 5 && rx_buffer[offset] == 0x02) {
            size_t data_start = offset + 4;
            int16_t mcb_speed = protocol_parse_field(rx_buffer, data_start, rx_index - data_start - 1);
            if (mcb_speed > 0 && mcb_speed <= SPEED_MAX_RPM) {
                // Confirm speed setting back to MCB
                send_command(CMD_SET_SPEED, mcb_speed);  // Ignore return
                delay_ms(5);
                target_speed_local = mcb_speed;  // Update local tracking
            }
        }
    }

    // Set forward again (original firmware does this twice)
    send_command(CMD_FORWARD, PARAM_FORWARD);  // Ignore return
    delay_ms(5);

    // Verify S2 unchanged
    motor_uart_flush_rx();
    send_query(CMD_SPEED_2);
    if (!wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
        uart_puts("[MOTOR] S2 verify timeout after stop\r\n");
    }

    // Verify CL unchanged
    motor_uart_flush_rx();
    send_query(CMD_CURRENT_LIMIT);
    if (!wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
        uart_puts("[MOTOR] CL verify timeout after stop\r\n");
    }
}

// Fast stop for tapping - RS=0 twice, then reset direction to forward
static void local_motor_stop_fast(void) {
    send_command(CMD_STOP, 0);
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);
    send_command(CMD_STOP, 0);
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);

    // Reset direction to forward - ensures known state for next direction change
    // MCB sometimes ignores JF if direction state is ambiguous after rapid cycling
    send_command(CMD_FORWARD, PARAM_FORWARD);
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);

    motor_set_actual_rpm(0);
    motor_enabled = false;
    direction_forward = true;
    STATE_LOCK();
    g_state.motor_running = false;
    g_state.motor_forward = true;
    STATE_UNLOCK();
}

static void local_motor_stop(void) {

    if (!send_command(CMD_STOP, 0)) {
        uart_puts("[STOP] UART timeout - using hardware disable\r\n");
        motor_hardware_disable();  // Hardware failsafe if UART fails
    }
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);

    // Save current speed to S2 (MCB's fallback speed for resets)
    if (target_speed_local >= SPEED_MIN_RPM) {
        if (!send_command(CMD_SPEED_2, target_speed_local)) {
            uart_puts("[STOP] S2 command timeout\r\n");
        }
        delay_ms(5);
    }

    motor_set_actual_rpm(0);
    motor_enabled = false;

    STATE_LOCK();
    g_state.motor_running = false;
    STATE_UNLOCK();

    // Post-stop re-sync only for normal stops (not during rapid tapping)
    local_motor_post_stop_sync();
}

/*===========================================================================*/
/* Spindle Hold */
/*===========================================================================*/

// Spindle hold implementation moved to spindle_hold.c module
// Public API: spindle_hold_start(), spindle_hold_release(), spindle_hold_update()

// Legacy compatibility wrapper
bool motor_is_spindle_hold_active(void) {
    return spindle_hold_is_active();
}

static void local_motor_set_direction(bool forward) {
    direction_forward = forward;

    if (!send_command(CMD_FORWARD, forward ? PARAM_FORWARD : PARAM_REVERSE)) {
        uart_puts("[DIR] JF command timeout\r\n");
    }
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);

    STATE_LOCK();
    g_state.motor_forward = forward;
    STATE_UNLOCK();
}

static void local_motor_start(void) {
    motor_hardware_enable();  // Enable hardware BEFORE sending command


    // Set CL=100% for full power when running
    // Original firmware: CL=70% idle → CL=100% running
    if (!send_command(CMD_CURRENT_LIMIT, CL_RUNNING_PERCENT)) {
        uart_puts("[START] CL command timeout\r\n");
    }
    delay_ms(20);  // Let CL command complete

    // Send ST (start) command - critical for motor start
    if (!send_command(CMD_START, 0)) {
        uart_puts("[START] ST command timeout - start may fail\r\n");
    }
    delay_ms(20);  // Let ST command complete

    // Send single SV (speed) command - MCB handles ramping
    if (!send_command(CMD_SET_SPEED, target_speed_local)) {
        uart_puts("[START] SV command timeout\r\n");
    }
    delay_ms(20);  // Let SV command complete

    // Flush any responses
    motor_uart_flush_rx();

    motor_enabled = true;

    STATE_LOCK();
    g_state.motor_running = true;
    STATE_UNLOCK();
}

static void local_motor_set_speed(uint16_t rpm) {
    // Clamp to valid range
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > SPEED_MAX_RPM) rpm = SPEED_MAX_RPM;

    // Send RPM directly as decimal ASCII (matches original firmware)
    target_speed_local = rpm;
    if (!send_command(CMD_SET_SPEED, rpm)) {
        uart_puts("[SPEED] SV command timeout\r\n");
    }
    // Quick RX flush to clear response without blocking long
    delay_ms(10);
    motor_uart_flush_rx();

    // Update S2 (MCB's stored fallback speed) so it remembers this speed across resets
    send_command(CMD_SPEED_2, rpm);  // Ignore return - non-critical
    delay_ms(5);

    // Don't update current_rpm here - let motor_query_status() read actual RPM from motor
    // This ensures display shows actual motor speed, not commanded speed
}



static void motor_query_status(void) {
    // Query flags first
    send_query(CMD_GET_FLAGS);
    if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
        // Debug: print raw response (disabled)
        #if 0
        uart_puts("RX:");
        for (size_t i = 0; i < rx_index && i < 32; i++) {
            uart_puts(" ");
            uint8_t b = rx_buffer[i];
            char hi = (b >> 4) < 10 ? '0' + (b >> 4) : 'A' + (b >> 4) - 10;
            char lo = (b & 0xF) < 10 ? '0' + (b & 0xF) : 'A' + (b & 0xF) - 10;
            uart_putc(hi);
            uart_putc(lo);
        }
        uart_puts("\r\n");
        #endif

        // Parse response - handle ACK byte first
        // Response format: [ACK][STX][unit][cmd_echo][data][ETX][checksum]
        size_t offset = 0;
        if (rx_index > 0 && rx_buffer[0] == 0x06) {
            offset = 1;  // Skip ACK byte
        }

        if (rx_index >= offset + 3 && rx_buffer[offset] == 0x02) {
            // Valid response received - reset failure counter (H5)
            consecutive_comm_failures = 0;

            // Parse response data
            // Format: [STX][unit][cmd_H][cmd_L][data...][ETX]
            // Data starts at offset+4 (after STX, unit, cmd high, cmd low)
            // Code polish: Removed unused 'speed' and 'vib' variables (compiler warnings)
            uint16_t flags = 0, load = 0, temp = 0;
            size_t field = 0;  // Declare outside to check if parsed

            // For GF response, data is ASCII number (flags)
            // Check if response has command echo (GF, SV, etc.)
            if (rx_index >= offset + 5) {
                size_t data_start = offset + 4;  // After STX, unit, cmd_H, cmd_L

                // Parse ASCII data field until ETX
                size_t field_start = data_start;
                for (size_t i = data_start; i < rx_index; i++) {
                    if (rx_buffer[i] == ',' || rx_buffer[i] == 0x03) {
                        int16_t val = protocol_parse_field(rx_buffer, field_start, i - field_start);
                        switch (field) {
                            case 0: flags = (uint16_t)val; break;
                            case 1: /* speed - unused, MCB doesn't send */ break;
                            case 2: load = (val >= 0 && val <= 100) ? val : 0; break;
                            case 3: /* vibration - unused */ break;
                            case 4: temp = (val > 0) ? val : 0; break;
                        }
                        field++;
                        field_start = i + 1;
                        if (rx_buffer[i] == 0x03) break;
                    }
                }
            }

            // If no comma-separated fields, just use flags value
            // (GF returns single ASCII number)

            // Debug: print parsed values (disabled - reduce serial noise)
            #if 0
            uart_puts("GF: flags=");
            char buf[8];
            int i = 0;
            uint16_t val = flags;
            do { buf[i++] = '0' + (val % 10); val /= 10; } while (val && i < 7);
            while (i > 0) uart_putc(buf[--i]);
            uart_puts(" speed=");
            i = 0; val = speed;
            do { buf[i++] = '0' + (val % 10); val /= 10; } while (val && i < 7);
            while (i > 0) uart_putc(buf[--i]);
            uart_puts(" load=");
            i = 0; val = load;
            do { buf[i++] = '0' + (val % 10); val /= 10; } while (val && i < 7);
            while (i > 0) uart_putc(buf[--i]);
            uart_puts("\r\n");
            #endif



            // Detect motor direction and running state from GF flags
            //
            // GF=32: STOPPED forward, GF=34: RUNNING forward
            // GF=436: STOPPED reverse, GF=438: RUNNING reverse
            // Error states have bit 14 set (0x4000), e.g., GF=16929+
            bool known_good_state = (flags == GF_MOTOR_STOPPED || flags == GF_MOTOR_RUNNING ||
                                     flags == GF_MOTOR_STOPPED_REV || flags == GF_MOTOR_RUNNING_REV);
            bool error_state = (flags & 0x4000) != 0;  // Bit 14 = error indicator

            STATE_LOCK();
            g_state.motor_fault = error_state;

            if (flags == GF_MOTOR_STOPPED || flags == GF_MOTOR_RUNNING) {
                g_state.motor_forward = true;
            } else if (flags == GF_MOTOR_STOPPED_REV || flags == GF_MOTOR_RUNNING_REV) {
                g_state.motor_forward = false;
            }
            // Also update running state from GF (34 or 438 = running)
            g_state.motor_running = (flags == GF_MOTOR_RUNNING || flags == GF_MOTOR_RUNNING_REV);
            STATE_UNLOCK();

            if (error_state) {
                SEND_EVENT(EVT_MOTOR_FAULT);
            }

// Temperature monitoring
            const settings_t* s = settings_get();
            uint8_t temp_threshold = s->power.temp_threshold;

            // Critical overheat shutdown (safety-critical, kept here)
            if (temp >= TEMP_SHUTDOWN_DEFAULT) {
                if (motor_enabled) {
                    uart_puts("OVERHEAT SHUTDOWN!\r\n");
                    local_motor_stop();
                    SEND_EVENT(EVT_OVERHEAT);
                }
            }

            // Regular temperature monitoring (warning with hysteresis)
            temp_monitor_update(temp, temp_threshold);

            // Voltage monitoring - only check on unknown/error states
            // Known good states (32, 34, 436, 438) have various bits set that are
            // NOT error indicators - they're part of the state encoding.
            // Only report voltage issues for actual error states (bit 14 set).
            if (!known_good_state && !error_state) {
                // Unknown state - log for debugging
                if (!voltage_warning_active) {
                    voltage_warning_active = true;
                    uart_puts("Unknown GF state: ");
                    print_num(flags);
                    uart_puts("\r\n");
                }
            } else if (known_good_state) {
                voltage_warning_active = false;
            }

// Jam detection
            jam_load_update(load, motor_enabled, s->sensor.jam_detect,
                           s->sensor.spike_detect, s->sensor.spike_thresh);
        } else {
            // Got response but invalid format - count as failure (H5)
            consecutive_comm_failures++;
        }
    } else {
        // No response at all - communication failure (H5)
        consecutive_comm_failures++;
    }

    // Note: CL command returns "Current Limit" config, NOT current load!
    // Motor controller doesn't provide load sensing via serial.
    // Load is calculated from speed error in SV query below.

    // Query target speed via SV command (what motor is trying to achieve)
    motor_uart_flush_rx();  // Clear any leftover data from GF response
    send_query(CMD_SET_SPEED);  // SV query reads target speed setting
    if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
        size_t offset = protocol_find_stx(rx_buffer, rx_index, 3);
        if (offset != SIZE_MAX) {
            int16_t target_speed;
            if (protocol_parse_and_validate(rx_buffer, offset, rx_index, 1, SPEED_MAX_RPM, &target_speed)) {
                update_sv_state((uint16_t)target_speed);
            }
        }
    }

    // Query actual motor speed via CV command (measured feedback from motor)
    motor_uart_flush_rx();
    send_query(CMD_GET_CV);
    if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
        size_t offset = protocol_find_stx(rx_buffer, rx_index, 3);
        if (offset != SIZE_MAX) {
            int16_t actual_speed;
            if (protocol_parse_and_validate(rx_buffer, offset, rx_index, 0, SPEED_MAX_RPM, &actual_speed)) {
                update_cv_state((uint16_t)actual_speed);
            }
        }
    }

    // H4: Temperature query (T0 command)

// Temperature queried via temp_query_mcb()
    // Temperature now queried via temp_query_mcb() on-demand

    // H5: Check for consecutive communication failures
    if (consecutive_comm_failures >= MAX_COMM_FAILURES) {
        // Motor controller not responding - trigger fault and stop
        uart_puts("COMM FAULT!\r\n");
        motor_enabled = false;  // Mark as not running locally

        STATE_LOCK();
        g_state.motor_running = false;
        g_state.motor_fault = true;
        STATE_UNLOCK();

        SEND_EVENT(EVT_MOTOR_FAULT);
        consecutive_comm_failures = 0;  // Reset to allow recovery attempts
    }
}

// Query MCB temperature
static void motor_query_temperature(void) {
    temp_query_mcb();  // Now handled by temperature.c module
}

// Get current MCB temperature
uint16_t motor_get_temperature(void) {
    return temp_get_mcb();  // Returns cached MCB temp, or GD32 fallback
}

// Query MCB firmware version via GV command
//
static void motor_query_version(void) {
    motor_uart_flush_rx();
    send_query(CMD_GET_VERSION);  // "GV" command
    if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
        // Parse GV response: [STX]['1']['G']['V'][version_string][ETX]
        // Example: response "B1.7" stored as version string
        size_t offset = 0;
        for (size_t i = 0; i < rx_index && i < 3; i++) {
            if (rx_buffer[i] == 0x02) { offset = i; break; }
        }
        if (rx_index >= offset + 5 && rx_buffer[offset] == 0x02) {
            size_t data_start = offset + 4;  // After STX, unit, 'G', 'V'
            size_t len = 0;
            for (size_t i = data_start; i < rx_index && rx_buffer[i] != 0x03; i++) {
                if (len < sizeof(mcb_version) - 1) {
                    mcb_version[len++] = rx_buffer[i];
                }
            }
            mcb_version[len] = '\0';
        }
    }
}

// Get MCB firmware version string (for diagnostics)
const char* motor_get_version(void) {
    return mcb_version;
}

/*===========================================================================*/
/* Motor Initialization Helpers */
/*===========================================================================*/

// Boot timing macro (used during initialization)
#define TIME_MARK(msg, t_start) do { \
    TickType_t t = xTaskGetTickCount(); \
    uint32_t elapsed_ms = (t - (t_start)) * portTICK_PERIOD_MS; \
    char buf[16]; \
    int idx = 0; \
    uint32_t val = elapsed_ms; \
    do { buf[idx++] = '0' + (val % 10); val /= 10; } while (val && idx < 15); \
    uart_puts("["); \
    while (idx > 0) uart_putc(buf[--idx]); \
    uart_puts("ms] "); uart_puts(msg); uart_puts("\r\n"); \
} while(0)

/**
 * @brief Wait for MCB initialization complete (GF bit 3 clear)
 * @param t0 Boot timing reference
 * @return true if MCB ready, false on timeout
 */
static bool init_wait_for_mcb_ready(TickType_t t0) {
    #define GF_MOTOR_INIT_BIT 0x0008  // Bit 3

    TIME_MARK("Waiting for MCB ready (checking bit 3)...", t0);

    uint32_t timeout = 0;
    uint16_t flags = 0;

    while (timeout < 50) {  // Max 50 iterations = 500ms timeout
        // Query GF flags
        motor_uart_flush_rx();
        send_query(CMD_GET_FLAGS);
        if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
            // Parse GF response to get flags value
            size_t offset = 0;
            if (rx_index > 0 && rx_buffer[0] == 0x06) offset = 1;

            if (rx_index >= offset + 5 && rx_buffer[offset] == 0x02) {
                size_t data_start = offset + 4;
                flags = protocol_parse_field(rx_buffer, data_start, rx_index - data_start - 1);

                // Check bit 3
                if (!(flags & GF_MOTOR_INIT_BIT)) {
                    // Bit 3 clear - MCB ready!
                    uart_puts("  MCB ready (bit 3 clear), flags=");
                    char buf[8]; int i = 0;
                    do { buf[i++] = '0' + (flags % 10); flags /= 10; } while (flags && i < 7);
                    while (i > 0) uart_putc(buf[--i]);
                    uart_puts("\r\n");
                    return true;
                }
            }
        }

        delay_ms(10);
        timeout++;
        HEARTBEAT_UPDATE_MOTOR();
    }

    // Timeout
    uart_puts("  WARNING: MCB bit 3 timeout, flags=");
    char buf[8]; int i = 0; uint16_t v = flags;
    do { buf[i++] = '0' + (v % 10); v /= 10; } while (v && i < 7);
    while (i > 0) uart_putc(buf[--i]);
    uart_puts("\r\n");

    return false;
}

/**
 * @brief Perform MCB boot initialization sequence
 * @param full_init If true, do full init (cold/watchdog boot); if false, minimal init (soft boot)
 * @param t0 Boot timing reference
 */
static void init_mcb_boot_sequence(bool full_init, TickType_t t0) {
    // All boot types: minimal 50ms wait
    if (full_init) {
        TIME_MARK("Waiting for MCB ready (50ms)", t0);
    } else {
        uart_puts("Soft boot: minimal MCB wait\r\n");
    }
    delay_ms(50);
    HEARTBEAT_UPDATE_MOTOR();

    // SAFETY: Send RS=0 × 3 to ensure motor stopped before any init
    TIME_MARK("Safety stop (RS=0 x3)", t0);
    for (int i = 0; i < 3; i++) {
        send_command(CMD_STOP, 0);
        delay_ms(10);
    }
    HEARTBEAT_UPDATE_MOTOR();

    // NOTE: MCB params are factory-programmed and read-only
    // DO NOT sync params at boot - it breaks motor start!
    HEARTBEAT_UPDATE_MOTOR();

    // Query MCB firmware version
    TIME_MARK("Querying MCB version (GV)...", t0);
    motor_query_version();
    uart_puts("  MCB version: ");
    uart_puts(mcb_version);
    uart_puts("\r\n");
    HEARTBEAT_UPDATE_MOTOR();

    // Query S2 (MCB's stored fallback speed)
    send_query(CMD_SPEED_2);
    wait_response(MOTOR_RESPONSE_TIMEOUT_MS);

    // Wait for MCB ready flag (bit 3 clear) - full init only
    if (full_init) {
        init_wait_for_mcb_ready(t0);

        // Set forward direction after MCB ready
        if (!send_command(CMD_FORWARD, PARAM_FORWARD)) {
            uart_puts("  WARNING: JF init command timeout\r\n");
        }
        delay_ms(5);

        TIME_MARK("Motor init complete - MCB ready", t0);

        // Send boot complete event
        SEND_EVENT(EVT_BOOT_COMPLETE);
    }
}

/*===========================================================================*/
/* Task Entry Point                                                           */
/*===========================================================================*/

// External function from motor.c to sync settings
extern void motor_sync_settings(void);

void task_motor(void *pvParameters) {
    (void)pvParameters;

    // Boot timing reference
    TickType_t t0 = xTaskGetTickCount();
    uart_puts("[0ms] Motor task started\r\n");


    bool full_init = (g_boot_type == BOOT_COLD || g_boot_type == BOOT_WATCHDOG);
    init_mcb_boot_sequence(full_init, t0);

    motor_cmd_t cmd;
    TickType_t last_status_query = 0;


    // Idle: 500ms (2Hz) - reduce CPU/UART traffic
    // Running: 50ms (20Hz) - better responsiveness
    TickType_t status_interval = pdMS_TO_TICKS(MOTOR_STATUS_POLL_IDLE_MS);  // Start with idle rate

    for (;;) {
        // CRITICAL SAFETY: Update task heartbeat for watchdog monitoring
        HEARTBEAT_UPDATE_MOTOR();

        // NOTE: CV (Current Velocity) responses are parsed inline in wait_response()
        // The motor controller sends CV automatically while running

        // Check for commands (with timeout for periodic status query)
        if (xQueueReceive(g_motor_cmd_queue, &cmd, pdMS_TO_TICKS(MOTOR_CMD_QUEUE_TIMEOUT_MS)) == pdTRUE) {
            switch (cmd.cmd) {
                case CMD_MOTOR_STOP:
                    local_motor_stop();
                    break;

                case CMD_MOTOR_STOP_FAST:
                    // Fast stop for tapping - just RS=0, no post-stop sync
                    local_motor_stop_fast();
                    break;

                case CMD_MOTOR_BRAKE:
                    // Same as fast stop for tapping
                    local_motor_stop_fast();
                    break;

                case CMD_MOTOR_FORWARD:
                    local_motor_set_direction(true);
                    local_motor_start();
                    break;

                case CMD_MOTOR_REVERSE:
                    local_motor_set_direction(false);
                    local_motor_start();
                    break;

                case CMD_MOTOR_TAP_FORWARD:
                    // Same as CMD_MOTOR_FORWARD - tapping task handles stop timing
                    local_motor_set_direction(true);
                    local_motor_start();
                    break;

                case CMD_MOTOR_TAP_REVERSE:
                    // Same as CMD_MOTOR_REVERSE - tapping task handles stop timing
                    local_motor_set_direction(false);
                    local_motor_start();
                    break;

                case CMD_MOTOR_SET_SPEED:
                    local_motor_set_speed(cmd.param);
                    break;

                case CMD_MOTOR_QUERY_STATUS:
                    motor_query_status();
                    break;

                case CMD_MOTOR_QUERY_TEMP:
                    motor_query_temperature();
                    break;

                case CMD_MOTOR_SPINDLE_HOLD:
                    spindle_hold_start(false);  // Manual hold, no timeout
                    break;

                case CMD_MOTOR_SPINDLE_HOLD_SAFETY:
                    spindle_hold_start(true);   // Safety hold with timeout
                    break;

                case CMD_MOTOR_SPINDLE_RELEASE:
                    spindle_hold_release();
                    break;

                case CMD_MOTOR_READ_PARAMS:
                    // Read all MCB parameters and store in shared state
                    {
                        // Disable background polling during parameter read
                        motor_scan_mode = true;  // Reuse scan mode flag

                        // Flush any pending RX data from previous polling
                        motor_uart_flush_rx();
                        delay_ms(10);  // Let any in-flight responses drain
                        motor_uart_flush_rx();

                        mcb_params_t params;
                        if (motor_read_mcb_params(&params)) {
                            STATE_LOCK();
                            g_state.mcb_params.pulse_max = params.pulse_max;
                            g_state.mcb_params.adv_max = params.adv_max;
                            g_state.mcb_params.ir_gain = params.ir_gain;
                            g_state.mcb_params.ir_offset = params.ir_offset;
                            g_state.mcb_params.cur_lim = params.cur_lim;
                            g_state.mcb_params.spd_rmp = params.spd_rmp;
                            g_state.mcb_params.trq_rmp = params.trq_rmp;
                            g_state.mcb_params.voltage_kp = params.voltage_kp;
                            g_state.mcb_params.voltage_ki = params.voltage_ki;
                            g_state.mcb_params.valid = true;
                            STATE_UNLOCK();
                        } else {
                            STATE_LOCK();
                            g_state.mcb_params.valid = false;
                            STATE_UNLOCK();
                        }

                        // Re-enable background polling
                        motor_scan_mode = false;
                    }
                    break;
            }
        }

        // Maintain spindle hold (handles periodic refresh and safety timeout)
        spindle_hold_update();

        // Periodic status query
        // Skip polling if MCBSCAN is running (prevents conflicts)
        TickType_t now = xTaskGetTickCount();
        if (!motor_scan_mode && (now - last_status_query) >= status_interval) {
            last_status_query = now;

            STATE_LOCK();
            bool running = g_state.motor_running;
            STATE_UNLOCK();


            // Running: 50ms (20Hz) for better responsiveness
            // Idle: 500ms (2Hz) to reduce CPU/UART traffic
            if (running && motor_enabled) {
                status_interval = pdMS_TO_TICKS(MOTOR_STATUS_POLL_RUNNING_MS);  // 20Hz
            } else {
                status_interval = pdMS_TO_TICKS(MOTOR_STATUS_POLL_IDLE_MS);  // 2Hz
            }

            // Original firmware polling pattern
            // When idle: GF × 2, then KR × 1 (catches state changes faster)
            // When running: Full motor_query_status() (includes GF, SV, CV)
            //
            // GF polling provides faster motor state detection than KR alone.
            // Original firmware polls GF every ~20ms idle, KR every ~300ms.

            if (running && motor_enabled) {
                // Motor running - full status query (includes GF, SV, CV)
                motor_query_status();
            } else {
                // Motor idle - match original firmware pattern: GF × 2, KR × 1
                // GF query to detect motor state/direction changes
                send_query(CMD_GET_FLAGS);
                if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
                    // Parse GF response for state changes
                    size_t offset = 0;
                    if (rx_index > 0 && rx_buffer[0] == 0x06) offset = 1;
                    if (rx_index >= offset + 5 && rx_buffer[offset] == 0x02) {
                        size_t data_start = offset + 4;
                        uint16_t flags = protocol_parse_field(rx_buffer, data_start, rx_index - data_start - 1);
                        // Update motor state from GF flags
                        bool known_good = (flags == GF_MOTOR_STOPPED || flags == GF_MOTOR_RUNNING ||
                                           flags == GF_MOTOR_STOPPED_REV || flags == GF_MOTOR_RUNNING_REV);
                        bool error_state = (flags & 0x4000) != 0;  // Error bit
                        STATE_LOCK();
                        if (flags == GF_MOTOR_STOPPED || flags == GF_MOTOR_RUNNING) {
                            g_state.motor_forward = true;
                        } else if (flags == GF_MOTOR_STOPPED_REV || flags == GF_MOTOR_RUNNING_REV) {
                            g_state.motor_forward = false;
                        }
                        g_state.motor_running = (flags == GF_MOTOR_RUNNING || flags == GF_MOTOR_RUNNING_REV);
                        if (known_good || error_state) {  // Only update fault from trusted flags
                            g_state.motor_fault = error_state;
                        }
                        STATE_UNLOCK();
                        if (error_state) {
                            SEND_EVENT(EVT_MOTOR_FAULT);
                        }
                    }
                }

                // Second GF query (matches original firmware pattern)
                send_query(CMD_GET_FLAGS);
                wait_response(MOTOR_RESPONSE_TIMEOUT_MS);

                // KR heartbeat query
                // KR = motor load percentage (0% when stopped, 19% idle running, up to 30% loaded)
                send_query(CMD_KEEP_RUNNING);
                wait_response(MOTOR_RESPONSE_TIMEOUT_MS);
            }
        }
    }
}

/*===========================================================================*/
/* Initialization                                                             */
/*===========================================================================*/

void motor_task_init(void) {
    motor_uart_init();  // Direct register USART3 init - shared with motor.c
    // Note: motor.c uses the same USART3 with direct register access now

    // Initialize motor hardware enable pin (PD4)
    // CRITICAL SAFETY: This pin provides hardware-level motor cutoff
    RCC->APB2ENR |= RCC_APB2ENR_IOPDEN;  // Enable GPIOD clock
    GPIOD->CRL &= ~(0xF << 16);          // Clear PD4 config bits
    GPIOD->CRL |= (0x3 << 16);           // PD4: Output push-pull, 50MHz
    GPIOD->BSRR = (1 << (4 + 16));       // Set PD4 LOW initially (motor disabled)

    // Verbose output only on full boot
    extern boot_type_t g_boot_type;
    if (g_boot_type == BOOT_COLD || g_boot_type == BOOT_WATCHDOG) {
        extern void uart_puts(const char* s);
        uart_puts("Motor enable pin (PD4) initialized - motor disabled\r\n");
    }
}
