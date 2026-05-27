/**
 * @file motor.c
 * @brief Motor Controller High-Level API
 *
 * MODULE: Motor Control Layer
 * LAYER: Application API (above protocol/UART layers)
 * THREAD SAFETY: motor_send_command() and motor_read_param() serialise
 * themselves on g_motor_mutex, which is RECURSIVE so that callers holding it
 * across a multi-command sequence (task_tapping.c) do not deadlock.
 *
 * The header used to claim "All functions protected by g_motor_mutex" while
 * motor_send_command() took no lock at all, and motor_read_param() filled the
 * shared tx_buffer BEFORE acquiring it. Two mutex-respecting callers could
 * still corrupt each other: task A builds its query, blocks on the mutex, task
 * B overwrites tx_buffer and transmits, then A transmits B's bytes and
 * attributes the reply to A's register. Fixed 2026-08-30.
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
#include "safety.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

/* The motor UART is shared by the motor, UI, tapping and console tasks. These
 * helpers are scheduler-aware because motor_* functions also run during init,
 * before vTaskStartScheduler(), where taking a mutex is illegal. */
static inline bool motor_uart_lock(void) {
    extern SemaphoreHandle_t g_motor_mutex;
    if (g_motor_mutex && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreTakeRecursive(g_motor_mutex, portMAX_DELAY);
        return true;
    }
    return false;
}

static inline void motor_uart_unlock(bool taken) {
    extern SemaphoreHandle_t g_motor_mutex;
    if (taken) {
        xSemaphoreGiveRecursive(g_motor_mutex);
    }
}

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
/* AUDIT FIX (HIGH, motor.c:120): this wrote the file-static tx_buffer and
 * drove USART3 with no lock at all. Concrete failure: adjusting
 * Sensor > Overload calls motor_send_command(CMD_LD, ...) from the UI task
 * (priority 2); the motor task (priority 4) preempts mid-packet and transmits
 * its own GF/SV/CV query. The frames interleave byte-for-byte, the setting is
 * silently never applied, and the motor task's wait_response() times out —
 * 15 of which trip the COMM FAULT hardware cutoff (task_motor.c:816).
 *
 * The buffer fill and the transmit must be inside the same critical region:
 * locking only the transmit still lets another task overwrite tx_buffer
 * between build and send. */
