/**
 * @file depth.c
 * @brief Depth Sensor Implementation (ADC-based)
 *
 * Depth/quill position is read from potentiometer on PC1 (ADC1 Channel 11).
 * ADC range observed: ~44-178 counts when moving quill.
 * Calibration allows setting current position as zero reference.
 */

#include "depth.h"
#include "config.h"
#include "stm32f1xx_hal.h"
#include <stdbool.h>

/*===========================================================================*/
/* Configuration                                                             */
/*===========================================================================*/

// ADC to depth conversion factor
// Observed: ~134 counts for full quill travel
// Assuming ~50mm travel = 500 units (0.1mm each)
// Scale factor: 500/134 ≈ 3.7 -> use fixed point: 37/10
#define DEPTH_SCALE_NUM     37
#define DEPTH_SCALE_DEN     10

// Low-pass filter coefficient (0-255, higher = more smoothing)
// 240 = very smooth, ~16 samples to settle
// 200 = moderate smoothing
// Original firmware likely uses heavy averaging via DMA
#define LPF_ALPHA           240

// ADC validation constants
#define ADC_MIN_VALID       10      // Minimum expected ADC value (sensor connected)
#define ADC_MAX_VALID       4090    // Maximum expected ADC value (12-bit ADC)
#define ADC_STUCK_THRESHOLD 10      // Consecutive identical reads = stuck sensor
#define ADC_TIMEOUT_MAX     3       // Max consecutive timeouts before fault

/*===========================================================================*/
/* Private Variables                                                         */
/*===========================================================================*/

static volatile uint16_t raw_adc = 0;       // Raw ADC reading
static int32_t filtered_adc = 0;            // Low-pass filtered (x256)
static int16_t current_depth = 0;           // Current depth in 0.1mm
static int16_t last_depth = 0;              // Previous depth for delta
static int16_t calibration_offset = 0;      // Zero reference ADC value

// ADC health monitoring
static uint16_t last_raw_adc = 0;           // Previous raw reading
static uint8_t adc_stuck_count = 0;         // Consecutive identical readings
static uint8_t adc_timeout_count = 0;       // Consecutive timeouts
static bool adc_fault = false;              // ADC hardware fault detected
static bool calibrated = false;
static bool initialized = false;

// ADC registers (use CMSIS ADC1_BASE if defined)
#ifndef ADC1_BASE
#define ADC1_BASE       0x40012400
#endif
#define ADC1_SR         (*(volatile uint32_t*)(ADC1_BASE + 0x00))
#define ADC1_CR1        (*(volatile uint32_t*)(ADC1_BASE + 0x04))
#define ADC1_CR2        (*(volatile uint32_t*)(ADC1_BASE + 0x08))
#define ADC1_SMPR1      (*(volatile uint32_t*)(ADC1_BASE + 0x0C))
#define ADC1_SQR3       (*(volatile uint32_t*)(ADC1_BASE + 0x34))
#define ADC1_DR         (*(volatile uint32_t*)(ADC1_BASE + 0x4C))

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

void depth_init(void) {
    // Enable ADC1 clock
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    // Enable GPIOC clock (should already be enabled)
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;

    // Configure PC1 as analog input (CNF=00, MODE=00)
    GPIOC->CRL &= ~(0xF << 4);  // Clear PC1 config bits

    // Small delay for clock stabilization
    for (volatile int i = 0; i < 1000; i++);

    // Power on ADC
    ADC1_CR2 = (1 << 0);  // ADON
    for (volatile int i = 0; i < 10000; i++);  // Stabilization delay

    // Configure sample time for channel 11 (PC1)
    // SMPR1 bits 3-5: SMP11 = 6 (71.5 cycles for stable reading)
    ADC1_SMPR1 = (ADC1_SMPR1 & ~(7 << 3)) | (6 << 3);

    // Select channel 11 in sequence register
    ADC1_SQR3 = 11;

    // Calibration reset
    ADC1_CR2 |= (1 << 3);  // RSTCAL
    while (ADC1_CR2 & (1 << 3));

    // Run calibration
    ADC1_CR2 |= (1 << 2);  // CAL
    while (ADC1_CR2 & (1 << 2));

    // Take initial reading
    ADC1_CR2 |= (1 << 0);  // Start conversion
    while (!(ADC1_SR & (1 << 1)));  // Wait for EOC
    raw_adc = ADC1_DR & 0xFFF;

    // Initialize filter with current value
    filtered_adc = raw_adc << 8;
    current_depth = 0;
    last_depth = 0;
    calibration_offset = raw_adc;  // Start with current position as zero
    calibrated = false;
    initialized = true;
}

