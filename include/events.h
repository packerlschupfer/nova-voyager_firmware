/**
 * @file events.h
 * @brief Event handling for button and system events
 *
 * Handles all UI events (buttons, encoder, motor faults) and updates system state
 * Extracted from main.c for better code organization
 */

#ifndef EVENTS_H
#define EVENTS_H

#include "shared.h"

/**
 * @brief Handle a system event
 *
 * Processes events from the event queue and updates system state:
 * - Button presses (START, MENU, ZERO, F1-F4)
 * - Encoder rotation (speed adjustment)
 * - Motor faults and jam detection
 * - Safety interlocks (guard, E-stop)
 *
 * @param evt Event type to handle
 */
void handle_event(event_type_t evt);

/**
 * @brief Get variable speed step for encoder/button adjustment
 *
 * Returns speed step size that varies based on current RPM for better UX:
 * - <200 RPM: 5 (fine) / 20 (coarse)
 * - <500 RPM: 5 / 50
 * - <1000 RPM: 5 / 100
 * - <3000 RPM: 10 / 200
 * - >=3000 RPM: 20 / 400
 *
 * @param rpm Current RPM value
 * @param coarse True for coarse adjustment (F1 button), false for fine (encoder)
 * @return Step size in RPM
 */
uint16_t get_speed_step(uint16_t rpm, bool coarse);

#endif // EVENTS_H
