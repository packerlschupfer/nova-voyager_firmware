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
 * @brief Reset depth counter to zero at current position
 */
void depth_reset(void);

/**
 * @brief Get depth change since last poll
 * @return Change in ADC counts
 */
int16_t depth_get_delta(void);

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

#endif /* DEPTH_H */
