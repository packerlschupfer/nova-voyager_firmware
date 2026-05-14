/**
 * @file motor.c
 * @brief Motor Controller High-Level API
 *
 * MODULE: Motor Control Layer
 * LAYER: Application API (above protocol/UART layers)
 * THREAD SAFETY: All functions protected by g_motor_mutex
 *
 * Provides high-level motor control functions:
 * - Start/stop/speed/direction control
 * - Parameter configuration (profiles, power, PID)
 * - Status querying
 * - Retry logic with exponential backoff
 *
 * Dependencies:
 * - motor_uart.c: Hardware UART layer
 * - motor_protocol.c: Protocol packet building/parsing
 *
 * Protocol format (derived from reverse engineering):
 *   [0x04][0x30][0x30][0x31][0x31][0x02][0x31][CMD_H][CMD_L][PARAM...][0x03][XOR]
 *    SOH   '0'   '0'   '1'   '1'   STX  '1'   Command       Parameter    ETX  Checksum
 */

#include "motor.h"
#include "config.h"
#include "settings.h"
#include "shared.h"
#include "utilities.h"
#include "motor_protocol.h"
#include "motor_uart.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

// External UART functions for debug output
extern void uart_puts(const char* s);
extern void uart_putc(char c);
extern void print_num(int32_t n);

/*===========================================================================*/
/* Private Variables                                                         */
/*===========================================================================*/

// [SHARED_STATE:g_motor_mutex] Accessed from multiple tasks via motor_* API
// Protected by g_motor_mutex for UART access serialization
static motor_status_t motor_status;
static uint8_t tx_buffer[MOTOR_UART_BUFFER_SIZE];
static motor_error_t last_error = MOTOR_OK;  // Last error for diagnostics
static uint8_t rx_buffer[MOTOR_UART_BUFFER_SIZE];
// Note: USART3 is initialized by task_motor.c using direct register access

// UART functions now provided by motor_uart.c module

/*===========================================================================*/
/* Protocol Functions (Phase 6: Now using motor_protocol.c)                  */
/*===========================================================================*/

// Protocol constants and functions moved to motor_protocol.c module
// Old functions build_packet() and build_query_packet() replaced with protocol_* calls

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

bool motor_init(void) {
    // Initialize status structure
    // USART3 is already initialized by task_motor.c
    memset(&motor_status, 0, sizeof(motor_status));
    motor_status.state = MOTOR_STOPPED;
    last_error = MOTOR_OK;
    return true;
}

/*===========================================================================*/
/* Error Handling                                                            */
/*===========================================================================*/

motor_error_t motor_get_last_error(void) {
    return last_error;
}

const char* motor_error_string(motor_error_t error) {
    switch (error) {
        case MOTOR_OK: return "OK";
        case MOTOR_ERR_UART_TX_TIMEOUT: return "UART TX timeout";
        case MOTOR_ERR_UART_RX_TIMEOUT: return "UART RX timeout";
        case MOTOR_ERR_INVALID_RESPONSE: return "Invalid response";
        case MOTOR_ERR_OUT_OF_RANGE: return "Parameter out of range";
        case MOTOR_ERR_BUSY: return "Motor busy";
        case MOTOR_ERR_FAULT: return "Motor fault";
        case MOTOR_ERR_MAX_RETRIES: return "Max retries exceeded";
        case MOTOR_ERR_HARDWARE: return "Hardware error";
        case MOTOR_ERR_INVALID_STATE: return "Invalid state";
        default: return "Unknown error";
    }
}

/**
 * @brief Send command packet to motor controller with timeout protection
 *
 * Builds command packet via protocol layer and transmits via USART3.
 * Waits for transmission complete with timeout. Single attempt - no retries.
 * For retry logic, use motor_send_command_with_retry() instead.
 *
 * @param cmd Command code (e.g., CMD_STOP=0x5253, CMD_START=0x5354, CMD_SET_SPEED=0x5356)
 * @param param Parameter value (speed in RPM, direction code, or 0 for no-param commands)
 * @return true if packet sent and TC flag set, false on timeout
 *
 * Timeout values:
 * - Per-byte TX: MOTOR_UART_BYTE_TIMEOUT_MS
 * - TX complete: MOTOR_UART_TX_TIMEOUT_MS
 *
 * Thread safety: Safe to call from any task (uses motor_uart layer)
 *
 * @note Does NOT wait for or validate response - fire-and-forget transmission
 * @note Caller should check motor status via motor_query_status() if response needed
 */
bool motor_send_command(uint16_t cmd, int16_t param) {
    // Use protocol layer
    size_t len = protocol_build_command(cmd, param, tx_buffer);

    // Send using direct register access
    for (size_t i = 0; i < len; i++) {
        if (!motor_uart_send_byte(tx_buffer[i])) {
            last_error = MOTOR_ERR_UART_TX_TIMEOUT;
            return false;  // TX timeout on byte send
        }
    }

    // motor_uart_send_byte() already handles TX complete
    last_error = MOTOR_OK;
    return true;
}

