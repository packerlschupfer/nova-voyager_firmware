/**
 * @file task_depth.c
 * @brief Depth Task - ADC-based Depth Sensor
 *
 * Reads quill position from potentiometer on PC1 (ADC1 Channel 11).
 * Uses DMA1_Channel1 for efficient continuous sampling.
 * Applies low-pass filtering and calibration.
 */

#include "shared.h"
#include "settings.h"
#include "stm32f1xx_hal.h"

// Enable DMA for ADC (reduces CPU usage ~10-15%)
#define USE_ADC_DMA     1

/*===========================================================================*/
/* ADC Register Access                                                        */
/*===========================================================================*/

// Use HAL-defined ADC1_BASE, add register offsets
#define ADC1_SR_REG     (*(volatile uint32_t*)(ADC1_BASE + 0x00))
#define ADC1_CR1_REG    (*(volatile uint32_t*)(ADC1_BASE + 0x04))
#define ADC1_CR2_REG    (*(volatile uint32_t*)(ADC1_BASE + 0x08))
#define ADC1_SMPR1_REG  (*(volatile uint32_t*)(ADC1_BASE + 0x0C))
#define ADC1_SQR3_REG   (*(volatile uint32_t*)(ADC1_BASE + 0x34))
#define ADC1_DR_REG     (*(volatile uint32_t*)(ADC1_BASE + 0x4C))

/*===========================================================================*/
/* DMA Register Access (DMA1_Channel1 for ADC1)                               */
/*===========================================================================*/

#if USE_ADC_DMA
#define DMA1_CH1_BASE   (DMA1_BASE + 0x08)  // Channel 1 offset
#define DMA1_CH1_CCR    (*(volatile uint32_t*)(DMA1_CH1_BASE + 0x00))
#define DMA1_CH1_CNDTR  (*(volatile uint32_t*)(DMA1_CH1_BASE + 0x04))
#define DMA1_CH1_CPAR   (*(volatile uint32_t*)(DMA1_CH1_BASE + 0x08))
#define DMA1_CH1_CMAR   (*(volatile uint32_t*)(DMA1_CH1_BASE + 0x0C))
#define DMA1_IFCR       (*(volatile uint32_t*)(DMA1_BASE + 0x04))

// DMA buffer for continuous ADC sampling
#define ADC_DMA_BUFFER_SIZE  16
static volatile uint16_t adc_dma_buffer[ADC_DMA_BUFFER_SIZE];
#endif

// ADC to depth conversion factor
// Calibration: 150mm real = 152.5mm displayed with 58/100
// Fine-tuned: 58/1.0167 ≈ 57/100
#define DEPTH_SCALE_NUM     57
#define DEPTH_SCALE_DEN     100

// Low-pass filter coefficient (0-255, higher = more smoothing)
// 0=disabled, 128=50% smoothing (~60ms settle), 192=75% (~100ms settle)
#define LPF_ALPHA           128

/*===========================================================================*/
/* Private Variables                                                          */
/*===========================================================================*/

static volatile uint16_t raw_adc = 0;
static int32_t filtered_adc = 0;
static int16_t current_depth = 0;
static int16_t last_depth = 0;
static int16_t calibration_offset = 0;
static bool initialized = false;

// Busy-wait delay for init (scheduler not running yet)
static void delay_ms_init(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms * 7200; i++);
}

/*===========================================================================*/
/* ADC Functions                                                              */
/*===========================================================================*/