void depth_poll(void) {
    if (!initialized) return;

    // Start ADC conversion
    ADC1_CR2 |= (1 << 0);  // ADON triggers conversion

    // Wait for conversion (with timeout)
    int timeout = 10000;
    while (!(ADC1_SR & (1 << 1)) && --timeout > 0);

    if (timeout > 0) {
        // Read new value
        uint16_t new_adc = ADC1_DR & 0xFFF;

        // VALIDATION 1: Bounds checking
        if (new_adc < ADC_MIN_VALID || new_adc > ADC_MAX_VALID) {
            // Out of valid range - sensor disconnected or hardware fault
            adc_fault = true;
            return;  // Skip this reading
        }

        // VALIDATION 2: Stuck sensor detection
        if (new_adc == last_raw_adc) {
            adc_stuck_count++;
            if (adc_stuck_count >= ADC_STUCK_THRESHOLD) {
                // Sensor stuck at same value - hardware fault
                adc_fault = true;
                // Continue using last valid reading (graceful degradation)
            }
        } else {
            adc_stuck_count = 0;  // Reading changed, reset counter
        }
        last_raw_adc = new_adc;

        // Timeout cleared - valid reading received
        adc_timeout_count = 0;

        raw_adc = new_adc;

        // Low-pass filter: filtered = alpha*filtered + (1-alpha)*new
        // Using fixed point: filtered_adc is scaled by 256
        filtered_adc = ((LPF_ALPHA * filtered_adc) +
                        ((256 - LPF_ALPHA) * (new_adc << 8))) >> 8;

        // Save previous depth for delta calculation
        last_depth = current_depth;

        // Convert filtered ADC to depth units (0.1mm)
        // depth = (adc - calibration) * scale
        int16_t adc_val = filtered_adc >> 8;
        int32_t delta_adc = adc_val - calibration_offset;
        current_depth = (delta_adc * DEPTH_SCALE_NUM) / DEPTH_SCALE_DEN;
    } else {
        // VALIDATION 3: Timeout detection
        adc_timeout_count++;
        if (adc_timeout_count >= ADC_TIMEOUT_MAX) {
            // Multiple consecutive timeouts - ADC hardware fault
            adc_fault = true;
        }
    }
}

void depth_reset(void) {
    // Set current position as zero
    calibration_offset = filtered_adc >> 8;
    current_depth = 0;
    last_depth = 0;
}

int16_t depth_get(void) {
    return current_depth;
}

int16_t depth_get_raw(void) {
    return raw_adc;
}

int16_t depth_get_delta(void) {
    return current_depth - last_depth;
}

bool depth_is_decreasing(void) {
    // Quill rising (ADC decreasing = depth decreasing)
    return current_depth < (last_depth - 2);  // 0.2mm threshold
}

bool depth_is_increasing(void) {
    // Quill lowering (ADC increasing = depth increasing)
    return current_depth > (last_depth + 2);  // 0.2mm threshold
}

int8_t depth_get_direction(void) {
    int16_t delta = depth_get_delta();
    if (delta > 2) return -1;   // Going down (drilling)
    if (delta < -2) return 1;   // Going up (retracting)
    return 0;                    // Stationary
}

void depth_calibrate(void) {
    calibration_offset = filtered_adc >> 8;
    current_depth = 0;
    last_depth = 0;
    calibrated = true;
}

void depth_set_offset(int16_t offset) {
    calibration_offset = offset;
    if (offset != 0) calibrated = true;
}

int16_t depth_get_offset(void) {
    return calibration_offset;
}

bool depth_is_calibrated(void) {
    return calibrated;
}

bool depth_is_initialized(void) {
    return initialized;
}

bool depth_has_fault(void) {
    return adc_fault;
}

void depth_clear_fault(void) {
    adc_fault = false;
    adc_stuck_count = 0;
    adc_timeout_count = 0;
}