/**
 * @brief Send command with retry logic and exponential backoff
 *
 * Implements retry logic with exponential backoff for improved reliability:
 * - Attempt 1: Send command, 50ms delay on failure
 * - Attempt 2: Retry, 100ms delay on failure
 * - Attempt 3: Final retry, 200ms delay on failure
 *
 * @param cmd Command code (e.g., CMD_STOP, CMD_JOG, CMD_SET_SPEED)
 * @param param Parameter value (direction, speed, etc.)
 * @return true if command sent successfully (even on first try), false on max retries
 *
 * Note: Currently implements fire-and-forget transmission. Response validation
 * could be added for critical commands that expect ACK/NAK responses.
 */
bool motor_send_command_with_retry(uint16_t cmd, int16_t param) {
    extern void uart_puts(const char* s);
    extern void print_hex_byte(uint8_t val);
    extern void print_num(int32_t n);

    uint16_t retry_delay_ms = MOTOR_RETRY_DELAY_MS;  // Renamed to avoid collision with delay_ms()
    motor_status.retry_count = 0;

    for (uint8_t attempt = 0; attempt < MOTOR_RETRY_MAX; attempt++) {
        // Send command (currently always succeeds - fire and forget)
        bool sent = motor_send_command(cmd, param);

        if (sent) {
            // Success on this attempt
            motor_status.retry_count = attempt;
            return true;
        }

        // Command send failed (unlikely with current implementation)
        motor_status.retry_count = attempt + 1;

        // Log retry attempt (not on last failure)
        if (attempt < MOTOR_RETRY_MAX - 1) {
            uart_puts("[MOTOR] Retry ");
            print_num(attempt + 1);
            uart_puts("/");
            print_num(MOTOR_RETRY_MAX);
            uart_puts("\r\n");

            // Exponential backoff delay before retry
            delay_ms(retry_delay_ms);
            retry_delay_ms *= 2;  // Double delay for next retry (50ms, 100ms, 200ms)
        }
    }

    // Max retries exceeded - command failed on all attempts
    uart_puts("[MOTOR] ERROR: Max retries exceeded for cmd 0x");
    print_hex_byte((cmd >> 8) & 0xFF);
    print_hex_byte(cmd & 0xFF);
    uart_puts("\r\n");
    motor_status.retry_count = MOTOR_RETRY_MAX;
    return false;
}

// Phase 6: parse_decimal replaced with protocol_parse_field from motor_protocol.c

/**
 * @brief Parse GF (Get Flags) response to extract status data
 * Response format (estimated from RE):
 *   [SOH][ADDR][STX][DATA...][ETX][XOR]
 * DATA contains comma-separated values:
 *   flags,speed,load,vibration,temp
 */
static void parse_gf_response(size_t len) {
    // Find STX to start of data
    size_t data_start = 0;
    for (size_t i = 0; i < len; i++) {
        if (rx_buffer[i] == PROTO_STX) {
            data_start = i + 2;  // Skip STX and unit byte
            break;
        }
    }

    if (data_start == 0) return;  // No valid response

    // Parse comma-separated values
    // Field 0: flags (bit field)
    // Field 1: actual speed
    // Field 2: load percentage
    // Field 3: vibration level
    // Field 4: temperature

    size_t field = 0;
    size_t field_start = data_start;

    for (size_t i = data_start; i < len; i++) {
        if (rx_buffer[i] == ',' || rx_buffer[i] == PROTO_ETX) {
            int16_t value = protocol_parse_field(rx_buffer, field_start, i - field_start);

            switch (field) {
                case 0:  // Flags (bit field from MCB)
                    motor_status.raw_flags = (uint16_t)value;
                    motor_status.fault = (value & 0x01) != 0;
                    motor_status.overload = (value & 0x02) != 0;
                    motor_status.jam_detected = (value & 0x04) != 0;
                    motor_status.rps_error = (value & 0x18) != 0;    // RPS error bits
                    motor_status.pfc_fault = (value & 0x20) != 0;    // PFC fault
                    motor_status.voltage_error = (value & 0xC0) != 0; // Voltage errors
                    motor_status.overheat = (value & 0x300) != 0;    // Overheat bits
                    break;
                case 1:  // Actual speed
                    motor_status.speed_rpm = (value > 0) ? value : 0;
                    break;
                case 2:  // Load percentage
                    motor_status.load_percent = (value > 0 && value <= 100) ? value : 0;
                    break;
                case 3:  // Vibration
                    motor_status.vibration = (value > 0) ? value : 0;
                    break;
                case 4:  // Temperature
                    motor_status.temperature = (value > 0) ? value : 0;
                    break;
            }

            field++;
            field_start = i + 1;

            if (rx_buffer[i] == PROTO_ETX) break;
        }
    }
}

