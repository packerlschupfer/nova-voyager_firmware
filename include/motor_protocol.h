/**
 * @file motor_protocol.h
 * @brief Motor Controller Serial Protocol Layer
 *
 * Phase 6: Extracted from task_motor.c and motor.c
 *
 * Defines the motor controller communication protocol:
 * - Query format: [SOH][addr][0x31][CMD_H][CMD_L][ENQ]
 * - Command format: [SOH][addr][STX][0x31][CMD_H][CMD_L][param...][ETX][XOR]
 *
 * Protocol constants:
 * - SOH = 0x04, STX = 0x02, ETX = 0x03, ENQ = 0x05
 * - Address = "0011"
 * - Unit byte = '1' (0x31)
 * - Checksum: XOR from unit byte to ETX (inclusive)
 */

#ifndef MOTOR_PROTOCOL_H
#define MOTOR_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*===========================================================================*/
/* Protocol Constants                                                         */
/*===========================================================================*/

#define PROTO_SOH           0x04
#define PROTO_STX           0x02
#define PROTO_ETX           0x03
#define PROTO_ENQ           0x05
#define PROTO_ACK           0x06

/* Bytes of an MCB reply before the ASCII data field:
 * [STX][unit][cmd_H][cmd_L]. See protocol_validate_response(). */
#define PROTO_RESP_HEADER_LEN 4
#define PROTO_ADDR          "0011"
#define PROTO_UNIT          '1'

#define PROTO_MAX_PACKET_SIZE   32

/*===========================================================================*/
/* Protocol Building Functions                                               */
/*===========================================================================*/

/**
 * @brief Build query packet (for reading status/parameters)
 * @param cmd Command code (e.g., CMD_GET_FLAGS, CMD_SET_SPEED)
 * @param buffer Output buffer (must be at least PROTO_MAX_PACKET_SIZE)
 * @return Packet length
 *
 * Format: [SOH][addr]['1'][CMD_H][CMD_L][ENQ]
 * Note: No checksum for query format!
 */
size_t protocol_build_query(uint16_t cmd, uint8_t* buffer);

/**
 * @brief Build command packet (for setting parameters)
 * @param cmd Command code (e.g., CMD_STOP, CMD_START, CMD_SET_SPEED)
 * @param param Parameter value (decimal, can be negative)
 * @param buffer Output buffer (must be at least PROTO_MAX_PACKET_SIZE)
 * @return Packet length
 *
 * Format: [SOH][addr][STX]['1'][CMD_H][CMD_L][param_ascii][ETX][XOR]
 * Note: Checksum is XOR from unit byte '1' to ETX (inclusive)
 */
size_t protocol_build_command(uint16_t cmd, int16_t param, uint8_t* buffer);

/**
 * @brief Build a command-format READ: same frame, no parameter digits.
 * @note Not the same as protocol_build_command(cmd, 0, ...) — that writes 0.
 */
size_t protocol_build_read(uint16_t cmd, uint8_t* buffer);

/*===========================================================================*/
/* Protocol Parsing Functions                                                */
/*===========================================================================*/

/**
 * @brief Parse decimal field from ASCII response buffer
 * @param buf Response buffer
 * @param start Start offset in buffer
 * @param len Length to parse (or until ETX/comma)
 * @return Parsed integer value (supports negative)
 */
int16_t protocol_parse_field(const uint8_t* buf, size_t start, size_t len);

/**
 * @brief Parse GF (Get Flags) response with comma-separated values
 * @param response Response buffer (already validated)
 * @param len Response length
 * @param flags Output: flags value
 * @param speed Output: speed value (or 0 if not present)
 * @param load Output: load value (or 0 if not present)
 * @param vib Output: vibration value (or 0 if not present)
 * @param temp Output: temperature value (or 0 if not present)
 * @return true if parsed successfully, false on error
 *
 * GF response can be:
 * - Single value: flags only
 * - Multiple values: flags,speed,load,vib,temp (comma-separated)
 */
bool protocol_parse_gf_response(const uint8_t* response, size_t len,
                                 uint16_t* flags, uint16_t* speed, uint8_t* load,
                                 uint16_t* vib, uint16_t* temp);

/**
 * @brief Validate an MCB response frame and locate its data field.
 *
 * Reply framing is `[ACK]? [STX] [unit] [cmd_H] [cmd_L] <digits> [ETX] [XOR]`.
 * (Do not confuse this with the REQUEST framing built by
 * protocol_build_query(), which is SOH + "0011" + '1' + cmd + ENQ. Validating
 * one against the other is the bug this contract was rewritten to fix.)
 *
 * @param response      Received bytes.
 * @param len           Number of bytes in @p response.
 * @param expected_cmd  Command that was queried, e.g. CMD_GR (0x4752). The
 *                      buffer is SCANNED for the frame whose command echo
 *                      matches, because the MCB emits CV updates unsolicited
 *                      and one can land in the read window ahead of the reply
 *                      being waited for. Pass 0 to skip the check and validate
 *                      only the frame at the start of the buffer.
 * @return Offset of the first data byte, or 0 if the frame is not a valid
 *         response (0 is never a valid data offset — a frame always has at
 *         least the STX header before its data).
 */
size_t protocol_validate_response(const uint8_t* response, size_t len,
                                  uint16_t expected_cmd);

/**
 * @brief Calculate protocol checksum (XOR)
 * @param data Data buffer starting from unit byte
 * @param len Length of data (up to and including ETX)
 * @return XOR checksum
 */
uint8_t protocol_calc_checksum(const uint8_t* data, size_t len);

/*===========================================================================*/
/* Response Parsing Helpers                                                  */
/*===========================================================================*/

/**
 * @brief Find STX (0x02) offset in response buffer
 * @param buffer Response buffer
 * @param len Buffer length
 * @param max_scan Maximum bytes to scan (typically 3 for ACK handling)
 * @return Offset to STX, or SIZE_MAX if not found
 */
size_t protocol_find_stx(const uint8_t* buffer, size_t len, size_t max_scan);

/**
 * @brief Parse and validate response field with range checking
 * @param buffer Response buffer
 * @param offset STX offset
 * @param len Buffer length
 * @param min_value Minimum valid value
 * @param max_value Maximum valid value
 * @param out_value Output pointer for parsed value
 * @return true if parsed and valid, false otherwise
 */
bool protocol_parse_and_validate(const uint8_t* buffer, size_t offset, size_t len,
                                  int16_t min_value, int16_t max_value,
                                  int16_t* out_value);

#endif // MOTOR_PROTOCOL_H
