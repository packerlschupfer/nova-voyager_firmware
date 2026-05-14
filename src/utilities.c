/**
 * @file utilities.c
 * @brief Common Utility Functions Implementation
 *
 * Phase 3.3: Created to consolidate duplicated code patterns
 */

#include "utilities.h"

int int_to_decimal_str(uint32_t value, char* buf) {
    int len = 0;

    // Handle zero specially
    if (value == 0) {
        buf[len++] = '0';
        return len;
    }

    // Convert to ASCII digits (reversed - LSB first)
    while (value > 0 && len < 10) {
        buf[len++] = '0' + (value % 10);
        value /= 10;
    }

    return len;
}