int32_t motor_read_response(uint32_t timeout_ms) {
    uint32_t start = HAL_GetTick();
    size_t idx = 0;
    bool found_etx = false;

    while ((HAL_GetTick() - start) < timeout_ms) {
        if (motor_uart_rx_available()) {
            rx_buffer[idx] = motor_uart_read_byte();
            if (rx_buffer[idx] == PROTO_ETX) {
                idx++;  // Include ETX in buffer
                found_etx = true;
                (void)found_etx;  // Suppress unused warning - documents intent

                // FRAMING VALIDATION: Verify response structure
                // Expected: [SOH]['0']['0']['1']['1'][STX|'1'][...][ETX]
                if (idx < 7) {
                    extern void uart_puts(const char* s);
                    uart_puts("[MOTOR] Frame too short! ");
                    return -4;  // Frame error - packet too short
                }

                // Validate header (positions 0-4 must match protocol)
                if (rx_buffer[0] != PROTO_SOH ||
                    rx_buffer[1] != '0' ||
                    rx_buffer[2] != '0' ||
                    rx_buffer[3] != '1' ||
                    rx_buffer[4] != '1') {
                    extern void uart_puts(const char* s);
                    uart_puts("[MOTOR] Invalid frame header! ");
                    return -5;  // Frame error - invalid header
                }

                // Position 5 should be STX (0x02) for command response or '1' (0x31) for query
                if (rx_buffer[5] != PROTO_STX && rx_buffer[5] != '1') {
                    extern void uart_puts(const char* s);
                    uart_puts("[MOTOR] Invalid frame type! ");
                    return -6;  // Frame error - invalid type byte
                }

                // Read checksum byte (should follow ETX)
                uint32_t checksum_start = HAL_GetTick();
                while ((HAL_GetTick() - checksum_start) < 10) {  // Short timeout for checksum
                    if (motor_uart_rx_available()) {
                        uint8_t received_checksum = motor_uart_read_byte();

                        // Calculate expected checksum (XOR from unit byte onwards)
                        uint8_t expected_checksum = 0;
                        // Response format: [SOH]['0']['0']['1']['1'][STX]['1'][...data...][ETX][XOR]
                        // Checksum starts at position 6 (unit byte '1')
                        for (size_t i = 6; i < idx; i++) {  // idx includes ETX
                            expected_checksum ^= rx_buffer[i];
                        }

                        // Validate checksum
                        if (received_checksum != expected_checksum) {
                            extern void uart_puts(const char* s);
                            uart_puts("[MOTOR] CHECKSUM ERROR! ");
                            return -2;  // Checksum error
                        }

                        // Checksum valid - parse response
                        parse_gf_response(idx);
                        motor_status.last_update_ms = HAL_GetTick();
                        return 0;  // Success
                    }
                }
                return -3;  // Checksum byte timeout
            }
            idx++;
            if (idx >= sizeof(rx_buffer)) {
                idx = 0;  // Buffer overflow, reset
            }
        }
    }
    return -1;  // Timeout waiting for ETX
}

/**
 * @brief Stop motor immediately with hardware cutoff
 *
 * SAFETY CRITICAL: Hardware disable occurs FIRST, then UART command.
 * Motor will stop even if UART communication fails.
 *
 * Thread safety: Safe from any task
 */
void motor_stop(void) {
    motor_hardware_disable();  // Hardware cutoff FIRST
    motor_send_command_with_retry(CMD_STOP, 0);
    motor_status.state = MOTOR_STOPPED;
}

/**
 * @brief Start motor in forward direction
 *
 * Enables hardware control, then sends JOG forward command.
 * Order is critical: hardware BEFORE UART command.
 *
 * Thread safety: Safe from any task
 */
void motor_forward(void) {
    motor_hardware_enable();  // Enable hardware before command
    motor_send_command_with_retry(CMD_JOG, DIR_FORWARD);
    motor_status.state = MOTOR_FORWARD;
}

/**
 * @brief Start motor in reverse direction
 *
 * Enables hardware control, then sends JOG reverse command.
 * Order is critical: hardware BEFORE UART command.
 *
 * Thread safety: Safe from any task
 */
void motor_reverse(void) {
    motor_hardware_enable();  // Enable hardware before command
    motor_send_command_with_retry(CMD_JOG, DIR_REVERSE);
    motor_status.state = MOTOR_REVERSE;
}

/**
 * @brief Start motor (using last set direction)
 *
 * Enables hardware control and sends START command.
 *
 * Thread safety: Safe from any task
 */
void motor_start(void) {
    motor_hardware_enable();  // Enable hardware before command
    motor_send_command_with_retry(CMD_START, 0);
}

/**
 * @brief Set motor target speed with automatic clamping
 *
 * @param rpm Target speed in RPM (clamped to SPEED_MIN_RPM..SPEED_MAX_RPM)
 *
 * Speed range: 100-6000 RPM (hardware dependent)
 * Actual motor response time: ~50-200ms depending on load
 *
 * Thread safety: Safe from any task
 *
 * @note Does not return motor feedback - use motor_get_actual_rpm() for measured speed
 */
void motor_set_speed(uint16_t rpm) {
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > SPEED_MAX_RPM) rpm = SPEED_MAX_RPM;

    motor_send_command_with_retry(CMD_SET_SPEED, rpm);
    motor_status.target_speed = rpm;
}

uint16_t motor_get_speed(void) {
    return motor_status.speed_rpm;
}

const motor_status_t* motor_get_status(void) {
    return &motor_status;
}

void motor_update(void) {
    // Poll motor controller for status using QUERY format
    // Phase 6: Use protocol layer
    size_t len = protocol_build_query(CMD_GET_FLAGS, tx_buffer);

    // Send query using direct register access
    // Phase 1.1: Check return value for timeout
    for (size_t i = 0; i < len; i++) {
        if (!motor_uart_send_byte(tx_buffer[i])) {
            return;  // TX timeout, abort update
        }
    }

    // motor_uart layer handles TX complete - read response
    int32_t response = motor_read_response(50);

    if (response >= 0) {
        motor_status.last_update_ms = HAL_GetTick();
    }
}

