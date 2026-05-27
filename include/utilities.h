/**
 * @file utilities.h
 * @brief Common Utility Functions
 *
 * Phase 3.3: Created to consolidate duplicated code patterns
 */

#ifndef UTILITIES_H
#define UTILITIES_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Number of elements in a real array (NOT a pointer).
 *
 * Use this instead of writing the length out by hand next to the array.
 * A hand-maintained count is a second source of truth that silently rots the
 * moment someone edits the array: menu.c shipped `TAP_MENU_COUNT 13` against a
 * 12-entry `tap_menu[]` after a row was deleted, and the extra index landed on
 * the next table in .data — a phantom menu row that edited a variable the
 * operator never opened.
 *
 * The `(void)sizeof(struct{int _[1 - 2*!!__builtin_types_compatible_p(
 * __typeof__(a), __typeof__(&(a)[0]))];})` idiom is the usual way to reject a
 * decayed pointer at compile time, but it is a GCC extension; this codebase is
 * GCC-only (arm-none-eabi-gcc) so it is safe here, and it is what turns a
 * silent wrong answer into a build error.
 */
#define ARRAY_COUNT(a)                                                        \
    (sizeof(a) / sizeof((a)[0]) +                                             \
     0 * sizeof(struct {                                                      \
         int reject_pointer_arguments[1 - 2 * !!__builtin_types_compatible_p( \
             __typeof__(a), __typeof__(&(a)[0]))];                            \
     }))

/* Largest number of digits int_to_decimal_str() can write: UINT32_MAX is ten
 * digits, and its loop stops there. */
#define INT_DECIMAL_STR_MAX 10

/**
 * @brief Convert integer to decimal ASCII string (reversed in buffer)
 * @param value Integer value to convert
 * @param buf Output buffer, at least INT_DECIMAL_STR_MAX bytes
 * @return Number of digits written to buffer
 *
 * REVIEW FIX: this used to document "at least 6 bytes" while the writer's own
 * loop bound is `len < 10` — a contract that was simply wrong for what the
 * function can produce, and one caller sized its buffer from it and could be
 * made to overflow. The bound is now a named constant both sides use.
 *
 * Digits are written in REVERSE order (LSB first) for efficient output reversal.
 * Example: int_to_decimal_str(123, buf) writes "321" to buf and returns 3
 *
 * Usage pattern:
 *   char buf[8];
 *   int len = int_to_decimal_str(value, buf);
 *   for (int i = len - 1; i >= 0; i--) {
 *       output(buf[i]);  // Output in correct order
 *   }
 */
int int_to_decimal_str(uint32_t value, char* buf);

#endif // UTILITIES_H
