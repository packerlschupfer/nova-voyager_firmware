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
#define TEMP_SHUTDOWN_FLOOR     80  // Critical shutdown — non-configurable safety ceiling

// External UART function for logging
extern void uart_puts(const char* s);

// GD32F303 temperature sensor calibration
// Datasheet: V25 = 1.40-1.50V (typ 1.43V), Slope = 4.0-4.6 mV/°C (typ 4.3)
/* How long a GD32 die-temperature reading is reused before the ADC is
 * disturbed again. The die warms over minutes; the display asks 30x/s. */
#define TEMP_GD32_CACHE_MS 1000

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
    /* REVIEW FIX (MEDIUM): this has no cache and no rate limit, and
     * display_row3_temp() calls it once per display_update() — every 33 ms
     * while row 3 shows the temperature page. Each call enters a critical
     * section, rewrites ADC1 CR2/SQR3, DISCARDS task_depth's in-flight
     * conversion, runs its own channel-16 conversion and re-arms CONT+DMA. So
     * leaving the display on the temperature page tore down and restarted the
     * quill-depth DMA stream 30-60 times a second — one depth sample dropped
     * each time — and masked interrupts, the E-Stop EXTI among them, for the
     * aggregate duration. The bounds above are sized, in their own words, "for
     * a once-a-second board-temperature reading".
     *
     * The die warms over minutes. Serve a cached value and take a real reading
     * at most once a second. */
    static uint16_t cached_c = 0;
    static uint32_t cached_at = 0;
    static bool     have_cached = false;

    const uint32_t now = HAL_GetTick();
    if (have_cached && (uint32_t)(now - cached_at) < TEMP_GD32_CACHE_MS) {
        return cached_c;
    }

    if (!temp_sensor_initialized) {
        temperature_init();
    }

    // AUDIT FIX (HIGH, temperature.c:44): ADC1 is shared with the depth task
    // running in DMA + CONT mode. Old code saved only SQR3, which meant the
    // one-shot temperature read left DMA and CONT alone but the CR2 CAL from
    // temperature_init() itself had already stomped them permanently.
    // Now temperature_read_gd32 also saves+restores the full CR2 (CONT/DMA/
    // ADON), and quiesces the ADC into single-conversion mode for the read.
    // Depth task's continuous stream re-arms as soon as CR2 is restored.
    /* AUDIT FIX (MEDIUM, temperature.c:84): the critical section spans the
     * whole ADC sequence, so every poll iteration inside it runs with
     * interrupts OFF. The bounds used to be 100,000 (and I briefly added a
     * 20,000-iteration drain on top), which at 120 MHz is milliseconds of
     * masked interrupts on a machine whose E-Stop is an EXTI — for a
     * once-a-second board-temperature reading.
     *
     * A 239.5-cycle sample plus conversion at the 15 MHz ADC clock is ~19 us,
     * about 2,300 CPU cycles. The bounds below are a few times that, so a
     * healthy conversion always completes and a dead ADC costs tens of
     * microseconds rather than milliseconds. */
    taskENTER_CRITICAL();
    uint32_t saved_cr2  = ADC1->CR2;
    uint32_t saved_sqr3 = ADC1->SQR3;
    // AUDIT FIX (HIGH, temperature.c:95): EOC was never cleared before our
    // conversion was triggered, so the poll below returned the PREVIOUS
    // conversion's result — and the previous conversion belonged to the depth
    // task, on channel 11, the quill pot. task_depth.c:107 runs ADC1
    // continuously in CONT+DMA; clearing CONT here does not abort the
    // conversion already in flight, it lets it finish, and with DMA now off
    // nothing reads DR, so EOC latches holding the quill position. The
    // subsequent wait saw that stale flag immediately. Result: the "HMI board
    // temperature" on display.c:306 row 3, and motor_get_temperature()'s
    // fallback, both tracked QUILL POSITION rather than die temperature.
    //
    // Sequence below is deterministic: clear CONT/DMA (per the reference
    // manual, a CR2 write that changes bits other than ADON does not trigger a
    // conversion), drain the in-flight one, and only then select channel 16 and
    // trigger. With CONT clear and no trigger issued, nothing can set EOC
    // between the drain and our own start.
    //
    // TSVREFE is forced on rather than inherited: saved_cr2 is whatever the
    // DEPTH task left in CR2, and nothing there needs the temperature sensor,
    // so the internal channel was disconnected for our conversion. That was
    // invisible while the stale-EOC bug meant channel 16 was never actually
    // read — fixing the EOC ordering exposed it immediately, as "MCU/HMI: 0C"
    // on the machine (channel 16 reads ~0, the formula gives 357 C, and the
    // sanity check turns that into 0). temperature_init() sets TSVREFE once at
    // boot; that is not enough, because this write would clear it again.
    ADC1->CR2 = (saved_cr2 & ~(ADC_CR2_CONT | ADC_CR2_DMA)) | ADC_CR2_TSVREFE;

    // Drain the conversion the depth stream already had running. A 239.5-cycle
    // sample plus conversion is ~25 us at a legal ADC clock; the bound is
    // generous but finite so a dead ADC cannot wedge this critical section.
    uint32_t drain = 2000;
    while (!(ADC1->SR & ADC_SR_EOC) && drain > 0) { drain--; }
    (void)ADC1->DR;              // reading DR clears EOC

    ADC1->SQR3 = 16;
    ADC1->SR &= ~ADC_SR_EOC;     // belt and braces before our own trigger
    ADC1->CR2 |= ADC_CR2_ADON;   // ADON already 1 -> starts the conversion
    // AUDIT FIX (LOW, temperature.c:84): post-decrement wrapped `timeout` to
    // 0xFFFFFFFF on expiry, so the (timeout > 0) check below returned the
    // stale/garbage DR value instead of 0. Pre-decrement in the condition.
    uint32_t timeout = 4000;
    while (!(ADC1->SR & ADC_SR_EOC) && timeout > 0) { timeout--; }
    uint16_t adc_value = (timeout > 0) ? ADC1->DR : 0;
    // Restore depth task's CONT+DMA config and channel.
    ADC1->SQR3 = saved_sqr3;
    ADC1->CR2 = saved_cr2;
    // Re-arm the continuous conversion if depth was running one.
    if (saved_cr2 & (ADC_CR2_CONT | ADC_CR2_DMA)) {
        ADC1->CR2 |= ADC_CR2_ADON;   // SWSTART equivalent while CONT is set
    }
    taskEXIT_CRITICAL();

    if (timeout == 0) {
        /* Same rate-limit as the range check below: a wedged ADC must not be
         * re-entered on every display frame. */
        cached_c = 0;
        cached_at = now;
        have_cached = true;
        return 0;
    }

    // Convert to millivolts (3.3V reference, 12-bit ADC).
    // AUDIT FIX (LOW, temperature.c:101): use signed intermediate so
    // (V25_MV - voltage_mv) doesn't underflow when die temperature is below
    // 25 °C. Old code returned 0 for the entire sub-25 °C range because the
    // unsigned subtract wrapped to a huge positive.
    int32_t voltage_mv = (int32_t)((adc_value * 3300) / 4096);

    // Calculate temperature
    // GD32 has NEGATIVE temperature coefficient (voltage decreases as temp increases)
    // Formula: Temp = 25 + (V25 - Vsense) / Slope
    //               = 25 + (1430 - voltage_mv) / 4.3
    //               = 25 + ((1430 - voltage_mv) * 10) / 43
    int32_t temp_c = 25 + ((V25_MV - voltage_mv) * 10) / AVG_SLOPE_MV_X10;

    // Sanity check
    if (temp_c < -40 || temp_c > 125) {
        /* Rate-limit the retry too: a sensor reading out of range every time
         * would otherwise re-enter the ADC sequence on every display frame,
         * which is the behaviour this cache exists to stop. */
        cached_c = 0;
        cached_at = now;
        have_cached = true;
        return 0;  // Invalid reading
    }

    /* AUDIT FIX (MEDIUM, temperature.c:125): a legitimate sub-zero reading —
     * an unheated workshop in winter is not exotic — used to be cast straight
     * to uint16_t and surfaced as ~65531, which display.c then rendered as a
     * nonsense temperature. The return type cannot carry a negative and 0
     * already means "invalid", so a cold board reports 0 °C: wrong by a few
     * degrees at worst, and never absurd. Nothing acts on low temperatures;
     * the only threshold in the system is the 80 °C overheat cutoff. */
    if (temp_c < 0) {
        temp_c = 0;
    }

    cached_c   = (uint16_t)temp_c;
    cached_at  = now;
    have_cached = true;
    return cached_c;
}

