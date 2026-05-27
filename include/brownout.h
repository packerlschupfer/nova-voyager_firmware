/**
 * @file brownout.h
 * @brief Programmable Voltage Detector (PVD) — brown-out interlock.
 *
 * WHY THIS EXISTS
 * ---------------
 * Observed on the machine 2026-08-30: powering the drill press down produces a
 * burst of watchdog resets. As the 3.3 V rail decays the core keeps executing
 * unreliably, tasks stop meeting their deadlines, the IWDG fires, it reboots,
 * and the cycle repeats until the rail is finally gone. The reset log showed
 * five in a row before an actual power-on.
 *
 * The log noise is harmless. What is NOT harmless is the window: the IWDG takes
 * up to ~5 s to notice, and for that whole time an unreliable core is running
 * with PD4 — the motor enable line — potentially still HIGH. The GD32's own
 * power-on reset only trips near ~1.8 V, far below the point where software
 * stops being trustworthy.
 *
 * The PVD closes that: it interrupts the moment VDD falls through a threshold
 * we choose, and the handler drops PD4 in hardware immediately — the same
 * cutoff the E-Stop and guard ISRs perform — instead of waiting for a watchdog
 * that may be several seconds away.
 */

#ifndef BROWNOUT_H
#define BROWNOUT_H

#include <stdbool.h>

/**
 * @brief Enable the PVD and its interrupt. Call once, early in main().
 *
 * Safe to call before the scheduler: it touches only RCC, PWR, EXTI and NVIC.
 */
void brownout_init(void);

/**
 * @brief True once VDD has fallen through the PVD threshold since boot.
 *
 * Latched deliberately: a supply that has sagged that far is not one to start a
 * spindle on, and a genuine power-down completes regardless. Cleared only by a
 * reset. Consulted by safety_can_start_motor().
 */
bool brownout_latched(void);

#endif /* BROWNOUT_H */
