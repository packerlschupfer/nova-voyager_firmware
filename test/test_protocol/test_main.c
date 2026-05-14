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

void test_validate_response_valid(void) {
    uint8_t response[] = {0x04, '0', '0', '1', '1', 0x02, '1', 'G', 'F'};
    size_t offset = protocol_validate_response(response, 9);

    TEST_ASSERT_EQUAL(6, offset);  // Points to data start (after STX)
}

void test_validate_response_with_ack(void) {
    uint8_t response[] = {0x06, 0x04, '0', '0', '1', '1', 0x02, '1'};
    size_t offset = protocol_validate_response(response, 8);

    TEST_ASSERT_EQUAL(7, offset);  // Skips ACK, points to data
}

void test_validate_response_invalid_soh(void) {
    uint8_t response[] = {0xFF, '0', '0', '1', '1', 0x02};
    size_t offset = protocol_validate_response(response, 6);

    TEST_ASSERT_EQUAL(0, offset);  // Invalid
}

void test_validate_response_too_short(void) {
    uint8_t response[] = {0x04, '0', '0'};
    size_t offset = protocol_validate_response(response, 3);

    TEST_ASSERT_EQUAL(0, offset);  // Too short
}

/*===========================================================================*/
/* Test Runner                                                                */
/*===========================================================================*/

int main(int argc, char **argv) {
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
    RUN_TEST(test_validate_response_invalid_soh);
    RUN_TEST(test_validate_response_too_short);

    return UNITY_END();
}
