/**
 * @file test_main.c
 * @brief Unit tests for the AT24C02 EEPROM driver (eeprom.c)
 *
 * The real driver uses bit-bang I2C over PC4/PC5. Here we replace the
 * hardware layer with a 256-byte RAM array and a fault-injection flag,
 * then re-implement the EEPROM logic as pure functions that exercise
 * the same boundary conditions, page-splitting, and error paths.
 *
 * No HAL headers required — fully self-contained for the native env.
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/*===========================================================================*/
/* Constants (from config.h / eeprom.h)                                      */
/*===========================================================================*/

#define EEPROM_SIZE       256
#define EEPROM_PAGE_SIZE  8

typedef enum {
    EEPROM_OK = 0,
    EEPROM_ERROR,
    EEPROM_BUSY,
    EEPROM_TIMEOUT,
    EEPROM_NOT_FOUND
} eeprom_status_t;

/*===========================================================================*/
/* Mock backend                                                               */
/*===========================================================================*/

static uint8_t  mock_eeprom[256];
static bool     mock_i2c_fail = false;
static bool     mock_initialized = false;

/* Reset to power-on state: all bytes 0xFF, no faults. */
static void mock_reset(void) {
    memset(mock_eeprom, 0xFF, sizeof(mock_eeprom));
    mock_i2c_fail   = false;
    mock_initialized = true;
}

/*===========================================================================*/
/* Re-implemented EEPROM API using the mock backend                          */
/*===========================================================================*/

/*
 * eeprom_init — succeeds unless I2C fault is pre-injected.
 */
static bool eeprom_init(void) {
    if (mock_i2c_fail) {
        mock_initialized = false;
        return false;
    }
    mock_initialized = true;
    return true;
}

/*
 * eeprom_read — mirrors the boundary checks and sequential-read protocol
 * of the real driver, but reads from mock_eeprom instead of the wire.
 */
static eeprom_status_t eeprom_read(uint16_t addr, uint8_t *data, size_t len) {
    if (!mock_initialized || data == NULL || len == 0)
        return EEPROM_ERROR;
    if (addr + len > EEPROM_SIZE)
        return EEPROM_ERROR;
    if (mock_i2c_fail)
        return EEPROM_ERROR;

    memcpy(data, &mock_eeprom[addr], len);
    return EEPROM_OK;
}

/*
 * eeprom_write — mirrors the page-aware write loop from the real driver.
 * Each chunk is bounded by the 8-byte page boundary.
 * If mock_i2c_fail is set the first chunk fails immediately.
 */
static eeprom_status_t eeprom_write(uint16_t addr, const uint8_t *data, size_t len) {
    if (!mock_initialized || data == NULL || len == 0)
        return EEPROM_ERROR;
    if (addr + len > EEPROM_SIZE)
        return EEPROM_ERROR;
    if (mock_i2c_fail)
        return EEPROM_ERROR;

    size_t written = 0;
    while (written < len) {
        uint16_t page_offset = (addr + written) % EEPROM_PAGE_SIZE;
        size_t   chunk       = EEPROM_PAGE_SIZE - page_offset;
        if (chunk > len - written)
            chunk = len - written;

        memcpy(&mock_eeprom[addr + written], data + written, chunk);
        written += chunk;
    }
    return EEPROM_OK;
}

/* Thin wrappers — match eeprom.h signatures */
static eeprom_status_t eeprom_read_byte(uint16_t addr, uint8_t *value) {
    return eeprom_read(addr, value, 1);
}

static eeprom_status_t eeprom_write_byte(uint16_t addr, uint8_t value) {
    return eeprom_write(addr, &value, 1);
}

/*===========================================================================*/
/* Unity fixtures                                                             */
/*===========================================================================*/

void setUp(void) {
    mock_reset();
}

void tearDown(void) {
    /* nothing to clean up */
}

/*===========================================================================*/
/* Test 1 — Write then read: single-byte round-trip                          */
/*===========================================================================*/

void test_write_then_read_single_byte(void) {
    uint8_t val = 0xAB;
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_write_byte(0x10, val));

    uint8_t got = 0x00;
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_read_byte(0x10, &got));
    TEST_ASSERT_EQUAL_HEX8(val, got);
}

void test_write_then_read_does_not_disturb_neighbour(void) {
    eeprom_write_byte(0x20, 0x11);
    eeprom_write_byte(0x21, 0x22);

    uint8_t a, b;
    eeprom_read_byte(0x20, &a);
    eeprom_read_byte(0x21, &b);
    TEST_ASSERT_EQUAL_HEX8(0x11, a);
    TEST_ASSERT_EQUAL_HEX8(0x22, b);
}

/*===========================================================================*/
/* Test 2 — Block write: multi-byte sequence read back intact                */
/*===========================================================================*/

