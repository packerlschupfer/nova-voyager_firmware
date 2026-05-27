/**
 * @file test_motor_protocol.c
 * @brief Unit tests for motor protocol module (Phase 9)
 */

#include <unity.h>
#include "../../src/utilities.c"      // Needed for int_to_decimal_str
#include "../../src/motor_protocol.c"  // Include implementation for testing

void setUp(void) {
    // Setup before each test
}

void tearDown(void) {
    // Cleanup after each test
}

/*===========================================================================*/
/* protocol_build_query() Tests                                              */
/*===========================================================================*/

void test_build_query_basic(void) {
    uint8_t buffer[32];
    size_t len = protocol_build_query(0x4746, buffer);  // "GF" command

    // Should be 9 bytes: [SOH]['0']['0']['1']['1']['1'][CMD_H][CMD_L][ENQ]
    TEST_ASSERT_EQUAL(9, len);

    // Validate structure
    TEST_ASSERT_EQUAL_HEX8(0x04, buffer[0]);  // SOH
    TEST_ASSERT_EQUAL('0', buffer[1]);
    TEST_ASSERT_EQUAL('0', buffer[2]);
    TEST_ASSERT_EQUAL('1', buffer[3]);
    TEST_ASSERT_EQUAL('1', buffer[4]);
    TEST_ASSERT_EQUAL('1', buffer[5]);  // Query indicator (not STX!)
    TEST_ASSERT_EQUAL_HEX8(0x47, buffer[6]);  // 'G'
    TEST_ASSERT_EQUAL_HEX8(0x46, buffer[7]);  // 'F'
    TEST_ASSERT_EQUAL_HEX8(0x05, buffer[8]);  // ENQ
}

void test_build_query_different_command(void) {
    uint8_t buffer[32];
    size_t len = protocol_build_query(0x5356, buffer);  // "SV" command

    TEST_ASSERT_EQUAL(9, len);
    TEST_ASSERT_EQUAL_HEX8(0x53, buffer[6]);  // 'S'
    TEST_ASSERT_EQUAL_HEX8(0x56, buffer[7]);  // 'V'
}

/*===========================================================================*/
/* protocol_build_command() Tests                                            */
/*===========================================================================*/

void test_build_command_positive_param(void) {
    uint8_t buffer[32];
    size_t len = protocol_build_command(0x5356, 1200, buffer);  // SV=1200

    // Validate header
    TEST_ASSERT_EQUAL_HEX8(0x04, buffer[0]);  // SOH
    TEST_ASSERT_EQUAL('0', buffer[1]);
    TEST_ASSERT_EQUAL('0', buffer[2]);
    TEST_ASSERT_EQUAL('1', buffer[3]);
    TEST_ASSERT_EQUAL('1', buffer[4]);
    TEST_ASSERT_EQUAL_HEX8(0x02, buffer[5]);  // STX
    TEST_ASSERT_EQUAL('1', buffer[6]);  // Unit byte

    // Command
    TEST_ASSERT_EQUAL_HEX8(0x53, buffer[7]);  // 'S'
    TEST_ASSERT_EQUAL_HEX8(0x56, buffer[8]);  // 'V'

    // Parameter "1200"
    TEST_ASSERT_EQUAL('1', buffer[9]);
    TEST_ASSERT_EQUAL('2', buffer[10]);
    TEST_ASSERT_EQUAL('0', buffer[11]);
    TEST_ASSERT_EQUAL('0', buffer[12]);

    // ETX
    TEST_ASSERT_EQUAL_HEX8(0x03, buffer[13]);

    // Checksum (last byte) - just verify it exists
    TEST_ASSERT_EQUAL(15, len);  // Total length
}

void test_build_command_zero_param(void) {
    uint8_t buffer[32];
    size_t len = protocol_build_command(0x5253, 0, buffer);  // RS=0

    // Parameter should be '0'
    TEST_ASSERT_EQUAL('1', buffer[6]);  // Unit
    TEST_ASSERT_EQUAL_HEX8(0x52, buffer[7]);  // 'R'
    TEST_ASSERT_EQUAL_HEX8(0x53, buffer[8]);  // 'S'
    TEST_ASSERT_EQUAL('0', buffer[9]);  // Param
    TEST_ASSERT_EQUAL_HEX8(0x03, buffer[10]);  // ETX

    TEST_ASSERT_EQUAL(12, len);
}

void test_build_command_negative_param(void) {
    uint8_t buffer[32];
    size_t len = protocol_build_command(0x5356, -100, buffer);  // SV=-100

    // Should have minus sign
    TEST_ASSERT_EQUAL('-', buffer[9]);
    TEST_ASSERT_EQUAL('1', buffer[10]);
    TEST_ASSERT_EQUAL('0', buffer[11]);
    TEST_ASSERT_EQUAL('0', buffer[12]);
}

