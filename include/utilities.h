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
 * @brief Convert integer to decimal ASCII string (reversed in buffer)
 * @param value Integer value to convert
 * @param buf Output buffer for ASCII digits (must be at least 6 bytes)
 * @return Number of digits written to buffer
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