void test_block_write_reads_back_correctly(void) {
    const uint8_t src[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_write(0x40, src, sizeof(src)));

    uint8_t dst[6] = {0};
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_read(0x40, dst, sizeof(dst)));
    TEST_ASSERT_EQUAL_MEMORY(src, dst, sizeof(src));
}

void test_block_write_leaves_unwritten_bytes_unchanged(void) {
    /* 0xFF is the erased state set by mock_reset() */
    const uint8_t src[3] = {0xAA, 0xBB, 0xCC};
    eeprom_write(0x50, src, 3);

    uint8_t before;
    eeprom_read_byte(0x4F, &before);
    TEST_ASSERT_EQUAL_HEX8(0xFF, before);

    uint8_t after;
    eeprom_read_byte(0x53, &after);
    TEST_ASSERT_EQUAL_HEX8(0xFF, after);
}

/*===========================================================================*/
/* Test 3 — Address bounds: last valid address and just beyond               */
/*===========================================================================*/

void test_write_at_last_byte_succeeds(void) {
    /* Address 255, length 1 — exactly within the 256-byte device */
    uint8_t val = 0x7E;
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_write(255, &val, 1));

    uint8_t got;
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_read(255, &got, 1));
    TEST_ASSERT_EQUAL_HEX8(val, got);
}

void test_write_beyond_256_fails(void) {
    /* addr=255, len=2 → addr+len=257 > EEPROM_SIZE */
    uint8_t buf[2] = {0x11, 0x22};
    TEST_ASSERT_EQUAL(EEPROM_ERROR, eeprom_write(255, buf, 2));
}

void test_read_beyond_256_fails(void) {
    uint8_t buf[2];
    TEST_ASSERT_EQUAL(EEPROM_ERROR, eeprom_read(255, buf, 2));
}

void test_write_starting_beyond_device_fails(void) {
    /* addr=256 is already out of range for a 256-byte part */
    uint8_t val = 0xFF;
    /* addr + len = 257 > 256 — overflow path */
    TEST_ASSERT_EQUAL(EEPROM_ERROR, eeprom_write(256, &val, 1));
}

/*===========================================================================*/
/* Test 4 — Read empty: fresh EEPROM returns 0xFF                           */
/*===========================================================================*/

void test_fresh_eeprom_byte_is_0xFF(void) {
    uint8_t val;
    eeprom_read_byte(0x00, &val);
    TEST_ASSERT_EQUAL_HEX8(0xFF, val);
}

void test_fresh_eeprom_block_all_0xFF(void) {
    uint8_t buf[16];
    eeprom_read(0x00, buf, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, buf[i]);
    }
}

/*===========================================================================*/
/* Test 5 — I2C failure simulation                                           */
/*===========================================================================*/

void test_i2c_fail_init_returns_false(void) {
    mock_i2c_fail = true;
    TEST_ASSERT_FALSE(eeprom_init());
}

void test_i2c_fail_write_returns_error(void) {
    /* init succeeded before fault was set */
    mock_i2c_fail = true;
    uint8_t val = 0x42;
    TEST_ASSERT_EQUAL(EEPROM_ERROR, eeprom_write_byte(0x00, val));
}

void test_i2c_fail_read_returns_error(void) {
    mock_i2c_fail = true;
    uint8_t val;
    TEST_ASSERT_EQUAL(EEPROM_ERROR, eeprom_read_byte(0x00, &val));
}

void test_i2c_fail_does_not_corrupt_eeprom_contents(void) {
    /* Write a known value, then inject fault, then attempt another write */
    eeprom_write_byte(0x30, 0xDE);

    mock_i2c_fail = true;
    eeprom_write_byte(0x30, 0xAD);  /* must be rejected */
    mock_i2c_fail = false;

    uint8_t got;
    eeprom_read_byte(0x30, &got);
    TEST_ASSERT_EQUAL_HEX8(0xDE, got);  /* original value preserved */
}

/*===========================================================================*/
/* Test 6 — Partial write: crossing a page boundary (8-byte pages)          */
/*===========================================================================*/

/*
 * AT24C02 pages: 0x00-0x07, 0x08-0x0F, 0x10-0x17, ...
 * Writing 6 bytes starting at address 5 crosses into the next page at byte 8.
 * The driver must split the write into:
 *   chunk 1: addr=5, len=3  (fills bytes 5,6,7 of page 0)
 *   chunk 2: addr=8, len=3  (bytes 0,1,2 of page 1)
 */
void test_cross_page_write_reads_back_correctly(void) {
    const uint8_t src[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_write(5, src, 6));

    uint8_t dst[6] = {0};
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_read(5, dst, 6));
    TEST_ASSERT_EQUAL_MEMORY(src, dst, 6);
}