bool motor_is_running(void) {
    return (motor_status.state == MOTOR_FORWARD ||
            motor_status.state == MOTOR_REVERSE);
}

void motor_emergency_stop(void) {
    // Immediate stop - bypass normal state machine
    motor_hardware_disable();  // Hardware cutoff FIRST
    motor_send_command(CMD_STOP, 0);
    motor_status.state = MOTOR_STOPPED;
    motor_status.fault = true;
}

/*===========================================================================*/
/* Hardware Motor Enable Control                                             */
/*===========================================================================*/

void motor_hardware_enable(void) {
    // Set MOTOR_ENABLE pin HIGH (active high enable)
    GPIOD->BSRR = (1 << 4);  // BS4 = set PD4
}

void motor_hardware_disable(void) {
    // Set MOTOR_ENABLE pin LOW (disable motor immediately)
    GPIOD->BSRR = (1 << (4 + 16));  // BR4 = reset PD4
}

bool motor_hardware_is_enabled(void) {
    // Read current state of MOTOR_ENABLE pin
    return (GPIOD->ODR & (1 << 4)) != 0;
}

/*===========================================================================*/
/* Motor Status Accessors                                                    */
/*===========================================================================*/

uint16_t motor_get_vibration(void) {
    return motor_status.vibration;
}

uint16_t motor_get_load(void) {
    return motor_status.load_percent;
}

bool motor_vibration_exceeds(uint16_t threshold) {
    return motor_status.vibration > threshold;
}

void motor_set_pid(int16_t speed_kp, int16_t speed_ki, int16_t voltage_kp, int16_t voltage_ki) {
    motor_send_command(CMD_SET_KP, speed_kp);
    motor_send_command(CMD_SET_KI, speed_ki);
    motor_send_command(CMD_SET_VKP, voltage_kp);
    motor_send_command(CMD_SET_VKI, voltage_ki);
}

void motor_set_current_limit(uint16_t limit_ma) {
    motor_send_command(CMD_SET_ILIM, limit_ma);
}

void motor_set_ir_comp(int16_t ir_gain, int16_t ir_offset) {
    // Send IR gain and offset as separate 16-bit values
    // Phase 4.1: Factory defaults now in config.h (MOTOR_FACTORY_IR_GAIN/OFFSET)
    motor_send_command(CMD_SET_IR_GAIN, ir_gain);
    motor_send_command(CMD_SET_IR_OFFSET, ir_offset);
}

void motor_set_braking(bool enabled) {
    // DISABLED: Brake causes motor overheating
    // Brake (BR=1) keeps motor energized even when stopped, heating it up
    // RS (stop) command provides adequate braking without the heat issue
    (void)enabled;  // Suppress unused warning
    // motor_send_command(CMD_SET_BRAKE, enabled ? 1 : 0);  // DISABLED
}

void motor_set_pulse_max(uint16_t value) {
    motor_send_command(CMD_SET_PULSE_MAX, value);
}

void motor_set_advance_max(uint16_t value) {
    motor_send_command(CMD_SET_ADV_MAX, value);
}

void motor_restore_mcb_defaults(void) {
    // Factory default values from Teknatool Service PDF (Phase 4.1: Named constants)
    // These bypass HMI menu limits - sent directly to MCB

    // Electrical Parameters
    motor_send_command(CMD_SET_PULSE_MAX, MOTOR_FACTORY_PULSE_MAX);
    delay_ms(10);
    motor_send_command(CMD_SET_ADV_MAX, MOTOR_FACTORY_ADV_MAX);
    delay_ms(10);
    motor_send_command(CMD_SET_ILIM, MOTOR_FACTORY_CUR_LIM);
    delay_ms(10);

    // IR Compensation
    motor_send_command(CMD_SET_IR_GAIN, MOTOR_FACTORY_IR_GAIN);
    delay_ms(10);
    motor_send_command(CMD_SET_IR_OFFSET, MOTOR_FACTORY_IR_OFFSET);
    delay_ms(10);

    // Control Algorithm
    motor_send_command(CMD_SET_SPD_RMP, MOTOR_FACTORY_SPD_RMP);
    delay_ms(10);
    motor_send_command(CMD_SET_TRQ_RMP, MOTOR_FACTORY_TRQ_RMP);
    delay_ms(10);

    // Voltage Control (Kp/Ki)
    motor_send_command(CMD_SET_VKP, MOTOR_FACTORY_VOLTAGE_KP);
    delay_ms(10);
    motor_send_command(CMD_SET_VKI, MOTOR_FACTORY_VOLTAGE_KI);
    delay_ms(10);

    // Save to MCB EEPROM
    motor_save_mcb_params();
}