static void adc_init(void) {
    // Enable ADC1 clock
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    // Enable GPIOC clock
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;

    // Configure PC1 as analog input (CNF=00, MODE=00)
    GPIOC->CRL &= ~(0xF << 4);

    delay_ms_init(1);

#if USE_ADC_DMA
    // Enable DMA1 clock
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;

    // Configure DMA1_Channel1 for ADC1
    DMA1_CH1_CCR = 0;  // Disable channel first
    DMA1_CH1_CPAR = (uint32_t)&ADC1->DR;  // Peripheral address (ADC data register)
    DMA1_CH1_CMAR = (uint32_t)adc_dma_buffer;  // Memory address
    DMA1_CH1_CNDTR = ADC_DMA_BUFFER_SIZE;  // Number of transfers
    // CCR: CIRC=1 (circular), MINC=1 (memory increment), PSIZE=01 (16-bit),
    //      MSIZE=01 (16-bit), PL=01 (medium priority)
    DMA1_CH1_CCR = (1 << 5) |   // CIRC - circular mode
                   (1 << 7) |   // MINC - memory increment
                   (1 << 8) |   // PSIZE[0] - 16-bit peripheral
                   (1 << 10);   // MSIZE[0] - 16-bit memory
    DMA1_CH1_CCR |= (1 << 0);  // EN - enable DMA channel

    // Configure ADC for continuous conversion with DMA
    ADC1_CR2_REG = (1 << 0) |   // ADON - power on
                   (1 << 1) |   // CONT - continuous conversion
                   (1 << 8);    // DMA - enable DMA
    delay_ms_init(10);
#else
    // Power on ADC (single conversion mode)
    ADC1_CR2_REG = (1 << 0);  // ADON
    delay_ms_init(10);
#endif

    // Configure sample time for channel 11 (71.5 cycles)
    ADC1_SMPR1_REG = (ADC1_SMPR1_REG & ~(7 << 3)) | (6 << 3);

    // Select channel 11
    ADC1_SQR3_REG = 11;

    // Calibration
    ADC1_CR2_REG |= (1 << 3);  // RSTCAL
    while (ADC1_CR2_REG & (1 << 3));
    ADC1_CR2_REG |= (1 << 2);  // CAL
    while (ADC1_CR2_REG & (1 << 2));

#if USE_ADC_DMA
    // Re-enable continuous conversion and DMA after calibration
    ADC1_CR2_REG = (1 << 0) |   // ADON
                   (1 << 1) |   // CONT
                   (1 << 8);    // DMA
    // Start first conversion (continuous mode will keep running)
    ADC1_CR2_REG |= (1 << 22);  // SWSTART
    delay_ms_init(5);  // Let DMA buffer fill

    // Read initial value from DMA buffer (average)
    uint32_t sum = 0;
    for (int i = 0; i < ADC_DMA_BUFFER_SIZE; i++) {
        sum += adc_dma_buffer[i];
    }
    raw_adc = sum / ADC_DMA_BUFFER_SIZE;
#else
    // Take initial reading (single conversion)
    ADC1_CR2_REG |= (1 << 0);
    while (!(ADC1_SR_REG & (1 << 1)));
    raw_adc = ADC1_DR_REG & 0xFFF;
#endif

    filtered_adc = raw_adc << 8;
    calibration_offset = raw_adc;
    current_depth = 0;
    last_depth = 0;
    initialized = true;
}

// C7 fix: Maximum depth change per 20ms poll (10mm = 100 units in 0.1mm)
// This rejects noise spikes that would cause spurious tapping triggers
#define DEPTH_MAX_DELTA_PER_POLL    100

static void adc_poll(void) {
    if (!initialized) return;

#if USE_ADC_DMA
    // Read averaged value from DMA circular buffer (no CPU wait!)
    uint32_t sum = 0;
    for (int i = 0; i < ADC_DMA_BUFFER_SIZE; i++) {
        sum += adc_dma_buffer[i];
    }
    uint16_t new_adc = sum / ADC_DMA_BUFFER_SIZE;
#else
    // Start conversion (polling mode)
    ADC1_CR2_REG |= (1 << 0);

    // Wait with timeout
    int timeout = 10000;
    while (!(ADC1_SR_REG & (1 << 1)) && --timeout > 0);

    if (timeout <= 0) return;  // Timeout, keep previous value

    uint16_t new_adc = ADC1_DR_REG & 0xFFF;
#endif

    // C7 fix: Bounds check - ADC should be 0-4095
    if (new_adc > 4095) {
        return;  // Invalid reading, keep previous value
    }

    raw_adc = new_adc;

    // Low-pass filter
    filtered_adc = ((LPF_ALPHA * filtered_adc) +
                    ((256 - LPF_ALPHA) * (new_adc << 8))) >> 8;

    // Convert to depth units (0.1mm)
    int16_t adc_val = filtered_adc >> 8;
    int32_t delta_adc = adc_val - calibration_offset;
    int16_t new_depth = (delta_adc * DEPTH_SCALE_NUM) / DEPTH_SCALE_DEN;

    // C7 fix: Rate-of-change validation - reject sudden jumps
    int16_t depth_delta = new_depth - current_depth;
    if (depth_delta < 0) depth_delta = -depth_delta;  // abs

    if (depth_delta <= DEPTH_MAX_DELTA_PER_POLL) {
        last_depth = current_depth;
        current_depth = new_depth;
    }
    // else: reject outlier, keep previous depth value
}

