/**
 * @file motor_protocol.c
 * @brief Motor Controller Serial Protocol Implementation
 *
 * Extracted from task_motor.c and motor.c
 *
 * Implements the motor controller communication protocol with packet
 * building, parsing, and validation functions.
 */

#include "motor_protocol.h"
#include "utilities.h"
#include "config.h"
#include <string.h>

/*===========================================================================*/
/* Protocol Building Functions                                               */
/*===========================================================================*/

size_t protocol_build_query(uint16_t cmd, uint8_t* buffer) {
    size_t idx = 0;

    buffer[idx++] = PROTO_SOH;      // 0: SOH (0x04)
    buffer[idx++] = '0';            // 1: '0'
    buffer[idx++] = '0';            // 2: '0'
    buffer[idx++] = '1';            // 3: '1'
    buffer[idx++] = '1';            // 4: '1'
    buffer[idx++] = '1';            // 5: '1' (NOT STX!)
    buffer[idx++] = (cmd >> 8) & 0xFF;  // 6: CMD high
    buffer[idx++] = cmd & 0xFF;         // 7: CMD low
    buffer[idx++] = PROTO_ENQ;      // 8: ENQ (0x05)
    // No checksum for query format!

    return idx;
}

size_t protocol_build_read(uint16_t cmd, uint8_t* buffer) {
    /* A command frame with NO parameter digits — this is a READ, not a write.
     *
     * protocol_build_command(cmd, 0, ...) is NOT the same thing: it appends a
     * '0' parameter, which the MCB takes as "set this register to 0" and
     * answers with a bare ACK. That is exactly what happened with GR — reads
     * returned only 0x06 about five times in six, and the occasional value was
     * the MCB answering something else. The working "MQ GR" frame carries no
     * parameter at all:
     *     04 30 30 31 31 02 31 47 52 03 27
     *     SOH "0011"      STX '1' 'G' 'R' ETX XOR
     * Same shape, minus the digits. */
    size_t idx = 0;
    buffer[idx++] = PROTO_SOH;
    buffer[idx++] = '0';
    buffer[idx++] = '0';
    buffer[idx++] = '1';
    buffer[idx++] = '1';
    buffer[idx++] = PROTO_STX;
    buffer[idx++] = PROTO_UNIT;     /* XOR starts here */
    const size_t xor_start = idx - 1;

    buffer[idx++] = (cmd >> 8) & 0xFF;
    buffer[idx++] = cmd & 0xFF;

    uint8_t xor_sum = 0;
    for (size_t i = xor_start; i < idx; i++) xor_sum ^= buffer[i];

    buffer[idx++] = PROTO_ETX;
    xor_sum ^= PROTO_ETX;
    buffer[idx++] = xor_sum;
    return idx;
}

size_t protocol_build_command(uint16_t cmd, int16_t param, uint8_t* buffer) {
    size_t idx = 0;

    // Header
    buffer[idx++] = PROTO_SOH;      // 0: SOH (0x04)
    buffer[idx++] = '0';            // 1: '0'
    buffer[idx++] = '0';            // 2: '0'
    buffer[idx++] = '1';            // 3: '1'
    buffer[idx++] = '1';            // 4: '1'
    buffer[idx++] = PROTO_STX;      // 5: STX (0x02)
    buffer[idx++] = PROTO_UNIT;     // 6: '1' - XOR starts here!
    size_t xor_start = idx - 1;     // Mark position 6 for XOR calculation

    // Command (2 bytes, big-endian)
    buffer[idx++] = (cmd >> 8) & 0xFF;  // 7: CMD high
    buffer[idx++] = cmd & 0xFF;         // 8: CMD low

    // Initialize XOR from unit byte onwards
    uint8_t xor_sum = 0;
    for (size_t i = xor_start; i < idx; i++) {
        xor_sum ^= buffer[i];
    }

    /* Parameter (as ASCII decimal, with optional minus sign).
     *
     * REVIEW FIX (CRITICAL): this was `char digits[6]` while
     * int_to_decimal_str() writes up to TEN digits, and the negation was done
     * in int16_t — where -INT16_MIN is still INT16_MIN. So a param of -32768
     * emitted '-', left the value at -32768, widened it to 4294934528 on the
     * call, and wrote ten bytes into a six-byte stack buffer: four bytes of
     * this frame smashed, inside the function that builds every MCB command.
     * Take the magnitude in 32-bit, where it is representable, and give the
     * buffer the size the writer can actually use. */
    uint32_t magnitude;
    if (param < 0) {
        buffer[idx++] = '-';
        xor_sum ^= '-';
        magnitude = (uint32_t)(-(int32_t)param);
    } else {
        magnitude = (uint32_t)param;
    }

    // Convert parameter to decimal ASCII using utilities
    char digits[INT_DECIMAL_STR_MAX];
    int num_digits = int_to_decimal_str(magnitude, digits);

    // Output in correct order and update XOR
    for (int i = num_digits - 1; i >= 0; i--) {
        buffer[idx++] = digits[i];
        xor_sum ^= digits[i];
    }

    // ETX and checksum
    buffer[idx++] = PROTO_ETX;
    xor_sum ^= PROTO_ETX;
    buffer[idx++] = xor_sum;

    return idx;
}

