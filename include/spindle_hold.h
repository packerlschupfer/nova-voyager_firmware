/**
 * @file spindle_hold.h
 * @brief Spindle Hold Module - Powered Position Lock
 *
 * Phase 2.1: Extracted from task_motor.c (lines 549-664)
 * Discovered via logic analyzer 2026-01-24
 *
 * Spindle hold uses low-power motor engagement to lock spindle position
 * while stopped. Two modes:
 * - Manual hold: CL=10%, no timeout (user-initiated)
 * - Safety hold: CL=12%, 2s timeout (E-Stop/Guard triggered)
 */

#ifndef SPINDLE_HOLD_H
#define SPINDLE_HOLD_H

#include <stdbool.h>
#include <stdint.h>

/*===========================================================================*/
/* Public API                                                                 */
/*===========================================================================*/

/**
 * @brief Start manual spindle hold (CL=10%, no timeout)
 * @param is_safety If true, use safety parameters (CL=12%, 2s timeout)
 */
void spindle_hold_start(bool is_safety);

/**
 * @brief Release spindle hold and return to normal operation
 */
void spindle_hold_release(void);

/**
 * @brief Update spindle hold state (call periodically from motor task)
 * Handles:
 * - Safety hold timeout (auto-release after 2s)
 * - Hold maintenance cycle (repeat commands every 460ms)
 */
void spindle_hold_update(void);

/**
 * @brief Check if spindle hold is currently active
 * @return true if hold is active, false otherwise
 */
bool spindle_hold_is_active(void);

#endif // SPINDLE_HOLD_H
