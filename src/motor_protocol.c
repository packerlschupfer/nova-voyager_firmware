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

    // Parameter (as ASCII decimal, with optional minus sign)
    if (param < 0) {
        buffer[idx++] = '-';
        xor_sum ^= '-';
        param = -param;
    }

    // Convert parameter to decimal ASCII using utilities
    char digits[6];
    int num_digits = int_to_decimal_str(param, digits);

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

    for (size_t i = start; i < start + len && i < 128; i++) {
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

size_t protocol_validate_response(const uint8_t* response, size_t len) {
    // Need at least header (6 bytes minimum)
    if (len < 6) return 0;

    size_t offset = 0;

    // Skip ACK byte if present
    if (response[0] == PROTO_ACK) {
        offset = 1;
    }

    // Validate SOH
    if (response[offset] != PROTO_SOH) {
        return 0;  // Invalid frame
    }

    // Validate address "0011"
    if (response[offset + 1] != '0' ||
        response[offset + 2] != '0' ||
        response[offset + 3] != '1' ||
        response[offset + 4] != '1') {
        return 0;  // Invalid address
    }

    // Position 5 should be STX (0x02) for command response or '1' (0x31) for query
    if (response[offset + 5] != PROTO_STX && response[offset + 5] != '1') {
        return 0;  // Invalid frame type
    }

    // Return offset to data start (after STX/unit, skip command echo)
    return offset + 6;  // Points to start of command echo
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
    // Validate buffer has minimum data after STX
    if (len < offset + 5 || buffer[offset] != PROTO_STX) {
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