/*===========================================================================*/
/* Calibration                                                                */
/*===========================================================================*/

static void calibrate(void) {
    calibration_offset = filtered_adc >> 8;
    current_depth = 0;
    last_depth = 0;

    STATE_LOCK();
    g_state.depth_offset = calibration_offset;
    g_state.current_depth = 0;
    STATE_UNLOCK();
}

static void set_offset(int16_t offset) {
    calibration_offset = offset;

    // Recalculate current depth with new offset
    int16_t adc_val = filtered_adc >> 8;
    int32_t delta_adc = adc_val - calibration_offset;
    current_depth = (delta_adc * DEPTH_SCALE_NUM) / DEPTH_SCALE_DEN;
}

/*===========================================================================*/
/* Target Depth Check                                                         */
/*===========================================================================*/

static void check_target_depth(void) {
    STATE_LOCK();
    int16_t target = g_state.target_depth;
    uint8_t depth_mode = g_state.depth_mode;
    bool motor_running = g_state.motor_running;
    bool guard_closed = g_state.guard_closed;
    STATE_UNLOCK();

    if (depth_mode == 0 || target == 0 || !motor_running) {
        return;  // Ignore mode or no target
    }

    // SAFETY: don't queue motor commands if guard is open. handle_btn_guard
    // is already stopping us; this prevents a stale REVERSE/STOP racing
    // behind the guard event.
    if (!guard_closed) {
        return;
    }

    // Check if we've reached target depth
    if (current_depth >= target) {
        SEND_EVENT(EVT_DEPTH_TARGET);

        if (depth_mode == 1) {
            // Stop mode
            MOTOR_CMD(CMD_MOTOR_STOP, 0);
        } else if (depth_mode == 2) {
            // Stop and revert mode
            MOTOR_CMD(CMD_MOTOR_STOP, 0);
            delay_ms(TAP_TRANSITION_MS);  // Brief pause before direction change
            MOTOR_CMD(CMD_MOTOR_REVERSE, 0);
        }
    }
}

