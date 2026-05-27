/**
 * @file test_main.c
 * @brief Tests for the REAL menu value-field formatter.
 *
 * Includes the shipping include/menu_format.h rather than a copy, so these
 * exercise the same code menu_draw() calls.
 *
 * The bug this suite exists for: v0.1.0 computed the bracket column as
 * `7 - len - 1` and then wrote '[' at `start - 1`, reaching buf[-1] for any
 * 6-character option. Every case here writes through a guarded buffer so an
 * overflow fails a test instead of quietly corrupting a neighbouring local —
 * which is exactly how it escaped review on the target.
 */

#include <unity.h>
#include <string.h>
#include "menu_format.h"

/* buf sits between two canary regions. menu_format_enum() must touch only the
 * 9 bytes it was given. */
#define PAD 8
static char arena[PAD + MENU_FIELD_WIDTH + 1 + PAD];
static char* buf;

void setUp(void) {
    memset(arena, '#', sizeof(arena));
    buf = arena + PAD;
}

void tearDown(void) {}

static void assert_canaries_intact(void) {
    for (int i = 0; i < PAD; i++) {
        TEST_ASSERT_EQUAL_MESSAGE('#', arena[i], "wrote before buf");
    }
    for (size_t i = PAD + MENU_FIELD_WIDTH + 1; i < sizeof(arena); i++) {
        TEST_ASSERT_EQUAL_MESSAGE('#', arena[i], "wrote past buf");
    }
}

static void render(const char* opt, bool editing) {
    menu_format_enum(buf, opt, editing);
    assert_canaries_intact();
}

/*--- the regression ------------------------------------------------------*/

/* "Plywoo" — Speed > Materl > Plywood, truncated to 6. This is the case that
 * wrote buf[-1]. */
static void test_six_char_option_editing_stays_in_bounds(void) {
    render("Plywood", true);
    TEST_ASSERT_EQUAL_STRING("[Plywoo]", buf);
}

/* The '[' used to land outside the buffer, so the operator saw a lone ']'. */
static void test_six_char_option_editing_shows_opening_bracket(void) {
    render("Acrylc", true);
    TEST_ASSERT_EQUAL('[', buf[0]);
    TEST_ASSERT_EQUAL(']', buf[MENU_FIELD_WIDTH - 1]);
}

/* The old arithmetic was one column left of the layout, leaving a gap at
 * column 6 between the text and the ']'. */
static void test_no_gap_before_closing_bracket(void) {
    render("SpdSpr", true);
    TEST_ASSERT_EQUAL_STRING("[SpdSpr]", buf);
    TEST_ASSERT_NOT_EQUAL(' ', buf[MENU_FIELD_WIDTH - 2]);
}

/*--- lengths -------------------------------------------------------------*/

static void test_every_length_editing_is_bracketed_and_right_aligned(void) {
    static const char* opts[] = {"A", "AB", "ABC", "ABCD", "ABCDE", "ABCDEF"};
    static const char* want[] = {"      [A]", "     [AB]", "    [ABC]",
                                 "   [ABCD]", "  [ABCDE]", " [ABCDEF]"};
    for (int i = 0; i < 6; i++) {
        render(opts[i], true);
        /* want[] carries a leading space that belongs to the field padding;
         * compare the 8 visible columns. */
        TEST_ASSERT_EQUAL_STRING(want[i] + 1, buf);
    }
}

static void test_every_length_not_editing_is_right_aligned(void) {
    static const char* opts[] = {"A", "AB", "ABC", "ABCD", "ABCDE", "ABCDEF"};
    static const char* want[] = {"       A", "      AB", "     ABC",
                                 "    ABCD", "   ABCDE", "  ABCDEF"};
    for (int i = 0; i < 6; i++) {
        render(opts[i], false);
        TEST_ASSERT_EQUAL_STRING(want[i], buf);
    }
}

/*--- truncation ----------------------------------------------------------*/

static void test_overlong_option_truncates_not_overflows(void) {
    render("Forstner", true);
    TEST_ASSERT_EQUAL_STRING("[Forstn]", buf);
}

static void test_overlong_option_truncates_when_not_editing(void) {
    render("Forstner", false);
    TEST_ASSERT_EQUAL_STRING("  Forstn", buf);
}

/*--- degenerate inputs ---------------------------------------------------*/

static void test_null_option_renders_blank_field(void) {
    render(NULL, false);
    TEST_ASSERT_EQUAL_STRING("        ", buf);
}

static void test_null_option_renders_blank_even_when_editing(void) {
    /* An out-of-range enum value reaches here as NULL; it must not draw a
     * half-open bracket pair. */
    render(NULL, true);
    TEST_ASSERT_EQUAL_STRING("        ", buf);
}

static void test_empty_option_renders_empty_brackets(void) {
    render("", true);
    TEST_ASSERT_EQUAL_STRING("      []", buf);
}

/*--- invariants ----------------------------------------------------------*/

static void test_field_is_always_exactly_eight_columns_and_terminated(void) {
    static const char* opts[] = {"", "A", "ABCDEF", "Forstner", NULL};
    for (int i = 0; i < 5; i++) {
        for (int e = 0; e < 2; e++) {
            render(opts[i], e != 0);
            TEST_ASSERT_EQUAL_size_t(MENU_FIELD_WIDTH, strlen(buf));
            TEST_ASSERT_EQUAL('\0', buf[MENU_FIELD_WIDTH]);
        }
    }
}

/* The clamp and the bracket arithmetic are one invariant: two columns of the
 * field are spent on brackets. If someone widens the text clamp without
 * widening the field, the editing form no longer fits. */
static void test_text_clamp_leaves_room_for_both_brackets(void) {
    TEST_ASSERT_EQUAL_INT(MENU_FIELD_WIDTH - 2, MENU_FIELD_TEXT_MAX);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_six_char_option_editing_stays_in_bounds);
    RUN_TEST(test_six_char_option_editing_shows_opening_bracket);
    RUN_TEST(test_no_gap_before_closing_bracket);
    RUN_TEST(test_every_length_editing_is_bracketed_and_right_aligned);
    RUN_TEST(test_every_length_not_editing_is_right_aligned);
    RUN_TEST(test_overlong_option_truncates_not_overflows);
    RUN_TEST(test_overlong_option_truncates_when_not_editing);
    RUN_TEST(test_null_option_renders_blank_field);
    RUN_TEST(test_null_option_renders_blank_even_when_editing);
    RUN_TEST(test_empty_option_renders_empty_brackets);
    RUN_TEST(test_field_is_always_exactly_eight_columns_and_terminated);
    RUN_TEST(test_text_clamp_leaves_room_for_both_brackets);
    return UNITY_END();
}
