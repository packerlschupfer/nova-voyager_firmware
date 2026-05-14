/**
 * @file temperature.h
 * @brief Temperature Monitoring - GD32 Internal & MCB Heatsink
 *
 * Phase 2.2: Expanded to include MCB temperature monitoring
 */

#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* GD32 Internal Temperature Sensor (HMI Board CPU)                          */
/*===========================================================================*/

/**
 * @brief Initialize GD32 internal temperature sensor (ADC CH16)
 */
void temperature_init(void);

/**
 * @brief Read GD32 internal temperature in °C
 * @return Temperature in °C (typical 15-50°C), or 0 if not ready
 *
 * This reads the HMI board CPU temperature, which matches the
 * original firmware "T Heatsink" display (~18-30°C at room temp).
 */
uint16_t temperature_read_gd32(void);

/*===========================================================================*/
/* MCB Temperature Monitoring (Phase 2.2: Extracted from task_motor.c)      */
/*===========================================================================*/

/**
 * @brief Query MCB heatsink temperature via motor protocol
 * Sends T0 command to motor controller and caches result
 */
void temp_query_mcb(void);

/**
 * @brief Get cached MCB heatsink temperature
 * @return Temperature in °C, or GD32 temp if MCB never queried
 */
uint16_t temp_get_mcb(void);

/**
 * @brief Update temperature monitoring (call periodically from motor task)
 * Checks temperature against threshold with hysteresis and manages warnings
 *
 * @param current_temp Current MCB temperature in °C
 * @param threshold Temperature threshold for warning (0 = use default)
 */
void temp_monitor_update(uint16_t current_temp, uint8_t threshold);

/**
 * @brief Check if temperature warning is currently active
 * @return true if warning is active, false otherwise
 */
bool temp_is_warning_active(void);

#endif // TEMPERATURE_H