/*===========================================================================*/
/* Protocol Parsing Functions                                                */
/*===========================================================================*/

int16_t protocol_parse_field(const uint8_t* buf, size_t start, size_t len) {
    int16_t value = 0;
    bool negative = false;

    for (size_t i = start; i < start + len && i < PROTO_MAX_PACKET_SIZE; i++) {
        if (buf[i] == '-') {
            negative = true;
        } else if (buf[i] >= '0' && buf[i] <= '9') {
            value = value * 10 + (buf[i] - '0');
        } else if (buf[i] == PROTO_ETX || buf[i] == ',') {
            break;
        }
    }

    return negative ? -value : value;
}

bool protocol_parse_gf_response(const uint8_t* response, size_t len,
                                 uint16_t* flags, uint16_t* speed, uint8_t* load,
                                 uint16_t* vib, uint16_t* temp) {
    // Initialize outputs
    *flags = 0;
    *speed = 0;
    *load = 0;
    *vib = 0;
    *temp = 0;

    // Find STX to start of data
    size_t data_start = 0;
    for (size_t i = 0; i < len; i++) {
        if (response[i] == PROTO_STX) {
            data_start = i + 2;  // Skip STX and unit byte
            break;
        }
    }

    if (data_start == 0) return false;  // No valid response

    // Parse comma-separated values
    // Field 0: flags (bit field)
    // Field 1: actual speed
    // Field 2: load percentage
    // Field 3: vibration level
    // Field 4: temperature

    size_t field = 0;
    size_t field_start = data_start;

    for (size_t i = data_start; i < len; i++) {
        if (response[i] == ',' || response[i] == PROTO_ETX) {
            int16_t value = protocol_parse_field(response, field_start, i - field_start);

            switch (field) {
                case 0:  // Flags
                    *flags = (uint16_t)value;
                    break;
                case 1:  // Speed
                    *speed = (value > 0) ? value : 0;
                    break;
                case 2:  // Load
                    *load = (value >= 0 && value <= 100) ? value : 0;
                    break;
                case 3:  // Vibration
                    *vib = (value > 0) ? value : 0;
                    break;
                case 4:  // Temperature
                    *temp = (value > 0) ? value : 0;
                    break;
            }

            field++;
            field_start = i + 1;

            if (response[i] == PROTO_ETX) break;
        }
    }

    return true;  // Successfully parsed
}

/* AUDIT FIX (HIGH, motor_protocol.c:181): this validated REQUEST framing
 * against a RESPONSE. It required PROTO_SOH (0x04) followed by the literal
 * address "0011" — which is what protocol_build_query() emits — but an MCB
 * reply is
 *
 *     [ACK]? [STX] [unit] [cmd_H] [cmd_L] <ascii digits> [ETX] [XOR]
 *
 * starting with 0x02, as documented in docs/MOTOR_PROTOCOL.md ("GF response is
 * ALWAYS single-field") and as every live parser in task_motor.c assumes. It
 * therefore returned 0 for every genuine reply, so parse_param_response() hit
 * `if (offset == 0) return -1` unconditionally and motor_read_param() never
 * returned a value in the shipped firmware. Downstream: MCBPARAMS always said
 * "MCB not responding", motor_set_power_level() always reported a CL readback
 * mismatch, motor_factory_reset() always reported failure even on success,
 * motor_cv_confidence_check() always returned 0, temp_query_mcb() never
 * updated its cache, and every console read (T0?, UD?, I3? ...) printed -1.
 *
 * The unit tests covered this function and asserted the wrong frame, which is
 * why 403 passing tests did not catch it — see test/test_protocol.
 *
 * It now also verifies the echoed command, so a reply that arrives out of step
 * with the query (interleaved frames from an unsynchronised second caller —
 * see the locking note in motor.c) is rejected instead of being parsed as an
 * answer to the wrong register.
 */
