/**
 * @file temperature.c
 * @brief Temperature Monitoring - GD32 Internal & MCB Heatsink
 *
 * Phase 2.2: Expanded to include MCB temperature monitoring
 * Reads HMI board CPU temperature via GD32F303 internal sensor.
 * Monitors MCB heatsink temperature via motor protocol (T0 command).
 */

#include "temperature.h"
#include "config.h"
#include "motor.h"
#include "shared.h"
#include "stm32f1xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

// Temperature monitoring constants (from task_motor.c)
#define TEMP_WARNING_DEFAULT    60  // Default warning threshold (°C)
#define TEMP_HYSTERESIS         5   // Must drop 5°C below warning to clear

// External UART function for logging
extern void uart_puts(const char* s);

// GD32F303 temperature sensor calibration
// Datasheet: V25 = 1.40-1.50V (typ 1.43V), Slope = 4.0-4.6 mV/°C (typ 4.3)
#define V25_MV          1430    // Voltage at 25°C
#define AVG_SLOPE_MV_X10  43    // Slope * 10 (4.3 mV/°C)

// [MODULE_LOCAL] Phase 5.2: Init flag, safe without mutex (idempotent init)
static bool temp_sensor_initialized = false;

/**
 * @brief Initialize GD32 internal temperature sensor
 */
void temperature_init(void) {
    if (temp_sensor_initialized) return;

    // Enable ADC1 clock
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    // Power on ADC
    ADC1->CR2 = 0;
    ADC1->CR2 = ADC_CR2_ADON;

    // Wait for ADC to power up (t_stab = 1μs typical)
    for (volatile int i = 0; i < 200; i++);  // ~2μs at 120MHz

    // Enable temperature sensor and Vrefint
    ADC1->CR2 |= ADC_CR2_TSVREFE;

    // CRITICAL: Wait for temperature sensor to stabilize
    // Datasheet: t_START = 10μs typical
    // At 120MHz: 10μs = 1200 cycles
    for (volatile int i = 0; i < 2000; i++);  // ~17μs to be safe

    // Calibrate ADC
    ADC1->CR2 |= ADC_CR2_CAL;
    while (ADC1->CR2 & ADC_CR2_CAL);

    // Set long sampling time for CH16 (temperature sensor)
    ADC1->SMPR1 = 0x00FFFFFF;  // All channels on SMPR1: 239.5 cycles

    temp_sensor_initialized = true;
}

/**
 * @brief Read GD32 internal temperature in °C
 * @return Temperature in °C (15-50 typical), or 0 if sensor not ready
 */
uint16_t temperature_read_gd32(void) {
    if (!temp_sensor_initialized) {
        temperature_init();
    }

    // Select channel 16 (temperature sensor)
    ADC1->SQR3 = 16;

    // Start conversion
    ADC1->CR2 |= ADC_CR2_ADON;

    // Wait for conversion (timeout protection)
    uint32_t timeout = 100000;
    while (!(ADC1->SR & ADC_SR_EOC) && timeout--);

    if (timeout == 0) {
        return 0;  // Timeout
    }

    // Read ADC value
    uint16_t adc_value = ADC1->DR;

    // Convert to millivolts (3.3V reference, 12-bit ADC)
    uint32_t voltage_mv = (adc_value * 3300) / 4096;

    // Calculate temperature
    // GD32 has NEGATIVE temperature coefficient (voltage decreases as temp increases)
    // Formula: Temp = 25 + (V25 - Vsense) / Slope
    //               = 25 + (1430 - voltage_mv) / 4.3
    //               = 25 + ((1430 - voltage_mv) * 10) / 43
    int32_t temp_c = 25 + ((V25_MV - voltage_mv) * 10) / AVG_SLOPE_MV_X10;

    // Sanity check
    if (temp_c < -40 || temp_c > 125) {
        return 0;  // Invalid reading
    }

    return (uint16_t)temp_c;
}

/*===========================================================================*/
/* MCB Temperature Monitoring (Phase 2.2)                                    */
/*===========================================================================*/

// [MODULE_LOCAL] Phase 5.2: Only accessed from motor task via public API
// No mutex needed - all calls from single task context
static uint16_t mcb_temp_cached = 0;        // Cached MCB heatsink temperature
static bool temp_warning_active = false;    // Warning state

void temp_query_mcb(void) {
    // Query MCB heatsink temperature via T0 command
    int32_t temp = motor_read_param(CMD_T0);
    if (temp > 0 && temp < 150) {
        mcb_temp_cached = (uint16_t)temp;
    }
}

uint16_t temp_get_mcb(void) {
    // Return cached MCB temperature, or fall back to GD32 if never queried
    if (mcb_temp_cached > 0 && mcb_temp_cached < 150) {
        return mcb_temp_cached;
    }
    return temperature_read_gd32();  // Fallback to HMI board temp
}

void temp_monitor_update(uint16_t current_temp, uint8_t threshold) {
    // Use default threshold if not specified
    if (threshold == 0) {
        threshold = TEMP_WARNING_DEFAULT;
    }

    // Update cached value
    if (current_temp > 0 && current_temp < 150) {
        mcb_temp_cached = current_temp;
    }

    // Check temperature against threshold with hysteresis
    if (current_temp == 0) {
        // Invalid temperature - ignore
    } else if (current_temp >= threshold) {
        // Over threshold - trigger warning
        if (!temp_warning_active) {
            temp_warning_active = true;
            SEND_EVENT(EVT_TEMP_WARNING);
        }
    } else if (current_temp < threshold - TEMP_HYSTERESIS) {
        // Below threshold (with hysteresis) - clear warning
        temp_warning_active = false;
    }
}

bool temp_is_warning_active(void) {
    return temp_warning_active;
}
