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
#include "motor_load.h"
#include "safety.h"
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

/**
 * @brief Atomically claim the MCB parameter-write envelope.
 *
 * REVIEW FIX. motor_scan_mode was a bare bool with no owner, and three rounds
 * of bolting claim semantics onto it at the call sites produced three separate
 * defects:
 *
 *  - CLAIM-THEN-BACK-OUT. Callers set the flag, THEN took STATE_LOCK to decide
 *    whether they were allowed to. STATE_LOCK can block and yield, and the
 *    motor task runs at priority 4 against main's 1 and UI's 2 — so it could
 *    preempt in that window, dequeue a start or a depth auto-reverse, refuse it
 *    because the flag was up, and have the command silently consumed by a
 *    claimer that then backed out. Nothing retries a dropped auto-reverse.
 *  - NO OWNERSHIP ON RELEASE. A refusal path cleared the flag unconditionally,
 *    so the UI task tearing down its own attempt would end another task's
 *    ACTIVE envelope mid-write — resuming the 2 Hz poll into the middle of the
 *    parameter writes, which is the exact corruption the envelope prevents.
 *  - Two call sites claimed first and two checked first, inviting the next
 *    editor to assume one convention.
 *
 * Deciding and claiming inside ONE STATE_LOCK region fixes all three: the
 * decision is atomic against everything that commits start state (cmd_start,
 * handle_btn_start, the tapping task all take STATE_LOCK), the flag is never
 * raised unless the claim succeeds, and a failed claim has nothing to release.
 *
 * @return MOTOR_SCAN_CLAIMED if the envelope is now yours — call
 *         motor_scan_release() when done. MOTOR_SCAN_BUSY if the machine is
 *         mid-job, MOTOR_SCAN_HELD if another task already holds it. The two
 *         failures are distinguished because telling an operator to stop a
 *         spindle that is already stopped is worse than saying nothing.
 */
/* Who holds the envelope. REVIEW FIX: motor_scan_release() had no ownership
 * check, which is what let CMD_MOTOR_READ_PARAMS end another task's envelope
 * by clearing the flag unconditionally. A release from a task that does not
 * hold it is now a no-op, so that class cannot recur even if someone adds a
 * new call site. */
static TaskHandle_t s_scan_owner = NULL;

motor_scan_result_t motor_scan_try_claim(void) {
    motor_scan_result_t result;
    STATE_LOCK();
    /* motor_running alone is not "busy": task_depth.c:322 documents that it
     * reads false transiently between the STOP and REVERSE of a depth
     * auto-reverse. The app state stays DRILLING across that gap. */
    const bool busy = g_state.motor_running ||
                      g_state.state == APP_STATE_DRILLING ||
                      g_state.state == APP_STATE_TAPPING;
    /* REVIEW FIX: a failed claim used to be a single bool, so callers reported
     * every failure as "machine is running — STOP first". Two different things
     * fail here, and telling an operator to stop a spindle that is already
     * stopped is worse than saying nothing. */
    if (busy) {
        result = MOTOR_SCAN_BUSY;
    } else if (motor_scan_mode) {
        result = MOTOR_SCAN_HELD;
    } else {
        motor_scan_mode = true;
        s_scan_owner = xTaskGetCurrentTaskHandle();
        result = MOTOR_SCAN_CLAIMED;
    }
    STATE_UNLOCK();
    return result;
}

/* Re-claiming from the same task returns MOTOR_SCAN_HELD, and release is not
 * nested, so a caller that holds the claim across several console commands
 * (ALIGN, which stays live until "ALIGN OFF") has to be able to ask. */
bool motor_scan_held_by_caller(void) {
    STATE_LOCK();
    const bool mine = motor_scan_mode && s_scan_owner == xTaskGetCurrentTaskHandle();
    STATE_UNLOCK();
    return mine;
}

void motor_scan_release(void) {
    STATE_LOCK();
    if (s_scan_owner == xTaskGetCurrentTaskHandle()) {
        s_scan_owner = NULL;
        motor_scan_mode = false;
    }
    STATE_UNLOCK();
}

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
#define TEMP_SHUTDOWN_FLOOR    80  // Non-configurable safety floor (°C)

/*===========================================================================*/
/* Communication Timeout Constants (H5 safety fix)                            */
/*===========================================================================*/

#define MAX_COMM_FAILURES       15  // 15 consecutive failures triggers fault
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

// Timestamp of last status poll — bumped after speed commands to avoid collision
static TickType_t last_status_query = 0;

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

// Simple hex digit conversion (no snprintf). Only reached from DEBUG_PRINTC,
// which compiles to nothing in release builds.
static char hex_digit(uint8_t n) __attribute__((unused));
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
    for (size_t i = 0; i < len; i++) {
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
    // BUGFIX 2026-07-09: refresh jam's last-response timestamp on every
    // successful parse. Fixes the false JAM_COMM_TIMEOUT / "DRILL BIT JAM"
    // that fired on first drilling run after boot.
    jam_notify_response();
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
#ifdef BUILD_DEBUG
    /* Simulation owns motor_load while active. This has to sit here rather
     * than at the consumer: g_state.motor_load is republished from KR on every
     * poll, so a value written from the console would be overwritten
     * milliseconds later — the same way task_ui used to stamp on the simulated
     * pedal. Overriding at the single publisher means the load filter, the
     * baseline and all four jam detectors see the simulated value, which is
     * exactly the point of simulating it. */
    if (!g_state.sim_load_active) {
        g_state.motor_load = load;
    }
#else
    g_state.motor_load = load;
#endif
    STATE_UNLOCK();
    /* Always, even while simulating: this refreshes the jam comm-timeout, and
     * skipping it would trip JAM_COMM_TIMEOUT during a simulated run. */
    jam_notify_response();  // see update_cv_state
}

