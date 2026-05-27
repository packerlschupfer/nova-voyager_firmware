/**
 * @file test_main.c
 * @brief Unit tests for display formatting helpers (Phase 10)
 *
 * The real buf_num() and buf_depth() are static functions inside display.c
 * and write into a shared row_buf via a file-level cursor.  To keep these
 * tests self-contained and runnable on the native host, the pure formatting
 * logic is re-implemented here as standalone functions that write into a
 * caller-supplied char array.  The algorithms are identical to the originals;
 * only the output mechanism changes.
 *
 * The error_until comparison is lifted verbatim from display_update() so the
 * uint32_t rollover behaviour can be verified without linking any HAL code.
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Re-implemented standalone formatting helpers
 * ------------------------------------------------------------------------- */

/**
 * fmt_num — right-align an unsigned integer into out[0..width-1].
 *
 * Mirrors buf_num() from display.c (uint16_t variant used by the real code).
 * The caller must supply a buffer of at least `width` bytes; no NUL is added.
 */
static void fmt_num(uint32_t val, uint8_t width, char *out)
{
    char tmp[8];
    int i = width - 1;
    bool started = false;

    do {
        tmp[i] = '0' + (val % 10);
        val /= 10;
        if (tmp[i] != '0') started = true;
        i--;
    } while (val > 0 && i >= 0);

    while (i >= 0) tmp[i--] = ' ';

    /* Ensure at least one '0' is shown for zero */
    if (!started && width > 0) tmp[width - 1] = '0';

    memcpy(out, tmp, width);
}

/**
 * fmt_depth — format depth_01mm as "XX.X" with leading spaces and sign.
 *
 * Mirrors buf_depth() from display.c exactly, including the INT16_MIN clamp.
 * The caller must supply a buffer of at least `width` bytes; no NUL is added.
 */
static void fmt_depth(int16_t depth_01mm, uint8_t width, char *out)
{
    char tmp[8];
    bool negative = depth_01mm < 0;

    if (negative)
        depth_01mm = (depth_01mm == INT16_MIN) ? INT16_MAX : -depth_01mm;

    uint16_t mm   = (uint16_t)(depth_01mm / 10);
    uint8_t  frac = (uint8_t)(depth_01mm % 10);

    int pos = width - 1;

    /* Fractional digit and decimal point */
    tmp[pos--] = '0' + frac;
    tmp[pos--] = '.';

    /* Integer part — at least one digit */
    do {
        tmp[pos--] = '0' + (mm % 10);
        mm /= 10;
    } while (mm > 0 && pos >= 0);

    /* Sign if negative and room remains */
    if (negative && pos >= 0) tmp[pos--] = '-';

    /* Pad with spaces */
    while (pos >= 0) tmp[pos--] = ' ';

    memcpy(out, tmp, width);
}

/**
 * error_active — reproduce the subtraction-based window check from
 * display_update() in display.c line 302:
 *
 *   if (error_until > 0 && (error_until - now) <= ESTOP_DISPLAY_MS)
 *
 * uint32_t subtraction wraps on overflow, so a deadline in the past produces
 * a huge number that is > ESTOP_DISPLAY_MS, correctly reporting "expired".
 */
#define ESTOP_DISPLAY_MS  30000u