static void check_step_drill_rpm(void) {
    const settings_t* s = settings_get();

    if (!s->step_drill.enabled) {
        return;  // Step drill mode disabled
    }

    STATE_LOCK();
    bool motor_running = g_state.motor_running;
    bool guard_closed = g_state.guard_closed;
    STATE_UNLOCK();

    if (!motor_running) {
        return;  // Only adjust RPM while drilling
    }

    // SAFETY: stop adjusting RPM the moment guard opens. handle_btn_guard
    // will stop the motor; we must not queue a SET_SPEED behind that stop.
    if (!guard_closed) {
        return;
    }

    // Calculate current diameter based on depth (relative to ZERO button calibration)
    // current_depth is in 0.1mm units, step_depth_x2 is in 0.5mm units
    int16_t depth_mm_x10 = current_depth;  // e.g., 55 = 5.5mm
    int16_t step_depth_mm_x10 = s->step_drill.step_depth_x2 * 5;  // Convert 0.5mm units to 0.1mm

    if (step_depth_mm_x10 <= 0) {
        return;  // Invalid step depth config
    }

    // Use absolute depth for calculation (handle negative depths above zero point)
    if (depth_mm_x10 < 0) {
        depth_mm_x10 = 0;  // Treat above-zero as starting diameter
    }

    // Calculate current step number (0-based)
    int16_t current_step = depth_mm_x10 / step_depth_mm_x10;

    // Calculate current diameter
    uint16_t current_dia = s->step_drill.start_diameter + (current_step * s->step_drill.diameter_increment);

    // Clamp diameter to reasonable range
    if (current_dia < s->step_drill.start_diameter) {
        current_dia = s->step_drill.start_diameter;
    }
    if (current_dia > 50) {
        current_dia = 50;  // Max reasonable step drill diameter
    }

    // Check if target diameter reached (auto-stop)
    if (s->step_drill.target_diameter > 0 && current_dia >= s->step_drill.target_diameter) {
        // Stop motor - target diameter reached
        MOTOR_CMD(CMD_MOTOR_STOP, 0);

        STATE_LOCK();
        g_state.motor_running = false;
        STATE_UNLOCK();

        // Send event for user notification
        SEND_EVENT(EVT_DEPTH_TARGET);  // Reuse depth target event
        return;
    }

    // Calculate target RPM: base_rpm * (start_dia / current_dia)
    // Avoid division by zero
    if (current_dia == 0) current_dia = s->step_drill.start_diameter;

    uint16_t target_rpm = (s->step_drill.base_rpm * s->step_drill.start_diameter) / current_dia;

    // Clamp to valid RPM range
    if (target_rpm < SPEED_MIN_RPM) target_rpm = SPEED_MIN_RPM;
    if (target_rpm > SPEED_MAX_RPM) target_rpm = SPEED_MAX_RPM;

    // Update target RPM (motor task will ramp to it)
    STATE_LOCK();
    uint16_t current_target = g_state.target_rpm;
    STATE_UNLOCK();

    // Only update if changed significantly (avoid constant tiny adjustments)
    if (current_target > target_rpm + 50 || current_target < target_rpm - 50) {
        STATE_LOCK();
        g_state.target_rpm = target_rpm;
        STATE_UNLOCK();

        // Send speed change command to motor
        MOTOR_CMD(CMD_MOTOR_SET_SPEED, target_rpm);
    }
}

/*===========================================================================*/
/* Task Entry Point                                                           */
/*===========================================================================*/

void task_depth(void *pvParameters) {
    (void)pvParameters;

    TickType_t last_update = 0;
    const TickType_t update_interval = pdMS_TO_TICKS(DEPTH_UPDATE_INTERVAL_MS);

    for (;;) {
        // CRITICAL SAFETY: Update task heartbeat for watchdog monitoring
        HEARTBEAT_UPDATE_DEPTH();

        // Poll ADC
        adc_poll();

        // Update shared state
        STATE_LOCK();
        g_state.current_depth = current_depth;
        STATE_UNLOCK();

        // Check for ZERO button event (calibration request)
        // This is handled via event queue in main task

        // Check target depth
        check_target_depth();

        // Check step drill RPM adjustment
        check_step_drill_rpm();

        vTaskDelay(update_interval);
    }
}

/*===========================================================================*/
/* Initialization                                                             */
/*===========================================================================*/

/**
 * @brief Initialize depth sensor task and ADC hardware
 *
 * Configures ADC1 Channel 11 (PC1) for quill depth sensing.
 * Optionally configures DMA if enabled.
 *
 * Thread safety: Call once from main() during system init
 */
void depth_task_init(void) {
    adc_init();
}

/*===========================================================================*/
/* Public API (called from other tasks)                                       */
/*===========================================================================*/

/**
 * @brief Calibrate depth sensor to current position as zero
 *
 * Sets current quill position as zero reference point.
 * Triggered by ZERO button press (EVT_BTN_ZERO).
 *
 * Thread safety: Safe from any task (internal locking)
 */
void depth_calibrate_now(void) {
    calibrate();
}

/**
 * @brief Set depth calibration offset manually
 *
 * @param offset Offset in 0.1mm units to add to raw readings
 *
 * Thread safety: Safe from any task
 */
void depth_set_calibration(int16_t offset) {
    set_offset(offset);
}

/**
 * @brief Get raw ADC reading from depth sensor
 *
 * @return Raw 12-bit ADC value (0-4095) from PC1/ADC_CH11
 *
 * Thread safety: Safe from any task (reads cached value)
 * Update rate: 50Hz (depth task polling)
 */
int16_t depth_get_raw_adc(void) {
    return raw_adc;
}