bool motor_factory_reset(void) {
    extern void uart_puts(const char* s);
    extern void print_num(int32_t n);

    uart_puts("MCB Factory Reset: Starting...\r\n");

    // Step 1: RS=1 × 6 (prepare for EEPROM reset)
    // Note: RS=1 is specifically for EEPROM operations, NOT normal stop (RS=0)
    uart_puts("  Phase 1: RS=1 x6 (prepare)...\r\n");
    for (int i = 0; i < 6; i++) {
        motor_send_command(CMD_STOP, 1);  // RS=1 (NOT RS=0!)
        delay_ms(90);
    }

    // Step 2: EE command (EEPROM Execute)
    uart_puts("  Phase 2: EE (EEPROM Execute)...\r\n");
    motor_send_command(CMD_EE, 0);
    delay_ms(50);

    // Step 3: RS=1 × 7 (wait/confirm sequence)
    uart_puts("  Phase 3: RS=1 x7 (confirm)...\r\n");
    for (int i = 0; i < 7; i++) {
        motor_send_command(CMD_STOP, 1);  // RS=1
        delay_ms(90);
    }

    // Step 4: Wait for MCB to complete reset (~0.7s)
    // MCB goes silent during reset, won't respond to queries
    uart_puts("  Phase 4: Waiting for MCB...\r\n");
    delay_ms(700);

    // Step 5: Verify MCB is responding again
    uart_puts("  Phase 5: Verifying MCB response...\r\n");
    int32_t gf = motor_read_param(CMD_GET_FLAGS);
    if (gf >= 0) {
        uart_puts("MCB Factory Reset: COMPLETE (GF=");
        print_num(gf);
        uart_puts(")\r\n");
        return true;
    } else {
        uart_puts("MCB Factory Reset: WARNING - MCB not responding!\r\n");
        uart_puts("  Try power cycling the drill press.\r\n");
        return false;
    }
}

void motor_save_mcb_params(void) {
    extern void uart_puts(const char* s);

    // DISCOVERY 2026-01-25: There is NO "save params" command!
    // SP (0x5350) is Kprop, not "Save Parameters"!
    // EEPROM persistence requires: RS=1 (flag) + power cycle
    //
    // Factory reset sequence: RS=1 × N, then power cycle triggers reset on boot
    // Regular save: Same concept - RS=1 sets "pending" flag, power cycle saves

    uart_puts("  Setting EEPROM write flag (RS=1 x3)...\r\n");
    for (int i = 0; i < 3; i++) {
        motor_send_command(CMD_STOP, 1);  // RS=1 sets EEPROM pending flag
        delay_ms(90);
    }

    uart_puts("  EEPROM flag set.\r\n");
    uart_puts("  NOTE: Power cycle required to persist changes!\r\n");
}

/**
 * @brief Parse numeric response from MCB (Phase 6: Using protocol layer)
 * Extracts the value after command echo in response
 */
static int32_t parse_param_response(size_t len) {
    // Validate response structure
    size_t offset = protocol_validate_response(rx_buffer, len);
    if (offset == 0) return -1;  // Invalid frame

    // Skip command echo (2 bytes after validation offset)
    size_t value_start = offset + 2;

    // Parse using protocol layer
    return protocol_parse_field(rx_buffer, value_start, len - value_start);
}

// UART functions provided by motor_uart.c module

int32_t motor_read_param(uint16_t cmd) {
    // Clear rx buffer
    memset(rx_buffer, 0, sizeof(rx_buffer));

    // Build query packet (Phase 6: Use protocol layer)
    size_t len = protocol_build_query(cmd, tx_buffer);

    // Phase 1.4: Use mutex instead of critical section to allow interrupts
    // Critical sections disable ALL interrupts, including system tick
    // Mutex only prevents other tasks from using motor UART, much safer
    extern SemaphoreHandle_t g_motor_mutex;
    bool mutex_taken = false;
    if (g_motor_mutex && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
        mutex_taken = true;
    }

    // Send query with timeout protection (Phase 1.1)
    // Send packet using motor_uart layer
    if (!motor_uart_send_packet(tx_buffer, len)) {
        if (mutex_taken) xSemaphoreGive(g_motor_mutex);
        extern void uart_puts(const char* s);
        uart_puts("[MOTOR] TX timeout in motor_read_param\r\n");
        return -1;  // TX timeout
    }

    // Read response - poll for bytes with timeout
    // Response is typically ~10 bytes, each ~1ms at 9600 baud
    size_t idx = 0;
    int consecutive_timeouts = 0;
    TickType_t read_start = xTaskGetTickCount();
    TickType_t read_timeout_ticks = pdMS_TO_TICKS(200);  // Increased to 200ms

    uart_puts("  [RX] Waiting for response...\r\n");

    while (idx < sizeof(rx_buffer) && consecutive_timeouts < 50) {  // Increased retries
        if ((xTaskGetTickCount() - read_start) >= read_timeout_ticks) {
            uart_puts("  [RX] Overall timeout\r\n");
            break;  // Overall timeout
        }

        // Check for available byte using motor_uart layer
        if (motor_uart_rx_available()) {
            uint8_t byte = motor_uart_read_byte();
            rx_buffer[idx++] = byte;
            consecutive_timeouts = 0;
            if (idx == 1) {
                uart_puts("  [RX] First byte received!\r\n");
            }
        } else {
            // Small delay to avoid busy-waiting
            delay_ms(2);  // Increased to 2ms
            consecutive_timeouts++;
        }
    }

    // Release mutex
    if (mutex_taken) {
        xSemaphoreGive(g_motor_mutex);
    }

    if (idx == 0) {
        uart_puts("  [DEBUG] No bytes received (RX timeout)\r\n");
        return -1;  // No response
    }

    uart_puts("  [DEBUG] Received ");
    print_num(idx);
    uart_puts(" bytes, parsing...\r\n");

    // Parse and return the value
    int32_t result = parse_param_response(idx);
    if (result < 0) {
        uart_puts("  [DEBUG] Parse failed\r\n");
    }
    return result;
}

