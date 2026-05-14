/**
 * @file depth.h
 * @brief Depth Sensor Interface (ADC-based)
 *
 * Depth/quill position is read from a potentiometer on PC1 (ADC1 Channel 11).
 * The original Teknatool firmware uses DMA for continuous ADC reading.
 * This implementation uses polling with a simple low-pass filter.
 */

#ifndef DEPTH_H
#define DEPTH_H

#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

/**
 * @brief Initialize depth module and ADC hardware
 * Must be called before using other depth functions
 */
void depth_init(void);

/**
 * @brief Poll depth sensor (call periodically in main loop)
 * Reads ADC and applies low-pass filter to current_adc
 */
void depth_poll(void);

/**
 * @brief Get raw ADC value (0-4095)
 * @return Raw filtered ADC reading
 */
int16_t depth_get_raw(void);

/**
 * @brief Get current depth reading with calibration applied
 * @return Depth in 0.1mm units (positive = deeper)
 */
int16_t depth_get(void);

/**
 * @brief Reset depth counter to zero at current position
 */
void depth_reset(void);

/**
 * @brief Get depth change since last poll
 * @return Change in ADC counts
 */
int16_t depth_get_delta(void);

/**
 * @brief Check if depth is decreasing (quill rising/retracting)
 * @return true if ADC value is decreasing
 */
bool depth_is_decreasing(void);

/**
 * @brief Check if depth is increasing (quill lowering/drilling)
 * @return true if ADC value is increasing
 */
bool depth_is_increasing(void);

/**
 * @brief Get direction of movement
 * @return 1=up (retracting), -1=down (drilling), 0=stationary
 */
int8_t depth_get_direction(void);

/**
 * @brief Calibrate depth - store current position as zero reference
 * Call this when quill is at a known reference position
 */
void depth_calibrate(void);

/**
 * @brief Set calibration offset (call after loading from settings)
 * @param offset Offset in ADC counts to apply
 */
void depth_set_offset(int16_t offset);

/**
 * @brief Get calibration offset
 * @return Current offset in ADC counts
 */
int16_t depth_get_offset(void);

/**
 * @brief Check if depth has been calibrated
 * @return true if calibration has been performed
 */
bool depth_is_calibrated(void);

/**
 * @brief Check if ADC is initialized
 * @return true if depth_init() succeeded
 */
bool depth_is_initialized(void);

/**
 * @brief Check if ADC has detected a hardware fault
 *
 * Faults detected:
 * - ADC reading out of bounds (< 10 or > 4090)
 * - ADC stuck at same value for 10+ consecutive reads
 * - ADC timeout for 3+ consecutive conversions
 *
 * @return true if ADC fault detected (sensor failure)
 */
bool depth_has_fault(void);

/**
 * @brief Clear ADC fault flag
 * Call this after fixing hardware issue or sensor reconnection
 */
void depth_clear_fault(void);

#endif /* DEPTH_H */
