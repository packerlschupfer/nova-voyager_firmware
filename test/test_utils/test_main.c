/**
 * @file test_utilities.cpp
 * @brief Unit tests for utilities module (Phase 9)
 */

#include <unity.h>
#include "../src/utilities.c"  // Include implementation for testing

void setUp(void) {
    // Setup before each test
}

void tearDown(void) {
    // Cleanup after each test
}

/*===========================================================================*/
/* int_to_decimal_str() Tests                                                */
/*===========================================================================*/

void test_int_to_decimal_zero(void) {
    char buf[10];
    int len = int_to_decimal_str(0, buf);

    TEST_ASSERT_EQUAL(1, len);
    TEST_ASSERT_EQUAL('0', buf[0]);
}

void test_int_to_decimal_single_digit(void) {
    char buf[10];
    int len = int_to_decimal_str(7, buf);

    TEST_ASSERT_EQUAL(1, len);
    TEST_ASSERT_EQUAL('7', buf[0]);
}

void test_int_to_decimal_multi_digit(void) {
    char buf[10];
    int len = int_to_decimal_str(1234, buf);

    // Digits are REVERSED (LSB first) for efficient output reversal
    TEST_ASSERT_EQUAL(4, len);
    TEST_ASSERT_EQUAL('4', buf[0]);  // LSB
    TEST_ASSERT_EQUAL('3', buf[1]);
    TEST_ASSERT_EQUAL('2', buf[2]);
    TEST_ASSERT_EQUAL('1', buf[3]);  // MSB
}

void test_int_to_decimal_large_number(void) {
    char buf[10];
    int len = int_to_decimal_str(65535, buf);

    TEST_ASSERT_EQUAL(5, len);
    // 65535 reversed = "53556"
    TEST_ASSERT_EQUAL('5', buf[0]);
    TEST_ASSERT_EQUAL('3', buf[1]);
    TEST_ASSERT_EQUAL('5', buf[2]);
    TEST_ASSERT_EQUAL('5', buf[3]);
    TEST_ASSERT_EQUAL('6', buf[4]);
}

void test_int_to_decimal_motor_speed(void) {
    char buf[10];
    int len = int_to_decimal_str(1200, buf);  // Typical motor speed

    TEST_ASSERT_EQUAL(4, len);
    // 1200 reversed = "0021"
    TEST_ASSERT_EQUAL('0', buf[0]);
    TEST_ASSERT_EQUAL('0', buf[1]);
    TEST_ASSERT_EQUAL('2', buf[2]);
    TEST_ASSERT_EQUAL('1', buf[3]);
}

void test_int_to_decimal_max_speed(void) {
    char buf[10];
    int len = int_to_decimal_str(3200, buf);  // Max motor speed

    TEST_ASSERT_EQUAL(4, len);
    // 3200 reversed = "0023"
    TEST_ASSERT_EQUAL('0', buf[0]);
    TEST_ASSERT_EQUAL('0', buf[1]);
    TEST_ASSERT_EQUAL('2', buf[2]);
    TEST_ASSERT_EQUAL('3', buf[3]);
}

/*===========================================================================*/
/* Integration Test (used in protocol building)                              */
/*===========================================================================*/

void test_decimal_str_in_protocol_context(void) {
    // Simulate protocol usage: build decimal, output in reverse order
    char buf[10];
    int len = int_to_decimal_str(800, buf);

    // Build output string by reversing
    char output[10];
    for (int i = 0; i < len; i++) {
        output[i] = buf[len - 1 - i];
    }
    output[len] = '\0';

    TEST_ASSERT_EQUAL_STRING("800", output);
}

/*===========================================================================*/
/* Test Runner                                                                */
/*===========================================================================*/

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_int_to_decimal_zero);
    RUN_TEST(test_int_to_decimal_single_digit);
    RUN_TEST(test_int_to_decimal_multi_digit);
    RUN_TEST(test_int_to_decimal_large_number);
    RUN_TEST(test_int_to_decimal_motor_speed);
    RUN_TEST(test_int_to_decimal_max_speed);
    RUN_TEST(test_decimal_str_in_protocol_context);

    return UNITY_END();
}