bool motor_send_command(uint16_t cmd, int16_t param) {
    const bool locked = motor_uart_lock();

    size_t len = protocol_build_command(cmd, param, tx_buffer);

    for (size_t i = 0; i < len; i++) {
        if (!motor_uart_send_byte(tx_buffer[i])) {
            last_error = MOTOR_ERR_UART_TX_TIMEOUT;
            motor_uart_unlock(locked);
            return false;  // TX timeout on byte send
        }
    }

    // motor_uart_send_byte() already handles TX complete
    last_error = MOTOR_OK;
    motor_uart_unlock(locked);
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
// Phase 6: parse_decimal replaced with protocol_parse_field from motor_protocol.c

/**
 * @brief Parse GF (Get Flags) response to extract status data
 * Response format (estimated from RE):
 *   [SOH][ADDR][STX][DATA...][ETX][XOR]
 * DATA is a single ASCII integer (the flags bitfield) on the GB1.7 MCB —
 * verified empirically by 419 GF samples across all motor states. The
 * speculative comma-separated multi-field format (speed,load,vibration,temp)
 * never appears; speed/load/temp are sourced from their dedicated queries
 * (CV/KR/T0) and vibration is unsupported by this MCB. See
 * docs/MOTOR_PROTOCOL.md.
 */
static void parse_gf_response(size_t len) {
    size_t data_start = 0;
    for (size_t i = 0; i < len; i++) {
        if (rx_buffer[i] == PROTO_STX) {
            data_start = i + 2;  // Skip STX and unit byte
            break;
        }
    }
    if (data_start == 0) return;

    // Locate ETX (end of payload) and parse the single field as the flags value.
    for (size_t i = data_start; i < len; i++) {
        if (rx_buffer[i] == PROTO_ETX) {
            int16_t value = protocol_parse_field(rx_buffer, data_start, i - data_start);
            motor_status.raw_flags = (uint16_t)value;
            motor_status.fault = (value & 0x01) != 0;
            motor_status.overload = (value & 0x02) != 0;
            motor_status.jam_detected = (value & 0x04) != 0;
            motor_status.rps_error = (value & 0x18) != 0;
            motor_status.pfc_fault = (value & 0x20) != 0;
            motor_status.voltage_error = (value & 0xC0) != 0;
            motor_status.overheat = (value & 0x300) != 0;
            return;
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

const motor_status_t* motor_get_status(void) {
    return &motor_status;
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

// motor_hardware_is_enabled removed — zero callers

/*===========================================================================*/
/* Motor Status Accessors                                                    */
/*===========================================================================*/

// AUDIT NOTE (BUG 2 from /tmp/firmware_fixes_handoff.md, deliberately not
// actioned per user decision): motor_status.vibration is currently written
// by nothing (the old GF multi-field parser case 3 was dead code, removed
// 2026-05-30 during the BUG 1 collapse). This getter therefore returns 0
// forever, so the jam.c vibration-detection path is dormant. The API
// surface (this function + the enum + the vibration-sensitivity setting +
// the Sensor menu VibSen entry) is kept intact because:
//   - The OEM ships a full "Vibration Sensor / Sensitivity" UI (verified in
//     the R2P06K disasm strings) and we may still figure out how it's fed.
//   - Candidates: an unenumerated HMI ADC pin, an MCB register we haven't
//     ID'd, or a computed-from-KR-variance signal. See memory
//     [[vibration-sensitivity]] / vibration-sensor-status.md for the full
//     investigation and the motor_test::cmd_mcbvar experiment that would
//     find the source next time the OEM firmware is on the bench.
/* ALWAYS RETURNS 0. Nothing anywhere assigns motor_status.vibration — grep
 * finds reads only — so every consumer of this getter is dead code, including
 * the vibration jam detector in jam.c. Read that as "no coverage", not "no
 * events".
 *
 * Investigated 2026-09-05 against the original firmware's disassembly, jointly
 * with the session that holds the reverse-engineering work. What is
 * established:
 *   - The OEM firmware performs ZERO ADC reads and ZERO GPIO input reads, so
 *     nothing is sensed MCU-side.
 *   - A full scan of all 98 MCB registers found 7 that ever change; none is
 *     vibration.
 *   - The OEM's "Vibration Sensor" menu (DISABLED/LOW/MED/HIGH, default
 *     DISABLED) switches on the level and only calls display functions.
 *   - No command anywhere in the OEM image writes a 0-3 sensitivity, after a
 *     targeted search that did find every other command family.
 *   - VR/VS/VG/V8 are NOT vibration commands, whatever their names suggest —
 *     they are the brake/hold subsystem (see spindle_hold.c; V8=264 and VG=261
 *     are the hold-mode constants). An earlier analysis mistook them for
 *     vibration and concluded from their behaviour that vibration was
 *     unsupported. That reasoning was void; the conclusion may still be right.
 *
 * A hypothesis that the OEM feature was itself inert fitted all of the above
 * and was WRONG. The operator reports having actually triggered the "Excess
 * Vibration" error on the original firmware, with the sensor enabled. So the
 * feature works, real data reaches the comparison at 0x80095ac, and the source
 * exists — we simply have not found it.
 *
 * Worth recording how that hypothesis nearly stuck: this session proposed it,
 * the session holding the disassembly agreed, and two agents agreeing felt
 * like confirmation. It was not — it was one inference endorsed twice. A
 * single sentence from the person who owns the machine overturned it. Note
 * also that the "zero ADC reads, zero GPIO reads" result came from a Ghidra
 * pass that separately reported zero cross-references for strings that
 * demonstrably have them, so that negative is not trustworthy either and the
 * MCU-pin route is NOT excluded.
 *
 * Still open, both routes live:
 *   - an MCB register or command that carries the reading, or
 *   - an MCU pin the OEM reads by some path the disassembly pass missed.
 * The decisive trace remains the two variables behind 0x80098e0/0x80098e4.
 *
 * Left in place deliberately. Do not delete this as dead code — the feature
 * demonstrably works on the original firmware — and do not treat the detector
 * as protection until something actually assigns motor_status.vibration. */
uint16_t motor_get_vibration(void) {
    return motor_status.vibration;
}

// motor_set_pid, motor_set_current_limit removed — zero callers
// PID is set via motor_sync_settings, current limit via CL command directly

void motor_set_ir_comp(int16_t ir_gain, int16_t ir_offset) {
    // Send IR gain and offset as separate 16-bit values
    // Phase 4.1: Factory defaults now in config.h (MOTOR_FACTORY_IR_GAIN/OFFSET)
    motor_send_command(CMD_SET_IR_GAIN, ir_gain);
    motor_send_command(CMD_SET_IR_OFFSET, ir_offset);
}

// motor_set_pulse_max, motor_set_advance_max, motor_restore_mcb_defaults removed
// These were individual setters superseded by motor_sync_settings

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
static int32_t parse_param_response(size_t len, uint16_t expected_cmd) {
    /* AUDIT FIX (HIGH, motor_protocol.c:181): protocol_validate_response()
     * used to check request framing against a response and so returned 0 for
     * every real reply — this function always returned -1. It now returns the
     * data offset directly (past STX, unit and the command echo), so the
     * hand-rolled "+2 to skip the command echo" that used to live here is gone;
     * it was compensating for a header split that no longer exists. */
    size_t value_start = protocol_validate_response(rx_buffer, len, expected_cmd);
    if (value_start == 0) return -1;  // Not a valid response to this query

    /* AUDIT FIX (HIGH, motor.c:409): `len - value_start` is size_t. With the
     * validator permanently failing, this line was unreachable; the moment the
     * validator started working, a short frame would have underflowed it to
     * ~SIZE_MAX and walked protocol_parse_field() off the end of rx_buffer.
     * The validator guarantees len > value_start, but the subtraction is one
     * refactor away from being wrong again, so state the invariant here. */
    if (value_start >= len) return -1;

    return protocol_parse_field(rx_buffer, value_start, len - value_start);
}

// UART functions provided by motor_uart.c module

int32_t motor_read_param(uint16_t cmd) {
    /* AUDIT FIX (HIGH, motor.c:120): the rx_buffer memset and the tx_buffer
     * fill used to happen BEFORE the mutex was taken. Two callers that both
     * respected the mutex could still corrupt each other — task A built its
     * query, blocked on the lock, task B overwrote tx_buffer and sent, then A
     * woke and transmitted B's bytes, attributing B's reply to A's register.
     * Everything that touches the shared buffers is now inside the lock.
     *
     * Note this function still competes with task_motor's own RX draining for
     * bytes on the same USART; the mutex serialises the transmit and the read
     * window, and protocol_validate_response()'s command-echo check rejects a
     * frame that belongs to someone else rather than parsing it as ours. */
    const bool mutex_taken = motor_uart_lock();

    memset(rx_buffer, 0, sizeof(rx_buffer));
    /* Discard anything the MCB left in the receiver before asking a question,
     * exactly as task_motor.c::send_query() does — otherwise a stale byte is
     * parsed as the first byte of our reply. */
    motor_uart_flush_rx();
    size_t len = protocol_build_query(cmd, tx_buffer);

    // Send query with timeout protection (Phase 1.1)
    // Send packet using motor_uart layer
    if (!motor_uart_send_packet(tx_buffer, len)) {
        motor_uart_unlock(mutex_taken);
        extern void uart_puts(const char* s);
        uart_puts("[MOTOR] TX timeout in motor_read_param\r\n");
        return -1;  // TX timeout
    }

    /* AUDIT FIX (HIGH, motor.c:455): this loop used to delay_ms(2) whenever no
     * byte was ready. USE_USART3_DMA is not defined, so motor_uart_rx_available()
     * is a bare RXNE poll with no buffering whatsoever — a byte survives for one
     * character time, ~1.04 ms at 9600 baud. Sleeping 2 ms between polls
     * therefore overran the USART on the second byte of every reply, and the
     * ORE-clearing path in motor_uart_rx_available() discarded what was in DR.
     * Measured on the machine 2026-08-30, after the framing and locking fixes:
     * every console read still reported exactly "Received 1 bytes".
     *
     * task_motor.c::wait_response() has always polled tightly for precisely
     * this reason ("don't yield between bytes at 9600 baud to avoid USART
     * overrun"); this function did not. Same structure now: poll continuously,
     * stop at ETX, yield only occasionally so the watchdog stays fed. */
    size_t idx = 0;
    TickType_t read_start = xTaskGetTickCount();
    TickType_t read_timeout_ticks = pdMS_TO_TICKS(MOTOR_RESPONSE_TIMEOUT_MS);
    uint16_t poll_count = 0;

    while (idx < sizeof(rx_buffer)) {
        if ((xTaskGetTickCount() - read_start) >= read_timeout_ticks) {
            break;  // Overall timeout
        }

        if (motor_uart_rx_available()) {
            uint8_t byte = motor_uart_read_byte();
            rx_buffer[idx++] = byte;
            if (byte == PROTO_ETX) {
                break;  // Frame complete
            }
            poll_count = 0;
            continue;   // next byte is ~1 ms away — do not sleep
        }

        /* Nothing ready. Spin, yielding rarely: one tick of vTaskDelay is
         * already longer than a character time, so it can only be afforded
         * while the line is idle. */
        if (++poll_count >= 200) {
            poll_count = 0;
            vTaskDelay(1);
        }
    }

    /* REVIEW FIX (MEDIUM): the unlock used to sit here, above the parse — and
     * parse_param_response() reads the file-static rx_buffer, breaking the
     * invariant stated at the top of this function ("everything that touches
     * the shared buffers is now inside the lock"). The UI task (prio 2)
     * unlocks; task_motor (prio 4), already blocked on g_motor_mutex, takes it,
     * preempts, and memsets rx_buffer — the UI task then parses zeros, gets -1,
     * and reports a "CL readback mismatch" for a write that succeeded, or
     * resumes a little later and reads someone else's value. The mutex is
     * recursive, so parsing inside it is free. */
    if (idx == 0) {
        motor_uart_unlock(mutex_taken);
        return -1;  // No response
    }

    const int32_t value = parse_param_response(idx, cmd);
    motor_uart_unlock(mutex_taken);
    return value;
}

bool motor_read_mcb_params(mcb_params_t* params) {
    if (params == NULL) return false;
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

    /* Step 2: Commit with SE (discovered 2026-01-25).
     *
     * REVIEW FIX (HIGH): the parameter was cl_value — a PERCENTAGE — where SE
     * takes the COMMAND CODE of the parameter being committed. Every other SE
     * site passes a code (commands_motor.c: `motor_send_command(CMD_SE, CMD_I3)`
     * "SE with I3's command code", and the param_code path built as (h<<8)|l).
     * Sending SE=70 named parameter code 0x0046, not CL, so the commit did not
     * commit CL — which is exactly why this function then reports "CL readback
     * mismatch". */
    motor_send_command(CMD_SE, CMD_CURRENT_LIMIT);
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
    uint8_t sync_failures = 0;

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
    if (!motor_send_command(CMD_SET_VKP, vkp)) sync_failures++;
    delay_ms(5);
    if (!motor_send_command(CMD_SET_VKI, vki)) sync_failures++;
    delay_ms(5);
    HEARTBEAT_UPDATE_MOTOR();
    uart_puts("  Voltage PID set (VP/VI)\r\n");

    // Send speed PID parameters
    if (!motor_send_command(CMD_SET_KP, s->motor.speed_kprop)) sync_failures++;
    delay_ms(5);
    if (!motor_send_command(CMD_SET_KI, s->motor.speed_kint)) sync_failures++;
    delay_ms(5);
    HEARTBEAT_UPDATE_MOTOR();  // Prevent watchdog during long init
    uart_puts("  Speed PID set\r\n");

    // Send advance and pulse max (safety: use factory defaults if zero)
    uint16_t adv_max = s->motor.advance_max ? s->motor.advance_max : MOTOR_FACTORY_ADV_MAX;
    if (!motor_send_command(CMD_SET_ADV_MAX, adv_max)) sync_failures++;
    delay_ms(5);
    if (!motor_send_command(CMD_SET_PULSE_MAX, s->motor.pulse_max)) sync_failures++;
    delay_ms(5);
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

    // Send overload threshold (LD register)
    if (s->sensor.overload_threshold >= 10 && s->sensor.overload_threshold <= 100) {
        if (!motor_send_command(CMD_LD, s->sensor.overload_threshold)) sync_failures++;
        delay_ms(5);
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
    uart_puts("ms)");
    if (sync_failures > 0) {
        uart_puts(" WARNING: ");
        print_num(sync_failures);
        uart_puts(" commands failed!");
    }
    uart_puts("\r\n");
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
#ifdef BUILD_DEBUG
    /* Load-slip detects a CV overshoot against the commanded baseline, so
     * exercising it needs a fake ACTUAL rpm, not a fake load. */
    if (g_state.sim_cv_active) {
        return g_state.sim_cv;
    }
#endif
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

/*===========================================================================*/
/* Sensor Alignment (verified via motor_test ALIGN command)                  */
/*===========================================================================*/

/* Alignment energizes the windings: motor_hardware_enable() re-drives PD4 --
 * the same line the E-Stop and guard EXTI ISRs drop (encoder.c:350, :375) --
 * and then applies CL=20%/VR=20 holding torque. Without a gate here, typing
 * ALIGN on the console defeats the hardware interlock. Found by code review
 * 2026-08-30; shipped that way in v0.1.0.
 *
 * The check lives in motor.c and not in cmd_align() deliberately. These
 * functions do not go through g_motor_cmd_queue, so task_motor.c's in-task
 * re-check never sees them. Putting the gate on the capability rather than on
 * one caller is what safety.h's "path 5 -- any future start entry point" note
 * asks for; a gate in cmd_align() would leave the next caller uncovered.
 *
 * motor_exit_align() is deliberately NOT gated: it de-energizes, and must work
 * under exactly the conditions refused here.
 *
 * @return false if refused -- caller should report safety_refusal_reason(). */
/* REVIEW FIX (HIGH): safety_can_start_motor() deliberately does NOT test
 * motor_running — it is the gate a START passes through, and cmd_start sets
 * motor_running before queueing, so testing it there would refuse the very
 * start that set it. Alignment is the opposite case: it is only meaningful on
 * a stopped spindle, and applying a fixed commutation phase plus CL=20% to a
 * spindle turning at 2000 RPM is a way to destroy a workpiece or the drill.
 * ALIGN ships in release (flags 0), so this needs its own busy test. */
static bool align_machine_busy(void) {
    STATE_LOCK();
    const bool busy = g_state.motor_running ||
                      g_state.state == APP_STATE_DRILLING ||
                      g_state.state == APP_STATE_TAPPING;
    STATE_UNLOCK();
    return busy;
}

/* ALIGN holds the MCB in a manual commutation state until "ALIGN OFF", so it
 * takes the same scan claim MSYNC/MSAVE/MCBSCAN/REGSCAN take: without it
 * task_motor's 2 Hz GF/KR poll interleaves between the five parameter writes,
 * and nothing but the align state — which no start path checks — stands
 * between an energized winding and an ON press.
 *
 * The claim spans several console commands (ALIGN, ALIGN A|B|C, ALIGN OFF),
 * and our own claim sets motor_scan_mode, which safety_can_start_motor()
 * refuses on. So re-entering a claim we already hold skips that gate; every
 * other refusal — E-Stop, guard, ERROR, clock fault, flash, someone else's
 * claim — still applies, on the first entry, where it matters. */
static bool align_gate_ok(void) {
    if (align_machine_busy()) {
        return false;
    }
    /* REVIEW FIX (CRITICAL): the held-claim check used to sit HERE, returning
     * true before the safety gate was ever consulted — so a second ALIGN inside
     * an existing session skipped E-Stop, guard, ERROR state, clock fault and
     * brown-out entirely and re-energized the windings. My own comment claimed
     * the other refusals "still apply, on the first entry, where it matters":
     * that was simply wrong, because EVERY entry re-drives PD4 and re-applies
     * holding torque. The gate is consulted first, always; re-entrancy is now
     * handled inside it (safety.h) where it cannot be skipped. */
    if (!safety_can_start_motor()) {
        return false;
    }
    if (motor_scan_held_by_caller()) {
        return true;   /* our own session — claim already taken */
    }
    return motor_scan_try_claim() == MOTOR_SCAN_CLAIMED;
}

/* ALIGN energizes the windings at CL=20% and holds the scan claim — which
 * gates task_motor's ENTIRE poll block, so while a session is open the MCB is
 * not polled and all four jam detectors are dead. Nothing bounded it: if the
 * terminal disconnected, or the operator simply walked away, that state
 * persisted until someone typed ALIGN OFF or the machine was power cycled, with
 * every start refused as "MCB parameter write in progress" in the meantime.
 * spindle_hold has SAFETY_HOLD_TIMEOUT_MS for exactly this reason.
 *
 * 60 s is a chosen bound, not an OEM figure: long enough to step A/B/C and read
 * the sensors, short enough that energized windings and dead jam detection
 * cannot be left behind by accident. */
static TickType_t s_align_started = 0;
static bool s_align_active = false;

bool motor_align_timeout_expired(void) {
    if (!s_align_active) return false;
    return ((xTaskGetTickCount() - s_align_started) >=
            pdMS_TO_TICKS(ALIGN_SESSION_TIMEOUT_MS));
}

bool motor_enter_align(uint8_t phase) {
    if (!align_gate_ok()) {
        return false;
    }

    s_align_started = xTaskGetTickCount();
    s_align_active = true;
    motor_hardware_enable();
    motor_send_command(CMD_V8, 264);
    motor_send_command(CMD_VG, 261);
    motor_send_command(CMD_VS, phase);
    motor_send_command(CMD_CURRENT_LIMIT, 20);
    motor_send_command(CMD_VR, 20);
    return true;
}

/* Same gate as motor_enter_align(): this re-applies CL/VR holding torque, and
 * is reached from "ALIGN A|B|C" without ever entering align mode first. */
bool motor_set_align_phase(uint8_t phase) {
    if (!align_gate_ok()) {
        return false;
    }

    s_align_started = xTaskGetTickCount();
    s_align_active = true;
    motor_send_command(CMD_VR, 0);
    motor_send_command(CMD_CURRENT_LIMIT, 0);
    motor_send_command(CMD_VS, 0);
    motor_send_command(CMD_V8, 264);
    motor_send_command(CMD_VG, 261);
    motor_send_command(CMD_VS, phase);
    motor_send_command(CMD_CURRENT_LIMIT, 20);
    motor_send_command(CMD_VR, 20);
    return true;
}

/**
 * @brief Read GR (RPS sensor bitmask) using the COMMAND frame format.
 *
 * GR does not answer a QUERY frame. motor_read_param() builds
 * SOH + addr + '1' + cmd + ENQ via protocol_build_query(), and GR returns
 * nothing to it — which is why ALIGN has always printed "RPS:--- (no reply)".
 * It DOES answer a command frame (STX ... ETX + XOR), confirmed against the
 * original firmware's disassembly and measured here: "MQ GR" gets two bytes
 * back, a value byte followed by ACK.
 *
 * So the reply is not a normal parameter frame and parse_param_response()
 * cannot read it: there is no STX/ETX to validate, just <value><ACK>. Read the
 * bytes directly.
 *
 * @return 0..255 sensor byte, or -1 if the MCB said nothing.
 */
/* Spin budget per byte, matching serial_console.c::motor_read_resp(). */
#define MOTOR_READ_SPIN_LOOPS 100000u

int16_t motor_read_gr_ex(uint8_t* raw, size_t raw_cap, size_t* raw_len) {
    /* Use serial_console.c's motor_putc()/motor_read_resp() rather than the
     * motor_uart layer.
     *
     * Not a stylistic choice — a measured one. Four separate attempts with the
     * motor_uart primitives (stop-at-ACK removed, parameterless frame, tight
     * spin instead of yielding, same mutex) all stayed at roughly one
     * successful read in six, while "MQ GR" was 6 for 6 on the same register
     * seconds apart. The transmitted frames are byte-identical — I checked,
     * including the XOR: 0x31^0x47^0x52^0x03 = 0x27, matching MQ's capture. So
     * the difference lives in the TX/RX primitives, not the protocol, and I
     * could not tell you which one. Rather than ship a read that works 17% of
     * the time, use the pair that demonstrably works and say plainly that the
     * reason is not yet understood.
     *
     * Worth revisiting: whatever this is probably also affects other reads
     * built on the motor_uart layer. */
    extern void motor_putc(uint8_t c);
    extern int motor_read_resp(uint8_t* buf, int max_len);

    const bool mutex_taken = motor_uart_lock();

    memset(rx_buffer, 0, sizeof(rx_buffer));
    /* NO motor_uart_flush_rx() here — it is what made this read unreliable.
     *
     * Measured, and it is the single variable: with the flush, 1 successful
     * read in 8; without it, 8 in 8, on the same register seconds apart. MQ,
     * which has always worked, does not flush either. The mechanism is not
     * understood — plausibly the flush leaves the receiver in a state that
     * loses the first byte after the transmit.
     *
     * It does NOT follow that every flushing read is broken, and I checked
     * before claiming it did: motor_read_param() flushes on the same USART and
     * MREAD returned byte-identical values across four consecutive runs. The
     * difference is reply shape. A parameter read returns a full STX...ETX
     * frame and protocol_find_stx() resynchronises past a lost leading byte,
     * so the damage is invisible. GR answers with a bare two-byte
     * <value><ACK>: lose the value and only the ACK is left, which is fatal.
     * So the flush is survivable for framed replies and fatal for this one.
     *
     * A stale leading byte is handled in the parse below instead of by
     * flushing. */
    size_t len = protocol_build_read(CMD_GR, tx_buffer);
    for (size_t i = 0; i < len; i++) motor_putc(tx_buffer[i]);

    const int rlen = motor_read_resp(rx_buffer, (int)sizeof(rx_buffer));
    const size_t idx = (rlen > 0) ? (size_t)rlen : 0;

    /* Hand the caller what actually arrived. The read is intermittent on this
     * MCB and "no reply" alone cannot distinguish silence from a reply shaped
     * differently than expected — which is the whole question. */
    if (raw && raw_cap) {
        size_t n = (idx < raw_cap) ? idx : raw_cap;
        memcpy(raw, rx_buffer, n);
        if (raw_len) *raw_len = n;
    } else if (raw_len) {
        *raw_len = idx;
    }

    motor_uart_unlock(mutex_taken);

    /* The reply is <value><ACK>, so take the byte immediately BEFORE the ACK
     * rather than the first non-ACK byte. Without the flush a stale byte from
     * a previous exchange can lead the buffer — one read returned 106 against
     * a register that otherwise sits in the teens and twenties — and taking
     * the first byte would report that as the sensor value. A reply of ACK
     * alone is not a reading; say so rather than returning 6. */
    for (size_t i = idx; i-- > 0; ) {
        if (rx_buffer[i] == PROTO_ACK && i > 0) {
            return (int16_t)rx_buffer[i - 1];
        }
    }
    return -1;
}

int16_t motor_read_gr(void) {
    return motor_read_gr_ex(NULL, 0, NULL);
}

int8_t motor_read_align_sensors(void) {
    const int16_t v = motor_read_gr();
    return (v < 0) ? (int8_t)-1 : (int8_t)v;
}

void motor_exit_align(void) {
    s_align_active = false;

    motor_send_command(CMD_VR, 0);
    motor_send_command(CMD_CURRENT_LIMIT, 0);
    motor_send_command(CMD_VS, 0);
    motor_send_command(CMD_CURRENT_LIMIT, 100);
    motor_hardware_disable();
    motor_scan_release();   /* the claim align_gate_ok() took */
}