static void update_sv_state(uint16_t rpm) {
    /* REVIEW FIX (HIGH): this used to write the MCB's SV readback straight into
     * g_state.target_rpm — the OPERATOR's setpoint. The SV query runs every
     * MOTOR_STATUS_POLL_RUNNING_MS (50 ms) while the spindle turns, and the
     * encoder rate-limits its CMD_MOTOR_SET_SPEED, so the poll won: turning the
     * dial while running nudged the target and then had it reverted to the
     * MCB's older value before the command was ever sent. The operator's
     * intent is not the controller's to overwrite.
     *
     * SV is still worth asking — it tells us whether the MCB accepted what we
     * sent — so report a disagreement instead of adopting it. Rate-limited: a
     * mismatch persists across polls and would otherwise flood the console at
     * 20 Hz. */
    static uint16_t last_reported = 0;
    if (rpm != target_speed_local && rpm != last_reported) {
        last_reported = rpm;
        uart_puts("[SV] MCB reports ");
        print_num(rpm);
        uart_puts(" RPM, we commanded ");
        print_num(target_speed_local);
        uart_puts("\r\n");
    } else if (rpm == target_speed_local) {
        last_reported = 0;   /* agreement: re-arm the report */
    }
    jam_notify_response();  // see update_cv_state
}

/* AUDIT FIX (HIGH, task_motor.c:770): the CV swallow below was
 * unconditional, so the dedicated `send_query(CMD_GET_CV)` could never
 * succeed — its own reply was consumed inline and the wait ran to the full
 * 250 ms MOTOR_RESPONSE_TIMEOUT_MS every single poll. While drilling,
 * motor_query_status() therefore cost ~250 ms + 4x15 ms instead of the
 * designed MOTOR_STATUS_POLL_RUNNING_MS = 50, so KR load sampling ran at
 * ~3 Hz. The EMA whose comment claims "~100 ms time constant at 20 Hz"
 * (motor_load.c:14) actually had a ~2.5 s one: the load bar and the spike/step
 * jam detectors lagged real load by seconds, and queued STOP/speed commands
 * waited ~300 ms.
 *
 * @param expected_cmd  The command whose reply we are waiting for, so a frame
 *                      that IS that reply is returned to the caller rather
 *                      than being eaten as an unsolicited update. Pass 0 when
 *                      the caller does not care (every non-CV query today —
 *                      unsolicited CV frames still get absorbed for them,
 *                      which is the behaviour that was wanted all along). */
/* Spin iterations between yields while waiting for a reply. The body is a bare
 * register poll, so this is a spin BOUND, not a millisecond count — matched to
 * motor.c's motor_read_param(), which uses the same shape for the same reason. */
#define MOTOR_RX_SPINS_PER_YIELD 200

static bool wait_response_for(uint32_t timeout_ms, uint16_t expected_cmd) {
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    rx_index = 0;
    memset(rx_buffer, 0, sizeof(rx_buffer));
    bool found_etx = false;
    uint16_t poll_count = 0;    /* per-call, not static — see the yield below */

    while ((xTaskGetTickCount() - start) < timeout) {
        if (motor_uart_rx_available()) {
            uint8_t b = motor_uart_read_byte();
            poll_count = 0;   /* a byte arrived: the line is live, keep polling */
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

                        const bool is_expected =
                            expected_cmd != 0 &&
                            rx_buffer[i + 2] == (uint8_t)(expected_cmd >> 8) &&
                            rx_buffer[i + 3] == (uint8_t)(expected_cmd & 0xFF);

                        // Absorb an UNSOLICITED CV response (Current Velocity).
                        // If CV is what this caller queried, fall through and
                        // hand the frame back instead.
                        if (!is_expected &&
                            rx_buffer[i + 2] == 'C' && rx_buffer[i + 3] == 'V') {
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

                        /* REVIEW FIX (HIGH): a caller that named its command
                         * used to be handed ANY completed frame, and
                         * protocol_parse_and_validate() checks only the STX —
                         * never the two echo bytes. One timed-out query
                         * (the GF wait times out routinely; the code counts on
                         * it) put every later reply one slot early: a late GF
                         * of 34 became "target 34 RPM", the real SV was eaten
                         * by the CV wait, and a CV of 80 RPM became
                         * motor_load = 80%, which feeds jam detection and can
                         * trip JAM_LOAD_SPIKE into an emergency stop mid-cut.
                         * Discard a reply to a different command and keep
                         * waiting for ours. */
                        if (expected_cmd != 0 && !is_expected) {
                            rx_index = 0;
                            memset(rx_buffer, 0, sizeof(rx_buffer));
                            found_etx = false;
                            break;
                        }

                        // KR responses are parsed by the caller, not inline.
                        // (Inline parse consumed the buffer and caused timeout.)
                    }
                }

                // If still found_etx (wasn't a CV response), return success
                if (found_etx) {
                    diagnostics_mcb_comm(true);
                    return true;
                }
            }
        }
        /* Tight poll — don't sleep between bytes at 9600 baud (~1 ms/byte) or
         * the single-byte RDR overruns.
         *
         * REVIEW FIX (MEDIUM): the counter was incremented once per LOOP
         * ITERATION with a bare register poll in the body (tens of nanoseconds
         * at 120 MHz), so "yield every 10 ms" was really "poll ten times in
         * under a microsecond, then sleep a whole tick" — landing the effective
         * poll interval right on the 1.04 ms character time with no margin.
         * A wake one character late overruns the RDR, and
         * motor_uart_rx_available() reports ORE as "available", so the lost byte
         * silently shortens a field: a CV of "1250" comes back as 150 and still
         * passes the 0..SPEED_MAX_RPM range check.
         *
         * motor.c's motor_read_param() documents this exact hazard and fixed it
         * with a 200-iteration bound plus a reset on every byte received;
         * neither was carried over here. The counter was also `static`, so a
         * fresh call could enter at 9 and sleep before its first poll. */
        if (++poll_count >= MOTOR_RX_SPINS_PER_YIELD) {
            poll_count = 0;
            vTaskDelay(1);
        }
    }

    /* REVIEW FIX: diagnostics_mcb_comm() was declared, defined, and called from
     * nowhere — so STATS' "MCB Communication" block reported 0/0/0 for the life
     * of the firmware, and diagnostics.c divided by that zero. Every MCB query
     * in the system ends up here, success or timeout, so this is the one place
     * that knows. */
    diagnostics_mcb_comm(false);
    return false;  // Timeout — partial data is not a valid response
}