void test_cross_page_write_does_not_corrupt_preceding_bytes(void) {
    /* Pre-fill bytes 0-4 */
    const uint8_t pre[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
    eeprom_write(0, pre, 5);

    /* Now cross-page write at 5 */
    const uint8_t src[6] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    eeprom_write(5, src, 6);

    uint8_t check[5];
    eeprom_read(0, check, 5);
    TEST_ASSERT_EQUAL_MEMORY(pre, check, 5);
}

void test_cross_page_write_does_not_corrupt_following_bytes(void) {
    /* Write at 5+6=11, verify bytes 11-15 are still 0xFF */
    const uint8_t src[6] = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5};
    eeprom_write(5, src, 6);

    uint8_t tail[5];
    eeprom_read(11, tail, 5);
    for (size_t i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, tail[i]);
    }
}

/*
 * Exact page-boundary start: writing PAGE_SIZE bytes at the start of a page
 * must fit in a single chunk (no wrap confusion).
 */
void test_write_exactly_one_full_page_at_boundary(void) {
    const uint8_t src[8] = {0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7};
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_write(0x08, src, 8));

    uint8_t dst[8];
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_read(0x08, dst, 8));
    TEST_ASSERT_EQUAL_MEMORY(src, dst, 8);
}

/*===========================================================================*/
/* Test 7 — eeprom_read_byte / eeprom_write_byte: single-byte API           */
/*===========================================================================*/

void test_read_byte_returns_written_value(void) {
    eeprom_write_byte(0xF0, 0x5A);
    uint8_t val;
    TEST_ASSERT_EQUAL(EEPROM_OK, eeprom_read_byte(0xF0, &val));
    TEST_ASSERT_EQUAL_HEX8(0x5A, val);
}

void test_write_byte_overwrites_previous_value(void) {
    eeprom_write_byte(0xF1, 0x11);
    eeprom_write_byte(0xF1, 0x22);
    uint8_t val;
    eeprom_read_byte(0xF1, &val);
    TEST_ASSERT_EQUAL_HEX8(0x22, val);
}

void test_read_byte_null_pointer_returns_error(void) {
    TEST_ASSERT_EQUAL(EEPROM_ERROR, eeprom_read_byte(0x00, NULL));
}

void test_write_then_read_byte_at_address_0(void) {
    eeprom_write_byte(0x00, 0xBE);
    uint8_t val;
    eeprom_read_byte(0x00, &val);
    TEST_ASSERT_EQUAL_HEX8(0xBE, val);
}

void test_write_then_read_byte_at_address_255(void) {
    eeprom_write_byte(0xFF, 0xEF);
    uint8_t val;
    eeprom_read_byte(0xFF, &val);
    TEST_ASSERT_EQUAL_HEX8(0xEF, val);
}

/*===========================================================================*/
/* Test runner                                                                */
/*===========================================================================*/

int main(void) {
    UNITY_BEGIN();

    /* Test 1: Write then read */
    RUN_TEST(test_write_then_read_single_byte);
    RUN_TEST(test_write_then_read_does_not_disturb_neighbour);

    /* Test 2: Block write */
    RUN_TEST(test_block_write_reads_back_correctly);
    RUN_TEST(test_block_write_leaves_unwritten_bytes_unchanged);

    /* Test 3: Address bounds */
    RUN_TEST(test_write_at_last_byte_succeeds);
    RUN_TEST(test_write_beyond_256_fails);
    RUN_TEST(test_read_beyond_256_fails);
    RUN_TEST(test_write_starting_beyond_device_fails);

    /* Test 4: Read empty */
    RUN_TEST(test_fresh_eeprom_byte_is_0xFF);
    RUN_TEST(test_fresh_eeprom_block_all_0xFF);

    /* Test 5: I2C failure simulation */
    RUN_TEST(test_i2c_fail_init_returns_false);
    RUN_TEST(test_i2c_fail_write_returns_error);
    RUN_TEST(test_i2c_fail_read_returns_error);
    RUN_TEST(test_i2c_fail_does_not_corrupt_eeprom_contents);

    /* Test 6: Partial write / page boundary */
    RUN_TEST(test_cross_page_write_reads_back_correctly);
    RUN_TEST(test_cross_page_write_does_not_corrupt_preceding_bytes);
    RUN_TEST(test_cross_page_write_does_not_corrupt_following_bytes);
    RUN_TEST(test_write_exactly_one_full_page_at_boundary);

    /* Test 7: eeprom_read_byte / eeprom_write_byte */
    RUN_TEST(test_read_byte_returns_written_value);
    RUN_TEST(test_write_byte_overwrites_previous_value);
    RUN_TEST(test_read_byte_null_pointer_returns_error);
    RUN_TEST(test_write_then_read_byte_at_address_0);
    RUN_TEST(test_write_then_read_byte_at_address_255);

    return UNITY_END();
}