/*===========================================================================*/
/* MCB Temperature Monitoring (Phase 2.2)                                    */
/*===========================================================================*/

// [MODULE_LOCAL] Phase 5.2: Only accessed from motor task via public API
// No mutex needed - all calls from single task context
static uint16_t mcb_temp_cached = 0;        // Cached MCB heatsink temperature
static bool temp_warning_active = false;     // Warning state (hysteresis)
static bool temp_shutdown_active = false;    // Critical-shutdown latch

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

    /* Critical overheat shutdown (non-configurable safety floor).
     *
     * REVIEW FIX (HIGH): this fired EVT_OVERHEAT unlatched, unlike the warning
     * below it. temp_monitor_update() runs from task_motor's idle poll every
     * 500 ms and its running poll at 1 Hz, and handle_overheat() does
     * xQueueReset(g_motor_cmd_queue) every time. An 80 C heatsink takes
     * minutes to cool: for that whole window the motor command queue was wiped
     * twice a second — discarding whatever any task had queued in the last
     * half-second, including tapping's stop/reverse sequence — APP_STATE_ERROR
     * was re-forced and the banner re-armed, so the operator could not
     * acknowledge and no motor command lived long enough to execute. Latch it
     * with the same hysteresis the warning uses. */
    if (current_temp >= TEMP_SHUTDOWN_FLOOR) {
        if (!temp_shutdown_active) {
            temp_shutdown_active = true;
            SEND_EVENT(EVT_OVERHEAT);
        }
        return;
    }
    if (temp_shutdown_active &&
        current_temp > 0 && current_temp < TEMP_SHUTDOWN_FLOOR - TEMP_HYSTERESIS) {
        temp_shutdown_active = false;   /* cooled off; re-arm the shutdown */
    }

    // Warning threshold with hysteresis
    if (current_temp == 0) {
        // Invalid temperature - ignore
    } else if (current_temp >= threshold) {
        if (!temp_warning_active) {
            temp_warning_active = true;
            SEND_EVENT(EVT_TEMP_WARNING);
        }
    } else if (current_temp < threshold - TEMP_HYSTERESIS) {
        temp_warning_active = false;
    }
}

bool temp_is_warning_active(void) {
    return temp_warning_active;
}