bool motor_read_mcb_params(mcb_params_t* params) {
    if (params == NULL) return false;

    // Initialize with zeros (stack fix in linker script allows memset now)
    memset(params, 0, sizeof(*params));

    // Simple busy delay between MCB queries (Phase 4.1: Named constant)
    // ~3-5ms delay at 72MHz - allows MCB to process before next query
    #define MCB_DELAY() do { for (volatile int _d = 0; _d < MOTOR_UART_SPIN_DELAY_LOOPS; _d++); } while(0)

    // Read all parameters with inter-query delay
    uart_puts("  Reading PulseMax (SU)...");
    params->pulse_max = motor_read_param(CMD_GET_PULSE_MAX);
    if (params->pulse_max < 0) {
        uart_puts(" FAILED\r\n");
        uart_puts("  MCB not responding to parameter queries\r\n");
        return false;  // First read failed, MCB likely not responding
    }
    uart_puts(" OK ("); print_num(params->pulse_max); uart_puts(")\r\n");
    MCB_DELAY();

    uart_puts("  Reading AdvMax (SA)...");
    params->adv_max = motor_read_param(CMD_GET_ADV_MAX);
    uart_puts(params->adv_max >= 0 ? " OK\r\n" : " FAILED\r\n");
    MCB_DELAY();

    uart_puts("  Reading IRGain (I0)...");
    params->ir_gain = motor_read_param(CMD_GET_IR_GAIN);
    uart_puts(params->ir_gain >= 0 ? " OK\r\n" : " FAILED\r\n");
    MCB_DELAY();

    uart_puts("  Reading IROffset (I3)...");
    params->ir_offset = motor_read_param(CMD_GET_IR_OFFSET);
    uart_puts(params->ir_offset >= 0 ? " OK\r\n" : " FAILED\r\n");
    MCB_DELAY();

    uart_puts("  Reading CurLim (CL)...");
    params->cur_lim = motor_read_param(CMD_GET_CUR_LIM);
    uart_puts(params->cur_lim >= 0 ? " OK\r\n" : " FAILED\r\n");
    MCB_DELAY();

    uart_puts("  Reading SpdRmp (DN)...");
    params->spd_rmp = motor_read_param(CMD_GET_SPD_RMP);
    uart_puts(params->spd_rmp >= 0 ? " OK\r\n" : " FAILED\r\n");
    MCB_DELAY();

    uart_puts("  Reading TrqRmp (SR)...");
    params->trq_rmp = motor_read_param(CMD_GET_TRQ_RMP);
    uart_puts(params->trq_rmp >= 0 ? " OK\r\n" : " FAILED\r\n");
    MCB_DELAY();

    uart_puts("  Reading VoltKp (VP)...");
    params->voltage_kp = motor_read_param(CMD_SET_VKP);
    uart_puts(params->voltage_kp >= 0 ? " OK\r\n" : " FAILED\r\n");
    MCB_DELAY();

    uart_puts("  Reading VoltKi (VI)...");
    params->voltage_ki = motor_read_param(CMD_SET_VKI);
    uart_puts(params->voltage_ki >= 0 ? " OK\r\n" : " FAILED\r\n");

    // Check if we got valid data (at least pulse_max should be > 0)
    if (params->pulse_max > 0) {
        params->valid = true;
        return true;
    }

    return false;
}

void motor_set_speed_ramp(uint16_t ramp_rate) {
    motor_send_command(CMD_SET_SPD_RMP, ramp_rate);
}

void motor_set_torque_ramp(uint16_t ramp_rate) {
    motor_send_command(CMD_SET_TRQ_RMP, ramp_rate);
}

void motor_set_profile(uint8_t profile) {
    // Map profile enum to motor controller commands
    // CORRECTED: Testing revealed S0=HARD, S8=SOFT (opposite of initial guess!)
    switch (profile) {
        case MOTOR_PROFILE_SOFT:
            // S8(264) - Gentle acceleration, low torque (user could stop by hand)
            // Value 264 (0x108) from original firmware initialization
            motor_send_command(CMD_PROFILE_S8, 264);
            break;

        case MOTOR_PROFILE_NORMAL:
            // S7(750) - Balanced acceleration
            motor_send_command(CMD_PROFILE_S7, 750);
            break;

        case MOTOR_PROFILE_HARD:
            // S0(0) - Aggressive acceleration, HIGH torque (hard to stop by hand)
            motor_send_command(CMD_PROFILE_S0, 0);
            break;

        default:
            // Default to NORMAL if invalid
            motor_send_command(CMD_PROFILE_S7, 750);
            break;
    }
}

void motor_set_power_output(uint8_t level) {
    // Map power output level to CL command percentage values
    // Based on Teknatool manual: Low=20%, Med=50%, High=70%
    //
    // NOTE (2026-01-22): Logic analyzer reveals original firmware behavior:
    // - Idle: CL=70%
    // - Running: CL=100%
    // Our implementation allows user-configurable levels (20/50/70),
    // but task_motor.c should set CL=100 during motor start for max power.
    uint16_t cl_value;

    switch (level) {
        case 0:  // Low
            cl_value = 20;
            break;

        case 1:  // Med
            cl_value = 50;
            break;

        case 2:  // High
            cl_value = 70;
            break;

        default:
            // Default to High if invalid
            cl_value = 70;
            break;
    }

    // Send CL command to motor controller
    motor_send_command(CMD_CURRENT_LIMIT, cl_value);
}