/*===========================================================================*/
/* protocol_parse_field() Tests                                              */
/*===========================================================================*/

void test_parse_field_positive(void) {
    uint8_t data[] = "1234";
    int16_t result = protocol_parse_field(data, 0, 4);
    TEST_ASSERT_EQUAL(1234, result);
}

void test_parse_field_negative(void) {
    uint8_t data[] = "-567";
    int16_t result = protocol_parse_field(data, 0, 4);
    TEST_ASSERT_EQUAL(-567, result);
}

void test_parse_field_zero(void) {
    uint8_t data[] = "0";
    int16_t result = protocol_parse_field(data, 0, 1);
    TEST_ASSERT_EQUAL(0, result);
}

void test_parse_field_with_etx(void) {
    uint8_t data[] = "123\x03tail";
    int16_t result = protocol_parse_field(data, 0, 8);
    TEST_ASSERT_EQUAL(123, result);  // Should stop at ETX
}

void test_parse_field_with_comma(void) {
    uint8_t data[] = "456,next";
    int16_t result = protocol_parse_field(data, 0, 9);
    TEST_ASSERT_EQUAL(456, result);  // Should stop at comma
}

/*===========================================================================*/
/* protocol_calc_checksum() Tests                                            */
/*===========================================================================*/

void test_calc_checksum_simple(void) {
    uint8_t data[] = {'1', 'G', 'F', '3', '2', 0x03};  // Example: unit + GF + 32 + ETX
    uint8_t checksum = protocol_calc_checksum(data, 6);

    uint8_t expected = '1' ^ 'G' ^ 'F' ^ '3' ^ '2' ^ 0x03;
    TEST_ASSERT_EQUAL_HEX8(expected, checksum);
}

void test_calc_checksum_empty(void) {
    uint8_t data[] = {};
    uint8_t checksum = protocol_calc_checksum(data, 0);
    TEST_ASSERT_EQUAL_HEX8(0, checksum);  // XOR of nothing is 0
}

/*===========================================================================*/
/* protocol_validate_response() Tests                                        */
/*===========================================================================*/
/* These used to assert the REQUEST framing (SOH + "0011") against a response
 * and so locked in the bug they were meant to guard: the validator rejected
 * every real MCB reply, and motor_read_param() never returned a value in
 * v0.1.0. The frame below is the documented one — see docs/MOTOR_PROTOCOL.md,
 * "GF response is ALWAYS single-field" — and matches every live parser in
 * task_motor.c:
 *
 *     [ACK]? [STX] [unit] [cmd_H] [cmd_L] <ascii digits> [ETX] [XOR]
 */

/* CMD_GR = 'G' << 8 | 'R'. Spelled out rather than including config.h so the
 * test states the wire encoding it depends on. */
#define TEST_CMD_GR 0x4752
#define TEST_CMD_GF 0x4746

void test_validate_response_valid(void) {
    /* GR reply carrying "7". */
    uint8_t response[] = {0x02, '1', 'G', 'R', '7', 0x03, 0x00};
    size_t offset = protocol_validate_response(response, sizeof(response), TEST_CMD_GR);

    TEST_ASSERT_EQUAL(4, offset);  /* first data byte, the '7' */
    TEST_ASSERT_EQUAL('7', response[offset]);
}

void test_validate_response_with_ack(void) {
    uint8_t response[] = {0x06, 0x02, '1', 'G', 'R', '7', 0x03, 0x00};
    size_t offset = protocol_validate_response(response, sizeof(response), TEST_CMD_GR);

    TEST_ASSERT_EQUAL(5, offset);  /* ACK skipped */
    TEST_ASSERT_EQUAL('7', response[offset]);
}

void test_validate_response_multi_digit_data(void) {
    /* The GF reply the machine actually sends when running. */
    uint8_t response[] = {0x02, '1', 'G', 'F', '3', '4', 0x03, 0x00};
    size_t offset = protocol_validate_response(response, sizeof(response), TEST_CMD_GF);

    TEST_ASSERT_EQUAL(4, offset);
    TEST_ASSERT_EQUAL(34, protocol_parse_field(response, offset, sizeof(response) - offset));
}

void test_validate_response_rejects_request_framing(void) {
    /* This is what protocol_build_query() emits. It is NOT a response, and
     * accepting it here is exactly the bug that shipped. */
    uint8_t request[] = {0x04, '0', '0', '1', '1', '1', 'G', 'R', 0x05};
    TEST_ASSERT_EQUAL(0, protocol_validate_response(request, sizeof(request), TEST_CMD_GR));
}

void test_validate_response_rejects_missing_stx(void) {
    uint8_t response[] = {0xFF, '1', 'G', 'R', '7', 0x03};
    TEST_ASSERT_EQUAL(0, protocol_validate_response(response, sizeof(response), TEST_CMD_GR));
}