size_t protocol_validate_response(const uint8_t* response, size_t len,
                                  uint16_t expected_cmd) {
    if (expected_cmd != 0) {
        /* The MCB also emits CV updates unsolicited, so a read window can
         * contain someone else's frame before ours. Scan for the frame whose
         * command echo matches instead of insisting ours is first — otherwise
         * a CV that lands a millisecond early makes a perfectly good reply
         * unreadable. Bounded by len; the buffer is tens of bytes. */
        for (size_t i = 0; i + PROTO_RESP_HEADER_LEN + 1 <= len; i++) {
            if (response[i] == PROTO_STX &&
                response[i + 2] == (uint8_t)(expected_cmd >> 8) &&
                response[i + 3] == (uint8_t)(expected_cmd & 0xFF)) {
                return i + PROTO_RESP_HEADER_LEN;
            }
        }
        return 0;
    }

    size_t offset = 0;

    /* The MCB prefixes an ACK to some replies and not others. */
    if (len > 0 && response[0] == PROTO_ACK) {
        offset = 1;
    }

    /* STX, unit, cmd_H, cmd_L, then at least one more byte: either a digit of
     * data or the ETX of an empty field. */
    if (len < offset + PROTO_RESP_HEADER_LEN + 1) {
        return 0;
    }

    if (response[offset] != PROTO_STX) {
        return 0;
    }

    /* Data starts after STX, unit and the two command-echo bytes. */
    return offset + PROTO_RESP_HEADER_LEN;
}

uint8_t protocol_calc_checksum(const uint8_t* data, size_t len) {
    uint8_t xor_sum = 0;
    for (size_t i = 0; i < len; i++) {
        xor_sum ^= data[i];
    }
    return xor_sum;
}

/*===========================================================================*/
/* Response Parsing Helpers                                                  */
/*===========================================================================*/

size_t protocol_find_stx(const uint8_t* buffer, size_t len, size_t max_scan) {
    if (max_scan > len) max_scan = len;

    for (size_t i = 0; i < max_scan; i++) {
        if (buffer[i] == PROTO_STX) {
            return i;
        }
    }

    return SIZE_MAX;  // Not found
}

bool protocol_parse_and_validate(const uint8_t* buffer, size_t offset, size_t len,
                                  int16_t min_value, int16_t max_value,
                                  int16_t* out_value) {
    /* Validate buffer has minimum data after STX.
     *
     * REVIEW FIX (MEDIUM): this admitted len == offset + 5, i.e. a header-only
     * frame [STX][unit][cmd_H][cmd_L][ETX] with data_len 0.
     * protocol_parse_field() then never enters its loop and returns 0, which
     * passes every min_value == 0 check: a truncated CV reply displayed 0 RPM
     * on a turning spindle and fed jam_load_update()'s belt-break test, and a
     * truncated KR reply gave a false motor_load of 0%. The GF path was
     * explicitly hardened against this ("an empty field is not a reading of
     * zero"); this shared helper was not. Require at least one data byte. */
    if (len < offset + 6 || buffer[offset] != PROTO_STX) {
        return false;  // Invalid response
    }

    // Data starts after STX, unit, cmd_H, cmd_L
    size_t data_start = offset + 4;
    size_t data_len = len - data_start - 1;  // Exclude ETX

    // Parse field
    int16_t value = protocol_parse_field(buffer, data_start, data_len);

    // Validate range
    if (value < min_value || value > max_value) {
        return false;  // Out of range
    }

    *out_value = value;
    return true;
}