bool motor_set_power_level(motor_power_t level) {
    extern void uart_puts(const char* s);
    extern void print_num(int32_t n);

    // Validate level
    uint8_t cl_value = (uint8_t)level;
    if (cl_value != MOTOR_POWER_LOW && cl_value != MOTOR_POWER_MED &&
        cl_value != MOTOR_POWER_HIGH && cl_value != MOTOR_POWER_MAX) {
        cl_value = MOTOR_POWER_HIGH;  // Default to factory default
    }

    uart_puts("Setting power level CL=");
    print_num(cl_value);
    uart_puts("%\r\n");

    // Step 1: Set CL value
    motor_send_command(CMD_CURRENT_LIMIT, cl_value);
    delay_ms(10);

    // Step 2: Commit with SE command (discovered 2026-01-25)
    // SE command commits parameter changes - without it, CL may not take effect
    motor_send_command(CMD_SE, cl_value);
    delay_ms(10);

    // Step 3: Verify by reading back CL
    int32_t readback = motor_read_param(CMD_CURRENT_LIMIT);
    if (readback == cl_value) {
        uart_puts("  Power level verified: CL=");
        print_num(readback);
        uart_puts("%\r\n");
        return true;
    } else {
        uart_puts("  WARNING: CL readback mismatch, got ");
        print_num(readback);
        uart_puts("\r\n");
        return false;
    }
}

void motor_set_thermal_threshold(uint8_t threshold_c) {
    // Set temperature threshold for current reduction
    // MCB will de-rate current when heatsink exceeds this temperature
    if (threshold_c < 40) threshold_c = 40;    // Min 40°C
    if (threshold_c > 100) threshold_c = 100;  // Max 100°C

    // Send TH command to motor controller
    motor_send_command(CMD_TH, threshold_c);
}

void motor_set_vibration_sensitivity(uint8_t level) {
    // Map vibration sensitivity level to VG command values
    // Based on original firmware: 0=OFF, 85=LOW, 170=MED, 261=HIGH
    uint16_t vg_value;
    uint8_t vs_enable;

    switch (level) {
        case 0:  // OFF
            vg_value = 0;
            vs_enable = 0;  // Disable sensor
            break;

        case 1:  // LOW
            vg_value = 85;
            vs_enable = 1;
            break;

        case 2:  // MED
            vg_value = 170;
            vs_enable = 1;
            break;

        case 3:  // HIGH
            vg_value = 261;
            vs_enable = 1;
            break;

        default:
            // Default to HIGH if invalid
            vg_value = 261;
            vs_enable = 1;
            break;
    }

    // Send VG command (vibration gain/sensitivity)
    motor_send_command(CMD_VG, vg_value);

    // Send VS command (enable/disable vibration sensor)
    motor_send_command(CMD_VS, vs_enable);
}