void test_validate_response_rejects_wrong_command_echo(void) {
    /* A KR reply arriving while we were waiting for GR — an interleaved frame
     * must not be parsed as the answer to our query. */
    uint8_t response[] = {0x02, '1', 'K', 'R', '4', '2', 0x03};
    TEST_ASSERT_EQUAL(0, protocol_validate_response(response, sizeof(response), TEST_CMD_GR));
}

void test_validate_response_accepts_any_command_when_unspecified(void) {
    uint8_t response[] = {0x02, '1', 'K', 'R', '4', '2', 0x03};
    TEST_ASSERT_EQUAL(4, protocol_validate_response(response, sizeof(response), 0));
}

void test_validate_response_too_short(void) {
    /* Header present but no data byte at all. */
    uint8_t response[] = {0x02, '1', 'G', 'R'};
    TEST_ASSERT_EQUAL(0, protocol_validate_response(response, sizeof(response), TEST_CMD_GR));
}

void test_validate_response_empty_buffer(void) {
    uint8_t response[] = {0x00};
    TEST_ASSERT_EQUAL(0, protocol_validate_response(response, 0, TEST_CMD_GR));
}

void test_validate_response_lone_ack_is_not_a_frame(void) {
    uint8_t response[] = {0x06};
    TEST_ASSERT_EQUAL(0, protocol_validate_response(response, 1, TEST_CMD_GR));
}

/* The two-byte fragment the machine was observed returning to a GR query while
 * the motor task was draining the same UART. It must be rejected, not parsed. */
void test_validate_response_rejects_truncated_frame(void) {
    uint8_t response[] = {0x02, '1'};
    TEST_ASSERT_EQUAL(0, protocol_validate_response(response, sizeof(response), TEST_CMD_GR));
}

/* The MCB emits CV updates unsolicited. One landing in the read window ahead
 * of the reply we asked for must not make that reply unreadable — that is the
 * difference between MCBPARAMS working and reporting "not responding". */
void test_validate_response_finds_frame_behind_an_unsolicited_cv(void) {
    uint8_t response[] = {
        0x02, '1', 'C', 'V', '1', '2', '0', '0', 0x03, 0x00,  /* not ours */
        0x02, '1', 'G', 'R', '7', 0x03, 0x00                  /* ours */
    };
    size_t offset = protocol_validate_response(response, sizeof(response), TEST_CMD_GR);
    TEST_ASSERT_EQUAL(14, offset);
    TEST_ASSERT_EQUAL('7', response[offset]);
}

/* Scanning must not turn "no reply from us" into a false positive. */
void test_validate_response_scan_still_rejects_when_ours_is_absent(void) {
    uint8_t response[] = {
        0x02, '1', 'C', 'V', '1', '2', '0', '0', 0x03, 0x00,
        0x02, '1', 'K', 'R', '4', '2', 0x03
    };
    TEST_ASSERT_EQUAL(0, protocol_validate_response(response, sizeof(response), TEST_CMD_GR));
}

/*===========================================================================*/
/* Test Runner                                                                */
/*===========================================================================*/

int main(void) {
    UNITY_BEGIN();

    // Build tests
    RUN_TEST(test_build_query_basic);
    RUN_TEST(test_build_query_different_command);
    RUN_TEST(test_build_command_positive_param);
    RUN_TEST(test_build_command_zero_param);
    RUN_TEST(test_build_command_negative_param);

    // Parse tests
    RUN_TEST(test_parse_field_positive);
    RUN_TEST(test_parse_field_negative);
    RUN_TEST(test_parse_field_zero);
    RUN_TEST(test_parse_field_with_etx);
    RUN_TEST(test_parse_field_with_comma);

    // Checksum tests
    RUN_TEST(test_calc_checksum_simple);
    RUN_TEST(test_calc_checksum_empty);

    // Validation tests
    RUN_TEST(test_validate_response_valid);
    RUN_TEST(test_validate_response_with_ack);
    RUN_TEST(test_validate_response_multi_digit_data);
    RUN_TEST(test_validate_response_rejects_request_framing);
    RUN_TEST(test_validate_response_rejects_missing_stx);
    RUN_TEST(test_validate_response_rejects_wrong_command_echo);
    RUN_TEST(test_validate_response_accepts_any_command_when_unspecified);
    RUN_TEST(test_validate_response_too_short);
    RUN_TEST(test_validate_response_empty_buffer);
    RUN_TEST(test_validate_response_lone_ack_is_not_a_frame);
    RUN_TEST(test_validate_response_rejects_truncated_frame);
    RUN_TEST(test_validate_response_finds_frame_behind_an_unsolicited_cv);
    RUN_TEST(test_validate_response_scan_still_rejects_when_ours_is_absent);

    return UNITY_END();
}