/* Most callers do not care which reply arrives; they only ever have one query
 * outstanding. They keep the old behaviour, including absorbing unsolicited CV
 * frames while they wait. */
static inline bool wait_response(uint32_t timeout_ms) {
    return wait_response_for(timeout_ms, 0);
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
    /* REVIEW FIX (MEDIUM): this used a bare wait_response() with hand-rolled STX
     * scanning and no command-echo check, unlike the gated
     * wait_response_for(..., CMD_SET_SPEED) used elsewhere — and validated only
     * `> 0 && <= SPEED_MAX_RPM`, with no SPEED_MIN_RPM floor. So a late KR load
     * reply of, say, 34 was accepted as an SV value, written BACK to the MCB as
     * the speed setting, and latched into target_speed_local: a stop could
     * silently reprogram the machine to 34 RPM. Ask for SV and insist on SV. */
    if (wait_response_for(MOTOR_RESPONSE_TIMEOUT_MS, CMD_SET_SPEED)) {
        size_t offset = protocol_find_stx(rx_buffer, rx_index, 3);
        if (offset != SIZE_MAX) {
            int16_t mcb_speed;
            if (protocol_parse_and_validate(rx_buffer, offset, rx_index,
                                            SPEED_MIN_RPM, SPEED_MAX_RPM,
                                            &mcb_speed)) {
                // Confirm speed setting back to MCB
                send_command(CMD_SET_SPEED, mcb_speed);  // Ignore return
                delay_ms(5);
                target_speed_local = (uint16_t)mcb_speed;  // Update local tracking
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
    g_state.current_rpm = 0;
    STATE_UNLOCK();
    jam_motor_stopped();
    motor_load_motor_stopped();
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
    g_state.current_rpm = 0;
    STATE_UNLOCK();
    jam_motor_stopped();
    motor_load_motor_stopped();

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

    // Poll GF until bit 2 clears — MCB signals commutation switch complete.
    // AUDIT FIX (HIGH, task_motor.c:461): the loop's real worst case with a
    // silent MCB is 20×(250 + 10) = 5.2 s — not the "Max ~200ms" the stale
    // comment claimed. That exceeded HEARTBEAT_TIMEOUT_MS (2 s), so main
    // stopped feeding the IWDG and the whole HMI hard-reset instead of
    // raising the designed COMM FAULT. Fix: feed the motor-task heartbeat
    // every iteration, and bail out after a few consecutive timeouts so we
    // let the main task handle it via COMM FAULT.
    uint8_t direction_timeouts = 0;
    for (int i = 0; i < 20; i++) {
        HEARTBEAT_UPDATE_MOTOR();
        send_query(CMD_GET_FLAGS);
        if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
            direction_timeouts = 0;
            size_t off = protocol_find_stx(rx_buffer, rx_index, 3);
            if (off != SIZE_MAX) {
                int16_t flags;
                if (protocol_parse_and_validate(rx_buffer, off, rx_index, 0, 30000, &flags)) {
                    if (!(flags & GF_DIR_CHANGE_BIT)) break;
                }
            }
        } else {
            if (++direction_timeouts >= 3) {
                // MCB is silent — abort the poll loop and let motor_query_status
                // take the COMM FAULT path (consecutive_comm_failures counter).
                uart_puts("[DIR] MCB silent — aborting GF poll\r\n");
                break;
            }
        }
        delay_ms(10);
    }

    STATE_LOCK();
    g_state.motor_forward = forward;
    STATE_UNLOCK();
}

/* Refusal message plus the state repair every refusal path owes the UI.
 * Shared by the command-dequeue gate and local_motor_start() so the two cannot
 * drift — they did, and the outer one skipped the repair entirely. */
static void start_refused(void) {
    uart_puts("[START] refused: ");
    uart_puts(safety_refusal_reason());
    uart_puts("\r\n");

    /* ...and SAY SO ON THE PANEL. This used to be console-only, so an operator
     * standing at the machine pressed ON and got silence — see
     * safety_refusal_lcd(). The guard, E-Stop, ERROR and clock-fault causes
     * have persistent screens of their own that display_update() renders before
     * this transient one, so those are unaffected; what this adds is the three
     * that had no screen at all: brown-out, settings save, MCB write. */
    const char* l1;
    const char* l2;
    safety_refusal_lcd(&l1, &l2);

    // Snap state back so UI doesn't lie about DRILLING.
    STATE_LOCK();
    if (g_state.state == APP_STATE_DRILLING || g_state.state == APP_STATE_TAPPING) {
        g_state.state = APP_STATE_IDLE;
    }
    g_state.motor_running = false;
    g_state.error_until = HAL_GetTick() + START_REFUSED_DISPLAY_MS;
    g_state.error_line1 = l1;
    g_state.error_line2 = l2;
    STATE_UNLOCK();
}

static void local_motor_start(void) {
    // AUDIT FIX (CRITICAL, task_motor.c:481): re-check interlock inside the
    // motor task, AFTER the command has been dequeued. This closes the race
    // where handle_btn_estop's xQueueReset arrives just after a start command
    // was already dequeued — without this check, we'd re-enable PD4 and spin
    // the spindle under an engaged E-Stop. Any refusal here is silent by
    // design; the caller has already committed and the user has direct signal
    // from the E-Stop button that the machine is disabled.
    if (!safety_can_start_motor()) {
        start_refused();
        return;
    }

    motor_hardware_enable();  // Enable hardware BEFORE sending command

    /* REVIEW FIX (CRITICAL): the gate above is a point-in-time check, and this
     * function then yields three times before the spindle is actually
     * commanded — delay_ms() is vTaskDelay, and this task runs at priority 4
     * while handle_btn_estop() runs in task_main at priority 1, so those
     * delays are exactly when it gets scheduled. It does xQueueReset (too late,
     * our command is already dequeued) and then deliberately re-drives PD4 for
     * the safety spindle hold — so without a re-check we resumed and sent ST
     * and SV into an ENABLED MCB with the E-Stop engaged, then overwrote the
     * handler's motor_running = false with true.
     *
     * Re-check after every yield. Anything already sent is undone by dropping
     * the enable line and issuing a stop, so a refusal mid-sequence leaves the
     * machine where the E-Stop wanted it. */
    #define START_ABORT_IF_UNSAFE()                                    \
        do {                                                           \
            if (!safety_can_start_motor()) {                           \
                motor_hardware_disable();                              \
                (void)send_command(CMD_STOP, 0);                       \
                start_refused();                                       \
                return;                                                \
            }                                                          \
        } while (0)

    // Set CL=100% for full power when running
    // Original firmware: CL=70% idle → CL=100% running
    if (!send_command(CMD_CURRENT_LIMIT, CL_RUNNING_PERCENT)) {
        uart_puts("[START] CL command timeout\r\n");
    }
    delay_ms(20);  // Let CL command complete
    START_ABORT_IF_UNSAFE();

    /* SPEED BEFORE START.
     *
     * REPORTED AND REPRODUCED 2026-08-30: with the previous speed at 2000, stop,
     * dial 1000, press ON — the spindle flares up toward 2000 for roughly half a
     * second before dropping back to 1000. The cause was this sequence sending
     * ST (start) BEFORE SV (speed): the MCB begins ramping toward the target it
     * already holds and only learns the new one ~20-40 ms later. It reliably
     * reaches for the OLD number because local_motor_stop() writes the last
     * speed into S2, the MCB's fallback register.
     *
     * Telling the controller what speed to run before telling it to run removes
     * the flare. SV is accepted while stopped — it sets the target register, and
     * local_motor_set_speed() already sends it independently of start/stop. */
    if (!send_command(CMD_SET_SPEED, target_speed_local)) {
        uart_puts("[START] SV command timeout\r\n");
    }
    delay_ms(20);  // Let SV command complete
    START_ABORT_IF_UNSAFE();

    // Send ST (start) command - critical for motor start
    if (!send_command(CMD_START, 0)) {
        uart_puts("[START] ST command timeout - start may fail\r\n");
    }
    delay_ms(20);  // Let ST command complete
    START_ABORT_IF_UNSAFE();
    #undef START_ABORT_IF_UNSAFE

    // Flush any responses
    motor_uart_flush_rx();

    motor_enabled = true;

    STATE_LOCK();
    g_state.motor_running = true;
    STATE_UNLOCK();

    // Notify jam detector so the spike-grace period and startup-timeout monitor
    // both arm. Pass target RPM so the inrush grace scales with the ramp
    // length (higher target → longer ramp → longer grace).
    jam_motor_started();
    motor_load_motor_started(target_speed_local);
}

static void local_motor_set_speed(uint16_t rpm) {
    if (rpm < SPEED_MIN_RPM) rpm = SPEED_MIN_RPM;
    if (rpm > SPEED_MAX_RPM) rpm = SPEED_MAX_RPM;

    uint16_t prev_target = target_speed_local;
    target_speed_local = rpm;

    if (motor_enabled) {
        motor_load_motor_speed_change(prev_target, rpm);
        // SV only while running — skip S2 (persistent fallback, only needed at idle).
        // Brief delay lets the MCB process the SV before the next status poll.
        send_command(CMD_SET_SPEED, rpm);
        delay_ms(10);
        motor_uart_flush_rx();
        /* REVIEW FIX (HIGH): `consecutive_comm_failures = 0` used to sit here —
         * after a send, a delay and a FLUSH, with no reply received and nothing
         * parsed. The invariant stated 70 lines below is the opposite: the
         * counter is cleared only once a status value has actually been parsed,
         * because clearing it disarms the MAX_COMM_FAILURES COMM FAULT cutoff
         * that drops PD4. With a dead MCB link, an operator adjusting the speed
         * — the natural reaction to a spindle behaving oddly — reset the
         * counter on every turn of the knob and suppressed the cutoff
         * indefinitely. Sending a command is not evidence of communication. */
        last_status_query = xTaskGetTickCount();
    } else {
        // Full handshake when idle — SV + S2 so the MCB remembers across resets
        if (!send_command(CMD_SET_SPEED, rpm)) {
            uart_puts("[SPEED] SV command timeout\r\n");
        }
        delay_ms(10);
        motor_uart_flush_rx();
        send_command(CMD_SPEED_2, rpm);
        delay_ms(5);
    }
}



/* Run every jam detector, once per poll, from BOTH polling branches.
 *
 * REVIEW FIX (CRITICAL): this block used to live inside motor_query_status(),
 * which the task loop calls only when `running && motor_enabled` — and
 * `running` is g_state.motor_running, which motor_query_status() itself sets
 * from the GF reply. So the two detectors that exist precisely for
 * "commanded but NOT running" — JAM_STARTUP_TIMEOUT and JAM_STALL_DETECTED —
 * could only ever be evaluated while motor_running was true. On a real stall
 * the MCB reports GF=32, motor_running goes false, jam_update() runs once more
 * at most (just arming stall_start_time), and from then on the loop takes the
 * idle branch: the 500 ms stall timer is never re-read. A bit binding in
 * hardwood produced no emergency stop and no "! MOTOR STALL !".
 *
 * Two rounds ago these moved out of the parsed-GF branch for the same reason
 * one level down. Detectors must not live inside a branch whose condition they
 * are supposed to be watching. */
static void run_jam_detectors(void) {
    const settings_t* js = settings_get();
    /* g_state.motor_load is the authoritative live KR — the GF reply carries
     * no load field; the KR query updates it one poll cycle behind. */
    motor_load_update(g_state.motor_load, g_state.current_rpm,
                      target_speed_local, motor_enabled);
    jam_load_update(motor_enabled, js->sensor.jam_detect,
                    js->sensor.spike_detect, js->sensor.spike_thresh,
                    js->sensor.step_thresh,
                    js->sensor.low_load_detect, js->sensor.low_load_thresh);
    jam_update(g_state.motor_running, motor_enabled);
}

static void motor_query_status(void) {
    // Query flags first
    send_query(CMD_GET_FLAGS);
    if (wait_response_for(MOTOR_RESPONSE_TIMEOUT_MS, CMD_GET_FLAGS)) {
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

        /* AUDIT FIX (HIGH, task_motor.c:690): this gate required only
         * offset+3 bytes while the data parse below needs offset+5, and
         * `flags` was initialised to 0 and then used as a real reading whether
         * or not anything was parsed. A truncated reply — [STX]['1'][ETX] —
         * passed the gate, reset consecutive_comm_failures (disarming the COMM
         * FAULT path), skipped the data block, and left flags == 0. Since
         * GF_MOTOR_RUNNING is 34, that publishes motor_running = false WHILE
         * THE SPINDLE IS TURNING: the UI shows stopped, the poll drops to 2 Hz,
         * and jam_update(false, true) enters stall detection, where two polls
         * 500 ms apart exceed JAM_STALL_TIMEOUT_MS and fire a spurious
         * "DRILL BIT JAM" emergency stop mid-cut.
         *
         * The frame is now validated properly — including the echoed command,
         * so a stray KR/SV reply is not read as a GF status — and the failure
         * counter is only cleared once a status value has actually been
         * parsed. */
        if (rx_index >= offset + 3 && rx_buffer[offset] == 0x02) {

            // Parse response data
            // Format: [STX][unit][cmd_H][cmd_L][data...][ETX]
            // Data starts at offset+4 (after STX, unit, cmd high, cmd low)
            // Code polish: Removed unused 'speed' and 'vib' variables (compiler warnings)
            // GF response is a single ASCII integer (32 stopped / 34 running,
            // or 16929+ with bit 14 set on error) — see docs/MOTOR_PROTOCOL.md
            // "GF Status Flags". Load lives in the separate KR query (cached
            // OEM-side at 0x2000002a). Direction is owned by the command-time
            // `direction_forward` flag, not derived from GF (older docs cited
            // 436/438 for reverse — verified absent on GB1.7).
            uint16_t flags = 0;
            bool flags_parsed = false;  /* nothing below may use flags until this is true */

            /* protocol_validate_response() re-checks STX and verifies the
             * command echo is GF, and returns the offset of the first data
             * byte. It returns 0 for a frame too short to hold one. */
            const size_t data_start =
                protocol_validate_response(rx_buffer, rx_index, CMD_GET_FLAGS);

            if (data_start != 0) {
                size_t field = 0;
                size_t field_start = data_start;
                for (size_t i = data_start; i < rx_index; i++) {
                    if (rx_buffer[i] == ',' || rx_buffer[i] == 0x03) {
                        const size_t field_len = i - field_start;
                        int16_t val = protocol_parse_field(rx_buffer, field_start, field_len);
                        /* An empty field (ETX immediately after the header) is
                         * not a reading of zero — it is a malformed frame. */
                        if (field == 0 && field_len > 0) {
                            flags = (uint16_t)val;
                            flags_parsed = true;
                        }
                        /* Fields 1-4 (speed/load/vib/temp) aren't sent by this
                         * MCB on GF — load comes from KR, others are unused. */
                        field++;
                        field_start = i + 1;
                        if (rx_buffer[i] == 0x03) break;
                    }
                }
            }

            if (!flags_parsed) {
                /* Truncated, unterminated or foreign frame. Treat it as the
                 * comm failure it is rather than as "motor stopped". */
                consecutive_comm_failures++;
            } else {
                consecutive_comm_failures = 0;

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



                // GF on GB1.7 reports stopped/running in bits 5+1 — direction is
                // NOT encoded here (verified across 419 samples, and again in
                // 2026-08-31 on-target runs where a sustained reverse reported
                // plain 34). Error states have
                // bit 14 set (0x4000), e.g. GF=16929+. Direction is owned by the
                // command-time `direction_forward` flag (set in local_motor_start_*
                // and mirrored into g_state.motor_forward there).
                /* Mask before comparing: GF sets extra status bits during a
                 * direction change (436/438 = the same stopped/running states
                 * with bits 2,4,7,8 also set). Whole-word equality read those
                 * as "unknown", which meant the guard below refused to publish
                 * motor_running at all and the flag went STALE on a spindle
                 * that was actually turning — see GF_STATE_MASK in config.h. */
                uint16_t gf_state = (uint16_t)flags & GF_STATE_MASK;
                bool known_good_state = (((uint16_t)flags & (uint16_t)~GF_KNOWN_BITS) == 0) &&
                                        (gf_state == GF_MOTOR_STOPPED ||
                                         gf_state == GF_MOTOR_RUNNING);
                bool error_state = (flags & 0x4000) != 0;

                /* REVIEW FIX (HIGH): known_good_state was computed here and
                 * then used only by the voltage logging below — the publish was
                 * unconditional. The idle branch got this guard (and a comment
                 * naming the consequence); the running branch, where it
                 * actually bites, did not. A GF value that is neither 32/34 nor
                 * an error code — the code already logs "Unknown GF state" for
                 * these — published motor_running = false on a TURNING spindle,
                 * and run_jam_detectors() then armed stall detection and fired
                 * a spurious emergency stop mid-cut one JAM_STALL_TIMEOUT_MS
                 * later. Only believe a reading we recognise. */
                STATE_LOCK();
                if (known_good_state || error_state) {
                    g_state.motor_fault = error_state;
                    g_state.motor_running = (gf_state == GF_MOTOR_RUNNING);
                }
                STATE_UNLOCK();
                jam_notify_response();  // BUGFIX 2026-07-09: refresh jam comm-timeout

                if (error_state) {
                    // Query F0 fault code for error screen
                    motor_uart_flush_rx();
                    send_query(CMD_F0);
                    if (wait_response(MOTOR_RESPONSE_TIMEOUT_MS)) {
                        size_t foff = protocol_find_stx(rx_buffer, rx_index, 3);
                        if (foff != SIZE_MAX) {
                            int16_t fc;
                            if (protocol_parse_and_validate(rx_buffer, foff, rx_index, 0, 200, &fc)) {
                                STATE_LOCK();
                                g_state.fault_code = (uint8_t)fc;
                                STATE_UNLOCK();
                            }
                        }
                    }
                    SEND_EVENT(EVT_MOTOR_FAULT);
                }

                // Voltage monitoring - only check on unknown/error states.
                // Known good states (32 stopped, 34 running) have various bits set
                // that are NOT error indicators. Only report voltage issues for
                // actual error states (bit 14 set).
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

            }  /* end: a status value was actually parsed */
        } else {
            // Got response but invalid format - count as failure (H5)
            consecutive_comm_failures++;
        }
    } else {
        // No response at all - communication failure (H5)
        consecutive_comm_failures++;
    }

    // SV query — confirm MCB accepted our target speed
    send_query(CMD_SET_SPEED);
    if (wait_response_for(MOTOR_RESPONSE_TIMEOUT_MS, CMD_SET_SPEED)) {
        size_t off = protocol_find_stx(rx_buffer, rx_index, 3);
        if (off != SIZE_MAX) {
            int16_t target_speed;
            if (protocol_parse_and_validate(rx_buffer, off, rx_index, 1, SPEED_MAX_RPM, &target_speed)) {
                update_sv_state((uint16_t)target_speed);
            }
        }
    }

    // CV query — actual motor speed (also arrives unsolicited, but query ensures freshness)
    send_query(CMD_GET_CV);
    if (wait_response_for(MOTOR_RESPONSE_TIMEOUT_MS, CMD_GET_CV)) {
        size_t off = protocol_find_stx(rx_buffer, rx_index, 3);
        if (off != SIZE_MAX) {
            int16_t actual_speed;
            if (protocol_parse_and_validate(rx_buffer, off, rx_index, 0, SPEED_MAX_RPM, &actual_speed)) {
                update_cv_state((uint16_t)actual_speed);
            }
        }
    }

    // KR query — motor load percentage
    send_query(CMD_KEEP_RUNNING);
    if (wait_response_for(MOTOR_RESPONSE_TIMEOUT_MS, CMD_KEEP_RUNNING)) {
        size_t off = protocol_find_stx(rx_buffer, rx_index, 3);
        if (off != SIZE_MAX) {
            int16_t load;
            if (protocol_parse_and_validate(rx_buffer, off, rx_index, 0, 100, &load)) {
                update_kr_state((uint8_t)load);
            }
        }
    }

    // AUDIT FIX (HIGH, task_motor.c:1161): T0 (MCB heatsink temperature) used
    // to be queried only in the idle polling branch, so the 80 °C overheat
    // shutdown and temp warnings were inoperative exactly while the motor was
    // running and generating heat. Now polled here as well, but throttled to
    // once per second (20 running-polls at 50 ms each) to keep UART load
    // predictable — MCB thermal mass is minutes, so 1 Hz is plenty.
    static uint8_t t0_throttle = 0;
    if (++t0_throttle >= 20) {
        t0_throttle = 0;
        motor_uart_flush_rx();
        send_query(CMD_T0);
        if (wait_response_for(MOTOR_RESPONSE_TIMEOUT_MS, CMD_T0)) {
            size_t toff = protocol_find_stx(rx_buffer, rx_index, 3);
            if (toff != SIZE_MAX) {
                int16_t mcb_temp;
                if (protocol_parse_and_validate(rx_buffer, toff, rx_index, 0, 150, &mcb_temp)) {
                    temp_monitor_update((uint16_t)mcb_temp, settings_get()->power.temp_threshold);
                }
            }
        }
    }

    // H5: Check for consecutive communication failures
    if (consecutive_comm_failures >= MAX_COMM_FAILURES) {
        // Motor controller not responding - hardware cutoff + fault
        uart_puts("COMM FAULT!\r\n");
        motor_hardware_disable();
        motor_enabled = false;

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
            /* AUDIT FIX (HIGH, motor.c:120), second half: this task OWNS the
             * motor UART but never took g_motor_mutex, so motor_read_param()
             * holding that mutex excluded the UI, console and tapping tasks
             * from each other and not from the one task that actually drives
             * the port. Measured on the machine 2026-08-30: a console `T0`
             * received exactly 1 byte of its reply — this task had drained the
             * rest — and reported -1. Locking here makes the mutex mean what
             * motor.c's header always claimed it meant.
             *
             * The lock is taken AFTER xQueueReceive so the blocking wait does
             * not hold it, and it spans the parse as well as the transaction,
             * because rx_buffer is shared. It is recursive, so the nested
             * motor_send_command() calls inside these handlers are fine. */
            MOTOR_CONTROL_LOCK();
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

                /* REVIEW FIX: the safety gate lives inside local_motor_start(),
                 * but local_motor_set_direction() runs FIRST and is not free —
                 * it sends JF and then polls GF up to 20 times. So a start
                 * refused for motor_scan_mode still injected a full JF plus up
                 * to twenty GF exchanges into the MCB in the middle of a
                 * parameter write, which is the second of the two hazards
                 * safety.h cites as the reason for that gate. Check before
                 * spending any UART. */
                case CMD_MOTOR_FORWARD:
                case CMD_MOTOR_TAP_FORWARD:
                case CMD_MOTOR_REVERSE:
                case CMD_MOTOR_TAP_REVERSE: {
                    /* REVIEW FIX: a bare `break` here skipped the state
                     * repair that local_motor_start() performs on refusal.
                     * Every producer commits state BEFORE enqueuing — events.c
                     * sets DRILLING, cmd_start sets DRILLING *and*
                     * motor_running — so refusing without the snap-back left
                     * the LCD showing DRILLING with the spindle stopped, and
                     * motor_running true re-armed depth and jam logic against a
                     * motor that never started. It also self-jammed the feature
                     * this gate exists for: with the state stuck at DRILLING,
                     * every busy check refuses MSYNC/MSAVE/menu-save forever. */
                    if (!safety_can_start_motor()) {
                        start_refused();
                        break;
                    }
                    const bool forward = (cmd.cmd == CMD_MOTOR_FORWARD ||
                                          cmd.cmd == CMD_MOTOR_TAP_FORWARD);
                    local_motor_set_direction(forward);
                    local_motor_start();   /* re-checks the gate in-task */
                    break;
                }

                case CMD_MOTOR_SET_SPEED: {
                    // Drain stale speed commands — only the latest RPM matters
                    motor_cmd_t peek;
                    while (xQueuePeek(g_motor_cmd_queue, &peek, 0) == pdTRUE &&
                           peek.cmd == CMD_MOTOR_SET_SPEED) {
                        xQueueReceive(g_motor_cmd_queue, &cmd, 0);
                    }
                    local_motor_set_speed(cmd.param);
                    break;
                }

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

                case CMD_MOTOR_APPLY_SETTINGS: {
                    const settings_t* s = settings_get();
                    if (s) {
                        motor_set_profile(s->motor.profile);
                        delay_ms(5);
                        motor_set_power_output(s->power.power_output);
                        delay_ms(5);
                    }
                    break;
                }

                case CMD_MOTOR_READ_PARAMS:
                    // Read all MCB parameters and store in shared state
                    {
                        /* This raised the flag by hand and cleared it
                         * unconditionally at the end, so an MREAD dequeued
                         * while the UI task was mid Save Settings ended THAT
                         * task's envelope, resuming the 2 Hz poll into the
                         * middle of its parameter writes.
                         *
                         * Claiming also means MREAD is now refused while the
                         * machine is drilling or tapping, which review flagged
                         * as a lost capability. It is deliberate: reading the
                         * nine MCB parameters holds the envelope for several
                         * hundred milliseconds, and the envelope suspends
                         * task_motor's whole poll block — where
                         * motor_load_update(), jam_load_update() and
                         * jam_update() live. A read-only diagnostic is not
                         * worth blinding all four jam detectors mid-cut. Read
                         * them with the spindle stopped. */
                        const motor_scan_result_t scan = motor_scan_try_claim();
                        if (scan != MOTOR_SCAN_CLAIMED) {
                            uart_puts("[MREAD] refused: ");
                            uart_puts(motor_scan_refusal(scan));
                            uart_puts("\r\n");
                            STATE_LOCK();
                            g_state.mcb_params.valid = false;
                            STATE_UNLOCK();
                            break;
                        }

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
                        motor_scan_release();
                    }
                    break;
            }
            MOTOR_CONTROL_UNLOCK();
        }

        /* Maintain spindle hold (periodic refresh + safety timeout).
         *
         * REVIEW FIX (MEDIUM): this sat OUTSIDE the motor_scan_mode gate that
         * the poll block below is behind. Envelope holders (menu Save Settings,
         * MSYNC/MSAVE) take and release g_motor_mutex per command, so this
         * slotted in between their parameter writes and spliced VR/CL/VS into
         * the middle of an MCB parameter-write sequence — including while
         * motor_save_mcb_params() sets the EEPROM commit flag, which is the
         * exact corruption the envelope exists to prevent. A manual console
         * HOLD refreshes every 460 ms, so the window is wide open.
         *
         * The safety TIMEOUT still has to run, or a claim taken while a safety
         * hold is live would strand the windings energized — so only the UART
         * refresh is suppressed; see spindle_hold_update(). */
        MOTOR_CONTROL_LOCK();
        spindle_hold_update_gated(!motor_scan_mode);

        /* An ALIGN session left open holds the scan claim, so the poll block
         * below never runs and the jam detectors stay dead. Checked here,
         * OUTSIDE that gate, for the same reason the hold timeout is. */
        if (motor_align_timeout_expired()) {
            uart_puts("ALIGN: session timeout - de-energizing\r\n");
            motor_exit_align();
        }
        MOTOR_CONTROL_UNLOCK();

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

            MOTOR_CONTROL_LOCK();
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
                        // GF on GB1.7 carries flags only (32 stopped / 34 running,
                        // or 16929+ with bit 14 = error). Direction is owned by
                        // command-time state, not derived from GF — see comment
                        // in the running-state polling path above.
                        /* Same masking as the running branch above — this
                         * parse had the identical whole-word equality test. */
                        uint16_t gf_state = (uint16_t)flags & GF_STATE_MASK;
                        bool known_good = (((uint16_t)flags & (uint16_t)~GF_KNOWN_BITS) == 0) &&
                                          (gf_state == GF_MOTOR_STOPPED ||
                                           gf_state == GF_MOTOR_RUNNING);
                        bool error_state = (flags & 0x4000) != 0;
                        /* REVIEW FIX (MEDIUM): motor_running was assigned
                         * unconditionally while motor_fault two lines down was
                         * guarded by the same known_good test — the guard
                         * existed but was not applied to the field that gates
                         * the display, the depth supervisor and
                         * motor_scan_try_claim(). A stray or spliced frame
                         * parsing to 34 marked an idle spindle as running. */
                        STATE_LOCK();
                        if (known_good || error_state) {
                            g_state.motor_running = (gf_state == GF_MOTOR_RUNNING);
                            g_state.motor_fault = error_state;
                        }
                        STATE_UNLOCK();
                        jam_notify_response();  // BUGFIX 2026-07-09: refresh jam comm-timeout
                        if (error_state) {
                            SEND_EVENT(EVT_MOTOR_FAULT);
                        }
                    }
                }

                // Second GF query (matches original firmware pattern)
                send_query(CMD_GET_FLAGS);
                wait_response(MOTOR_RESPONSE_TIMEOUT_MS);

                // KR query — motor load
                send_query(CMD_KEEP_RUNNING);
                /* REVIEW FIX (MEDIUM): the running branch got command-echo
                 * checking last round and the idle branch was left on the bare
                 * wait — same frame-misattribution cascade, just at 2 Hz: a late
                 * GF of 32/34 parsed as motor_load = 32%, a KR reading as an MCB
                 * temperature, a T0 reading as dc_bus_voltage (and a spurious
                 * EVT_LOW_VOLTAGE with it). */
                if (wait_response_for(MOTOR_RESPONSE_TIMEOUT_MS, CMD_KEEP_RUNNING)) {
                    size_t off = protocol_find_stx(rx_buffer, rx_index, 3);
                    if (off != SIZE_MAX) {
                        int16_t load;
                        if (protocol_parse_and_validate(rx_buffer, off, rx_index, 0, 100, &load)) {
                            update_kr_state((uint8_t)load);
                        }
                    }
                }

                // T0 query — MCB heatsink temperature (slow-changing, idle only)
                // Inline query avoids motor_read_param() mutex/busy-wait
                motor_uart_flush_rx();
                send_query(CMD_T0);
                if (wait_response_for(MOTOR_RESPONSE_TIMEOUT_MS, CMD_T0)) {
                    size_t toff = protocol_find_stx(rx_buffer, rx_index, 3);
                    if (toff != SIZE_MAX) {
                        int16_t mcb_temp;
                        if (protocol_parse_and_validate(rx_buffer, toff, rx_index, 0, 150, &mcb_temp)) {
                            temp_monitor_update((uint16_t)mcb_temp, settings_get()->power.temp_threshold);
                        }
                    }
                }

                // UD query — DC bus voltage (slow-changing, idle only)
                send_query(CMD_UD);
                if (wait_response_for(MOTOR_RESPONSE_TIMEOUT_MS, CMD_UD)) {
                    size_t off = protocol_find_stx(rx_buffer, rx_index, 3);
                    if (off != SIZE_MAX) {
                        int16_t voltage;
                        if (protocol_parse_and_validate(rx_buffer, off, rx_index, 0, 500, &voltage)) {
                            STATE_LOCK();
                            g_state.dc_bus_voltage = (uint16_t)voltage;
                            STATE_UNLOCK();
                            /* Sustained low bus, not an edge.
                             *
                             * FIELD FIX 2026-08-30: an edge detector cannot get
                             * this right. Seeded at 0 it treated boot as
                             * "already low", so a genuinely low bus never
                             * warned at all. Seeded at the threshold — my fix
                             * earlier the same day — it warned on EVERY boot,
                             * because the bus charges from zero and the first
                             * reading is legitimately below 300 V. The operator
                             * saw exactly that.
                             *
                             * What matters is that the bus STAYS low. Debounced,
                             * both cases work: the charge transient is ignored,
                             * a bus that is low from power-up still reports
                             * after ~2 s, and the warning re-arms once the bus
                             * recovers so a later sag is reported again. */
                            static uint8_t low_volt_count = 0;
                            static bool low_volt_reported = false;
                            if (voltage < DC_BUS_LOW_VOLTAGE_THRESHOLD) {
                                if (low_volt_count < DC_BUS_LOW_DEBOUNCE) {
                                    low_volt_count++;
                                }
                                if (low_volt_count >= DC_BUS_LOW_DEBOUNCE &&
                                    !low_volt_reported) {
                                    low_volt_reported = true;
                                    SEND_EVENT(EVT_LOW_VOLTAGE);
                                }
                            } else {
                                low_volt_count = 0;
                                low_volt_reported = false;
                            }
                        }
                    }
                }
            }

            /* Both branches, every poll — see run_jam_detectors(). The idle
             * branch is exactly where a stall lands us. */
            run_jam_detectors();
            MOTOR_CONTROL_UNLOCK();
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