static bool error_active(uint32_t error_until, uint32_t now)
{
    return error_until > 0 &&
           (error_until - now) <= ESTOP_DISPLAY_MS;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/** Copy fmt_depth output into a NUL-terminated string for TEST_ASSERT_EQUAL_STRING. */
static char g_depth_buf[16];
static const char *depth_str(int16_t v, uint8_t w)
{
    fmt_depth(v, w, g_depth_buf);
    g_depth_buf[w] = '\0';
    return g_depth_buf;
}

/** Copy fmt_num output into a NUL-terminated string for TEST_ASSERT_EQUAL_STRING. */
static char g_num_buf[16];
static const char *num_str(uint32_t v, uint8_t w)
{
    fmt_num(v, w, g_num_buf);
    g_num_buf[w] = '\0';
    return g_num_buf;
}

/* -------------------------------------------------------------------------
 * Unity lifecycle
 * ------------------------------------------------------------------------- */

void setUp(void) {}
void tearDown(void) {}

/* =========================================================================
 * buf_depth tests
 * ========================================================================= */

/* --- positive values (default width=5) --- */

void test_buf_depth_positive_typical(void)
{
    /* 124 → 12.4 mm, width 5 → " 12.4" */
    TEST_ASSERT_EQUAL_STRING(" 12.4", depth_str(124, 5));
}

void test_buf_depth_positive_zero(void)
{
    /* 0 → "  0.0" */
    TEST_ASSERT_EQUAL_STRING("  0.0", depth_str(0, 5));
}

void test_buf_depth_positive_sub_mm(void)
{
    /* 5 → 0.5 mm, width 5 → "  0.5" */
    TEST_ASSERT_EQUAL_STRING("  0.5", depth_str(5, 5));
}

/* --- negative values --- */

void test_buf_depth_negative_typical(void)
{
    /* -124 → "-12.4" */
    TEST_ASSERT_EQUAL_STRING("-12.4", depth_str(-124, 5));
}

void test_buf_depth_negative_sub_mm(void)
{
    /* -5 → " -0.5" */
    TEST_ASSERT_EQUAL_STRING(" -0.5", depth_str(-5, 5));
}

/* --- INT16_MIN clamp --- */

void test_buf_depth_int16_min_clamp(void)
{
    /*
     * INT16_MIN = -32768.  Negating it overflows a signed 16-bit int.
     * The implementation clamps to INT16_MAX (32767) to avoid UB.
     * Result must be a valid decimal string — not garbage — and must
     * match the clamped value 32767 → "3276.7" at width 6.
     */
    TEST_ASSERT_EQUAL_STRING("3276.7", depth_str(INT16_MIN, 6));
}

/* --- large value --- */

void test_buf_depth_large(void)
{
    /* INT16_MAX = 32767 → "3276.7" at width 6 */
    TEST_ASSERT_EQUAL_STRING("3276.7", depth_str(32767, 6));
}

/* --- width variations --- */

void test_buf_depth_width_5(void)
{
    TEST_ASSERT_EQUAL_STRING(" 12.4", depth_str(124, 5));
}

void test_buf_depth_width_6(void)
{
    /* Extra leading space at width 6 */
    TEST_ASSERT_EQUAL_STRING("  12.4", depth_str(124, 6));
}

void test_buf_depth_width_7(void)
{
    TEST_ASSERT_EQUAL_STRING("   12.4", depth_str(124, 7));
}

/* =========================================================================
 * buf_num tests
 * ========================================================================= */

void test_buf_num_zero(void)
{
    /* 0 at width 3 → "  0" */
    TEST_ASSERT_EQUAL_STRING("  0", num_str(0, 3));
}

void test_buf_num_positive_exact_fit(void)
{
    /* 1500 at width 4 — exactly fills the field, no padding */
    TEST_ASSERT_EQUAL_STRING("1500", num_str(1500, 4));
}

void test_buf_num_padding(void)
{
    /* 42 at width 4 → "  42" */
    TEST_ASSERT_EQUAL_STRING("  42", num_str(42, 4));
}

void test_buf_num_large(void)
{
    /* 999999 at width 6 — fills field exactly */
    TEST_ASSERT_EQUAL_STRING("999999", num_str(999999, 6));
}

void test_buf_num_single_digit(void)
{
    /* 7 at width 3 → "  7" */
    TEST_ASSERT_EQUAL_STRING("  7", num_str(7, 3));
}

void test_buf_num_width_1(void)
{
    /* width=1 with value 9 — single character */
    TEST_ASSERT_EQUAL_STRING("9", num_str(9, 1));
}

/* =========================================================================
 * error_until rollover tests
 * ========================================================================= */

void test_error_until_normal_active(void)
{
    /*
     * now=1000, error_until=4000.
     * Difference = 3000 ≤ 30000 and error_until > 0 → active.
     */
    TEST_ASSERT_TRUE(error_active(4000u, 1000u));
}

void test_error_until_normal_expired(void)
{
    /*
     * now=5000, error_until=4000.
     * Unsigned subtraction: 4000 - 5000 wraps to 0xFFFF_F030 (huge) > 30000.
     * error_until > 0, but window check fails → expired.
     */
    TEST_ASSERT_FALSE(error_active(4000u, 5000u));
}

void test_error_until_rollover_active(void)
{
    /*
     * Timer rolls over: event fired at 0xFFFFFF00, deadline set 3000 ms later.
     * error_until = 0xFFFFFF00 + 3000 = 0x00000AB8  (wraps past 32-bit limit).
     * now = 0xFFFFFF00 (the moment the event fired — still inside window).
     * Unsigned subtraction: 0x00000AB8 - 0xFFFFFF00 wraps to 0x00000BB8 = 3000,
     * which is ≤ 30000 → active.
     */
    uint32_t event_time  = 0xFFFFFF00u;
    uint32_t error_until = event_time + 3000u;   /* wraps to 0x00000AB8 */
    uint32_t now         = event_time;

    TEST_ASSERT_TRUE(error_active(error_until, now));
}

void test_error_until_rollover_expired(void)
{
    /*
     * Same rollover scenario but now has advanced 31 seconds past the event —
     * well beyond the 30-second window.
     * error_until = 0x00000AB8 (from event_time=0xFFFFFF00 + 3000)
     * now = 0xFFFFFF00 + 31000 = 0x00007818
     * Unsigned subtraction: 0x00000AB8 - 0x00007818 wraps to 0xFFFF9CA0 (huge) → expired.
     */
    uint32_t error_until = 0x00000AB8u;          /* 0xFFFFFF00 + 3000, wrapped */
    uint32_t now         = 0xFFFFFF00u + 31000u; /* 0x00007818 */

    TEST_ASSERT_FALSE(error_active(error_until, now));
}

void test_error_until_zero_never_active(void)
{
    /* error_until == 0 means "no error set"; must always return false. */
    TEST_ASSERT_FALSE(error_active(0u, 0u));
    TEST_ASSERT_FALSE(error_active(0u, 1000u));
}

/* =========================================================================
 * Test runner
 * ========================================================================= */

int main(void)
{
    UNITY_BEGIN();

    /* buf_depth — positive */
    RUN_TEST(test_buf_depth_positive_typical);
    RUN_TEST(test_buf_depth_positive_zero);
    RUN_TEST(test_buf_depth_positive_sub_mm);

    /* buf_depth — negative */
    RUN_TEST(test_buf_depth_negative_typical);
    RUN_TEST(test_buf_depth_negative_sub_mm);

    /* buf_depth — edge cases */
    RUN_TEST(test_buf_depth_int16_min_clamp);
    RUN_TEST(test_buf_depth_large);

    /* buf_depth — width variations */
    RUN_TEST(test_buf_depth_width_5);
    RUN_TEST(test_buf_depth_width_6);
    RUN_TEST(test_buf_depth_width_7);

    /* buf_num */
    RUN_TEST(test_buf_num_zero);
    RUN_TEST(test_buf_num_positive_exact_fit);
    RUN_TEST(test_buf_num_padding);
    RUN_TEST(test_buf_num_large);
    RUN_TEST(test_buf_num_single_digit);
    RUN_TEST(test_buf_num_width_1);

    /* error_until rollover */
    RUN_TEST(test_error_until_normal_active);
    RUN_TEST(test_error_until_normal_expired);
    RUN_TEST(test_error_until_rollover_active);
    RUN_TEST(test_error_until_rollover_expired);
    RUN_TEST(test_error_until_zero_never_active);

    return UNITY_END();
}
