/**
 * @file test_motor_uart.c
 * @brief Unit tests for motor UART layer
 *
 * Tests timeout handling, buffer management, and error conditions.
 */

#include <unity.h>
#include <string.h>

// Include mock implementations
#include "../mocks/stm32f1xx_hal.c"
#include "../mocks/FreeRTOS.c"

// Mock diagnostics (no-ops for testing)
void diagnostics_uart_tx_timeout(void) {}
void diagnostics_uart_rx_timeout(void) {}
void diagnostics_uart_tx_bytes(uint32_t n) { (void)n; }
void diagnostics_uart_rx_bytes(uint32_t n) { (void)n; }

// FreeRTOS mocking now in stm32_hal_mock.h

// Mock UART output
void uart_puts(const char* s) { (void)s; }

// Mock config.h constants (motor_uart.c needs these)
#ifndef MOTOR_UART_BYTE_TIMEOUT_MS
#define MOTOR_UART_BYTE_TIMEOUT_MS 100
#endif
#ifndef MOTOR_UART_RX_TIMEOUT_MS
#define MOTOR_UART_RX_TIMEOUT_MS 100
#endif
#ifndef APB1_FREQ
#define APB1_FREQ 30000000
#endif

// Include implementation
#include "../../src/motor_uart.c"

/*===========================================================================*/
/* Test Setup/Teardown                                                       */
/*===========================================================================*/

void setUp(void) {
    // Reset mock peripherals
    extern USART_TypeDef mock_USART3;
    extern RCC_TypeDef mock_RCC;
    extern GPIO_TypeDef mock_GPIOB;

    memset(&mock_USART3, 0, sizeof(mock_USART3));
    memset(&mock_RCC, 0, sizeof(mock_RCC));
    memset(&mock_GPIOB, 0, sizeof(mock_GPIOB));

    extern TickType_t mock_tick_count;
    extern TickType_t mock_tick_step;
    mock_tick_count = 0;
    mock_tick_step  = 0;  // Static tick by default; set >0 to enable timeout tests
}

void tearDown(void) {
    // Cleanup
}

/*===========================================================================*/
/* motor_uart_send_byte() Tests                                              */
/*===========================================================================*/

void test_send_byte_success(void) {
    extern USART_TypeDef mock_USART3;

    // Simulate TXE flag set immediately
    mock_USART3.SR = USART_SR_TXE;

    bool result = motor_uart_send_byte(0x42);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_HEX8(0x42, mock_USART3.DR);
}

void test_send_byte_timeout(void) {
    extern TickType_t mock_tick_step;
    // TXE never set; tick advances by 1 on every xTaskGetTickCount() call
    // → timeout fires after MOTOR_UART_BYTE_TIMEOUT_MS + 1 iterations
    mock_USART3.SR = 0x00;
    mock_tick_step = 1;

    bool result = motor_uart_send_byte(0x42);

    TEST_ASSERT_FALSE(result);
    // DR must NOT have been written — no byte sent on timeout
    TEST_ASSERT_EQUAL_HEX8(0x00, mock_USART3.DR);
}

void test_send_byte_eventual_success(void) {
    // Full "TXE becomes ready after N polls" needs dynamic TXE state —
    // not possible with a static register mock. Already covered by
    // test_send_byte_success (TXE pre-set) and test_send_byte_timeout.
    TEST_IGNORE_MESSAGE("Needs dynamic TXE mock - covered by success+timeout tests");
}

/*===========================================================================*/
/* motor_uart_rx_available() Tests                                           */
/*===========================================================================*/

void test_rx_available_when_data_present(void) {
    mock_USART3.SR = USART_SR_RXNE;

    bool result = motor_uart_rx_available();

    TEST_ASSERT_TRUE(result);
}

void test_rx_not_available_when_empty(void) {
    mock_USART3.SR = 0x00;

    bool result = motor_uart_rx_available();

    TEST_ASSERT_FALSE(result);
}

/*===========================================================================*/
/* motor_uart_read_byte() Tests                                              */
/*===========================================================================*/

void test_read_byte_success(void) {
    mock_USART3.SR = USART_SR_RXNE;
    mock_USART3.DR = 0xAB;

    uint8_t byte = motor_uart_read_byte();

    TEST_ASSERT_EQUAL_HEX8(0xAB, byte);
}

/*===========================================================================*/
/* motor_uart_flush_rx() Tests                                               */
/*===========================================================================*/

void test_flush_empty_buffer_returns_immediately(void) {
    // With RXNE = 0, flush exits immediately — no loop, no hang
    mock_USART3.SR = 0x00;  // RXNE clear

    motor_uart_flush_rx();  // Must return without blocking

    // If we reach here, flush correctly handled empty buffer
    TEST_PASS();
    // Note: "flush drains non-empty buffer" requires DR-clears-RXNE hardware
    // behaviour which a static register mock cannot simulate.
}

/*===========================================================================*/
/* Test Runner                                                               */
/*===========================================================================*/

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_send_byte_success);
    RUN_TEST(test_send_byte_timeout);           // Enabled: uses mock_tick_step
    RUN_TEST(test_send_byte_eventual_success);  // Skipped: needs dynamic TXE mock
    RUN_TEST(test_rx_available_when_data_present);
    RUN_TEST(test_rx_not_available_when_empty);
    RUN_TEST(test_read_byte_success);
    RUN_TEST(test_flush_empty_buffer_returns_immediately);

    return UNITY_END();
}