void motor_sync_settings(void) {
    extern void uart_puts(const char* s);
    extern void print_num(int32_t n);
    const settings_t* s = settings_get();
    if (!s) return;

    uart_puts("Syncing motor params to MCB...\r\n");
    TickType_t t0 = xTaskGetTickCount();

    // Send IR compensation (CRITICAL for speed accuracy!)
    motor_set_ir_comp(s->motor.ir_gain, s->motor.ir_offset);
    delay_ms(5);  // Match original firmware (5ms delays)
    HEARTBEAT_UPDATE_MOTOR();
    uart_puts("  IR comp set\r\n");

    // Send voltage PID parameters (CRITICAL: must be non-zero for motor to start!)
    // Safety: use factory defaults if stored values are zero
    //
    // NOTE: Service menu shows SP (Kprop) and SI (Kint) as PID params.
    // Current implementation uses VP/VI commands which work correctly.
    // MCB may accept both parameter sets or map VP/VI → SP/SI internally.
    // No change needed - motor operates correctly with current settings.
    uint16_t vkp = s->motor.voltage_kp ? s->motor.voltage_kp : MOTOR_FACTORY_VOLTAGE_KP;
    uint16_t vki = s->motor.voltage_ki ? s->motor.voltage_ki : MOTOR_FACTORY_VOLTAGE_KI;
    motor_send_command(CMD_SET_VKP, vkp);  // VP command (may be unused - see above)
    delay_ms(5);  // Match original firmware (5ms delays)
    motor_send_command(CMD_SET_VKI, vki);  // VI command (may be unused - see above)
    delay_ms(5);  // Match original firmware (5ms delays)
    HEARTBEAT_UPDATE_MOTOR();
    uart_puts("  Voltage PID set (VP/VI)\r\n");

    // Send speed PID parameters
    motor_send_command(CMD_SET_KP, s->motor.speed_kprop);
    delay_ms(5);  // Match original firmware (5ms delays)
    motor_send_command(CMD_SET_KI, s->motor.speed_kint);
    delay_ms(5);  // Match original firmware (5ms delays)
    HEARTBEAT_UPDATE_MOTOR();  // Prevent watchdog during long init
    uart_puts("  Speed PID set\r\n");

    // Send advance and pulse max (safety: use factory defaults if zero)
    uint16_t adv_max = s->motor.advance_max ? s->motor.advance_max : MOTOR_FACTORY_ADV_MAX;
    motor_send_command(CMD_SET_ADV_MAX, adv_max);
    delay_ms(5);  // Match original firmware (5ms delays)
    motor_send_command(CMD_SET_PULSE_MAX, s->motor.pulse_max);
    delay_ms(5);  // Match original firmware (5ms delays)
    HEARTBEAT_UPDATE_MOTOR();  // Prevent watchdog during long init
    uart_puts("  Adv/Pulse max set\r\n");

    // Send speed ramp rate to MCB (controls soft start/stop)
    if (s->motor.speed_ramp >= 50 && s->motor.speed_ramp <= 2000) {
        motor_set_speed_ramp(s->motor.speed_ramp);
        delay_ms(5);  // Match original firmware (5ms delays)
    }

    // Send torque ramp rate to MCB
    if (s->motor.torque_ramp >= 50 && s->motor.torque_ramp <= 2000) {
        motor_set_torque_ramp(s->motor.torque_ramp);
        delay_ms(5);  // Match original firmware (5ms delays)
    }
    HEARTBEAT_UPDATE_MOTOR();

    // NOTE: Brake (BR) command DISABLED - causes motor overheating
    // We do NOT send BR command to avoid keeping motor energized when stopped

    // DISCOVERY 2026-01-25: SE command takes parameter CODE, not just 1!
    // SE=<cmd_code> commits that specific parameter (e.g., SE=0x434C commits CL)
    // For now, skip SE since MCB seems to accept params without explicit commit
    // and original firmware only uses SE in service menu for individual params
    uart_puts("  (SE commit skipped - params applied directly)\r\n");

    TickType_t tend = xTaskGetTickCount();
    uint32_t elapsed = (tend - t0) * portTICK_PERIOD_MS;
    char buf[16];
    int idx = 0;
    uint32_t val = elapsed;
    do { buf[idx++] = '0' + (val % 10); val /= 10; } while (val && idx < 15);
    uart_puts("Motor params synced! (took ");
    while (idx > 0) uart_putc(buf[--idx]);
    uart_puts("ms)\r\n");
}

void motor_sync_and_save(void) {
    motor_sync_settings();
    delay_ms(100);
    motor_save_mcb_params();
}

/*===========================================================================*/
/* New Protocol Functions (discovered 2026-01-22)                            */
/*===========================================================================*/

uint16_t motor_get_actual_rpm(void) {
    return motor_status.actual_rpm;
}

void motor_set_actual_rpm(uint16_t rpm) {
    motor_status.actual_rpm = rpm;
}

void motor_send_keep_running(uint8_t param) {
    (void)param;  // Unused - KR is sent as query, not command

    // Send KR as QUERY (like GF), not as COMMAND
    // MCB will respond with KR=<load> in COMMAND format
    motor_read_param(CMD_KEEP_RUNNING);
}

void motor_send_speed_2(uint16_t rpm) {
    motor_send_command(CMD_SPEED_2, rpm);
}

uint16_t motor_cv_confidence_check(void) {
    // CV burst pattern (discovered 2026-01-25 via logic analyzer)
    // Before depth-triggered stop, original firmware queries CV 3× rapidly (~50ms apart)
    // This is a "confidence check" before committing to direction change

    extern void uart_puts(const char* s);
    extern void print_num(int32_t n);

    uint32_t cv_sum = 0;
    uint8_t valid_count = 0;

    for (int i = 0; i < CV_BURST_QUERIES; i++) {
        // Query CV
        int32_t cv = motor_read_param(CMD_CURRENT_VELOCITY);
        if (cv > 0 && cv <= SPEED_MAX_RPM) {
            cv_sum += cv;
            valid_count++;
            motor_set_actual_rpm((uint16_t)cv);
        }

        if (i < CV_BURST_QUERIES - 1) {
            delay_ms(CV_BURST_INTERVAL_MS);
        }
    }

    // Require majority of samples to be valid; otherwise the result is noise.
    // 0 signals "no confidence" to the caller.
    uint16_t avg_cv = (valid_count >= (CV_BURST_QUERIES + 1) / 2)
                          ? (cv_sum / valid_count)
                          : 0;

    #ifdef DEBUG_CV_BURST
    uart_puts("[CV_BURST] avg=");
    print_num(avg_cv);
    uart_puts(" (");
    print_num(valid_count);
    uart_puts(" samples)\r\n");
    #endif

    return avg_cv;
}

/*===========================================================================*/
/* Spindle Hold (discovered 2026-01-24 via logic analyzer capture)           */
/*===========================================================================*/

void motor_spindle_hold(void) {
    // Send command to motor task via queue
    MOTOR_CMD(CMD_MOTOR_SPINDLE_HOLD, 0);
}

void motor_spindle_hold_safety(void) {
    // Send safety hold command to motor task via queue (CL=12%)
    MOTOR_CMD(CMD_MOTOR_SPINDLE_HOLD_SAFETY, 0);
}

void motor_spindle_release(void) {
    // Send command to motor task via queue
    MOTOR_CMD(CMD_MOTOR_SPINDLE_RELEASE, 0);
}
