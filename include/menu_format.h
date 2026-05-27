/**
 * @file menu_format.h
 * @brief Right-aligned 8-column value formatting for the menu's value field.
 *
 * Header-only and hardware-free on purpose, so the real code can be unit
 * tested on the host (test/test_menu_format) instead of only being exercised
 * by eye on the LCD. Same reasoning as include/safety.h: a copy of the logic
 * in a test would only prove the copy right.
 *
 * WHY IT EXISTS
 * -------------
 * This arithmetic lived inline in menu_draw()'s MENU_ENUM case and shipped in
 * v0.1.0 with an out-of-bounds write: the bracket position was computed as
 * `7 - len - 1` and the '[' was then placed at `start - 1`, i.e. buf[-1] for
 * any 6-character option. It was reachable in two clicks (Speed > Materl >
 * "Plywood") and repeated on every 100 ms redraw while the row stayed
 * selected. The '[' landed outside the buffer so it never rendered, which is
 * why the symptom was only a lone ']' and the bug survived visual testing.
 *
 * THE LAYOUT
 * ----------
 * The field is exactly 8 columns, buf[0..7], NUL at buf[8].
 *
 *   not editing:  "  Plywoo"     text right-aligned to column 7
 *   editing:      "[Plywoo]"     '[' at 7-len-1, text at (7-len)..6, ']' at 7
 *
 * The option text is clamped to MENU_FIELD_TEXT_MAX = 6 because the editing
 * form spends two of the eight columns on the brackets. That clamp and the
 * bracket arithmetic are one invariant: widening either alone writes outside
 * the buffer. With len <= 6, the '[' index 7-len-1 is >= 0.
 */

#ifndef MENU_FORMAT_H
#define MENU_FORMAT_H

#include <stdbool.h>

/** Visible width of the value field, excluding the NUL. */
#define MENU_FIELD_WIDTH 8

/** Longest option text that still leaves room for '[' and ']'. */
#define MENU_FIELD_TEXT_MAX (MENU_FIELD_WIDTH - 2)

/**
 * @brief Render an enum option right-aligned into an 8-column field.
 *
 * @param buf     Destination, at least MENU_FIELD_WIDTH + 1 bytes. Fully
 *                written: padded with spaces and NUL-terminated.
 * @param opt     Option text. May be NULL, which renders as blanks. Text
 *                longer than MENU_FIELD_TEXT_MAX is truncated, not wrapped.
 * @param editing True to draw the bracketed editing form.
 */
static inline void menu_format_enum(char* buf, const char* opt, bool editing) {
    for (int i = 0; i < MENU_FIELD_WIDTH; i++) {
        buf[i] = ' ';
    }
    buf[MENU_FIELD_WIDTH] = '\0';

    if (!opt) {
        return;
    }

    int len = 0;
    while (opt[len] && len < MENU_FIELD_TEXT_MAX) {
        len++;
    }

    /* Editing reserves the last column for ']', so the text ends at column 6
     * instead of 7. start >= 1 for every len <= MENU_FIELD_TEXT_MAX, which is
     * what keeps buf[start - 1] inside the buffer. */
    const int start = editing ? (MENU_FIELD_WIDTH - 1 - len)
                              : (MENU_FIELD_WIDTH - len);

    if (editing) {
        buf[start - 1] = '[';
        buf[MENU_FIELD_WIDTH - 1] = ']';
    }
    for (int i = 0; i < len; i++) {
        buf[start + i] = opt[i];
    }
}

#endif /* MENU_FORMAT_H */
