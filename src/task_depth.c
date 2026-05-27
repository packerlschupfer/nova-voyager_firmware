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
#include "depth.h"
#include "tapping.h"
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

    /* REVIEW FIX (MEDIUM): settings.depth.offset is packed into the EEPROM
     * block, range-checked on load and mirrored to flash — and was read by
     * nothing at all. calibrate() set this file-static and never persisted it,
     * and nothing restored it here, so every zero the operator set was lost the
     * moment they pressed OFF (which is NRST on this machine). Restore a saved
     * calibration; fall back to "here is zero" only when there is none. */
    const settings_t* ds = settings_get();
    if (ds && ds->depth.offset > 0 && ds->depth.offset <= 4095) {
        calibration_offset = ds->depth.offset;
    } else {
        calibration_offset = raw_adc;
    }

    current_depth = 0;
    last_depth = 0;
    initialized = true;

    STATE_LOCK();
    g_state.depth_offset = calibration_offset;
    STATE_UNLOCK();
}

// C7 fix: Maximum depth change per 20ms poll (10mm = 100 units in 0.1mm)
// This rejects noise spikes that would cause spurious tapping triggers
#define DEPTH_MAX_DELTA_PER_POLL    100
/* How far the quill must retract above the target before the depth action can
 * fire again, in 0.1 mm units. Wide enough that dither at the stop point can
 * never re-trigger, narrow enough not to miss a genuine short peck. */
#define DEPTH_TARGET_REARM_0_1MM    20   /* 2.0 mm */

// AUDIT FIX (MEDIUM, task_depth.c:205): recovery count for the outlier
// rejector. After this many consecutive rejections (10 × 20 ms = ~200 ms) we
// accept the raw sample — a genuinely large move (spring return, hand-yanked
// quill) reads as a persistently large delta, not noise.
#define DEPTH_OUTLIER_RECOVERY_COUNT  10
static uint8_t outlier_streak = 0;

// AUDIT FIX (MEDIUM, task_depth.c:260): local sensor-fault flag. depth.c's
// depth_poll+depth_has_fault has zero callers (the running task uses
// adc_init/adc_poll in this file), so its bounds/stuck/timeout checks are
// dead code and depth_has_fault() is permanently false. Wire a stripped
// version of the same validation into the live DMA path.
#define ADC_BOUNDS_MIN         10       // Below this = pot disconnected
#define ADC_BOUNDS_MAX         4085     // Above this = short / rail
/* AUDIT FIX (HIGH, task_depth.c:313): the fault used to clear on the FIRST
 * in-range sample that differed from the last one, so a floating, noisy input
 * self-healed the flag between polls and the depth auto-stop then fired against
 * garbage. Require a run of good samples instead. ~0.4 s at 50 Hz. */
#define ADC_FAULT_CLEAR_COUNT  20
static bool adc_hw_fault = false;
static uint16_t adc_good_count = 0;
bool task_depth_has_fault(void) { return adc_hw_fault; }

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

    /* Sensor-fault detection: pot disconnected (rail LOW) or shorted (rail
     * HIGH). Bounds are wider than the useful range so nominal calibration
     * does not trip them.
     *
     * The third case this used to check — "stuck value" — is gone. It declared
     * a fault after ~5 s of an unchanging reading, which is the normal resting
     * state of a parked quill, and on 2026-08-30 it refused to start a drill on
     * a healthy machine ("DEPTH SENSOR! Check quill pot"). A test that cannot
     * tell a dead sensor from an idle one is not a test. Open and short remain
     * detectable and are what actually indicate a broken pot. */
    if (new_adc < ADC_BOUNDS_MIN || new_adc > ADC_BOUNDS_MAX) {
        adc_hw_fault = true;
        adc_good_count = 0;
        /* REVIEW FIX (HIGH): a sample we have just declared invalid used to
         * fall through into raw_adc and the LPF anyway. src/depth.c:132 gets
         * this right ("Skip this reading"). With the wiper open (ADC 0) the
         * filter halves every 20 ms; the rate limiter rejects the jump for ten
         * polls and then DEPTH_OUTLIER_RECOVERY_COUNT accepts it wholesale, so
         * within ~250 ms current_depth is large-negative garbage — displayed,
         * and consumed by check_step_drill_rpm() (which never checks the fault
         * flag) and the tapping depth/quill triggers. Hold the last good
         * reading instead; the fault itself is reported below. */
        return;
    } else {
        /* A sane, in-bounds reading counts toward clearing the fault.
         *
         * FIELD FIX 2026-08-30: this used to require the value to have CHANGED
         * ("clear only after a sustained run of sane, MOVING samples"), which
         * carried the same false assumption as the stuck detector that was
         * removed with it — that a healthy quill keeps moving. A drill-press
         * quill is parked most of its life, so a transient out-of-bounds sample
         * could latch adc_hw_fault and then never clear, because clearing
         * required movement that was never coming. In bounds is healthy;
         * whether the operator happens to be feeding is irrelevant. */
        if (adc_good_count < ADC_FAULT_CLEAR_COUNT) {
            adc_good_count++;
        }
        if (adc_good_count >= ADC_FAULT_CLEAR_COUNT) {
            adc_hw_fault = false;
        }
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
        outlier_streak = 0;
    } else {
        // AUDIT FIX (MEDIUM, task_depth.c:205): after a fast quill movement
        // (>10 mm/20 ms, e.g. spring return) the old code rejected samples
        // forever until the quill re-entered ±10 mm of the frozen value, so
        // check_target_depth ran against a stale deep reading. Recover by
        // accepting the raw new_depth after N consecutive rejections
        // (~200 ms), on the assumption that a persistently far reading is
        // reality, not noise.
        if (++outlier_streak >= DEPTH_OUTLIER_RECOVERY_COUNT) {
            last_depth = current_depth;
            current_depth = new_depth;
            outlier_streak = 0;
        }
    }
}

/*===========================================================================*/
/* Calibration                                                                */
/*===========================================================================*/

static void calibrate(void) {
    calibration_offset = filtered_adc >> 8;
    current_depth = 0;
    last_depth = 0;

    /* Persist it, so the zero survives the OFF button — see adc_init(). The
     * operator still has to SAVE; this only marks settings dirty. */
    settings_set_depth_offset(calibration_offset);

    STATE_LOCK();
    g_state.depth_offset = calibration_offset;
    g_state.current_depth = 0;
    STATE_UNLOCK();
}

/*===========================================================================*/
/* Target Depth Check                                                         */
/*===========================================================================*/

/* REVIEW FIX (MEDIUM): the depth auto-stops used plain MOTOR_CMD, which gives
 * up after MOTOR_CMD_TIMEOUT_MS (100 ms) on a full 16-deep queue and reports
 * nothing — while the code then set motor_running = false and latched
 * depth_target_fired, so the stop was never retried and the UI said IDLE with
 * the spindle still turning. MOTOR_CMD_SEND_CRITICAL exists for exactly this
 * (it falls back to motor_emergency_stop() and raises EVT_MOTOR_FAULT) and had
 * ZERO call sites anywhere in src/ — written for safety stops and never wired
 * to one. A stop that cannot be queued must not be silently discarded. */
static void check_target_depth(void) {
    STATE_LOCK();
    int16_t target = g_state.target_depth;
    uint8_t depth_mode = g_state.depth_mode;
    bool motor_running = g_state.motor_running;
    bool guard_closed = g_state.guard_closed;
    STATE_UNLOCK();

    // Latch to prevent repeated firing
    static bool depth_target_fired = false;
    static bool depth_fault_reported = false;

    /* AUDIT FIX (HIGH, task_depth.c:326): the latch used to be re-armed by
     * `if (!motor_running)`, which is not an event about depth at all. In
     * mode 2 (stop + reverse) the branch never clears g_state.motor_running,
     * but the motor task does, transiently, between processing CMD_MOTOR_STOP
     * and CMD_MOTOR_REVERSE — tens of milliseconds of UART work during which
     * this 50 Hz task sees false and unlatches. The quill is still at target,
     * so it fires again: STOP -> 100 ms blocking delay -> REVERSE ->
     * EVT_DEPTH_TARGET, at roughly 5 Hz for as long as the operator holds
     * depth. Miss that window instead and the opposite happens: the latch
     * never clears and the next plunge gets no auto-reverse at all.
     *
     * Re-arm on the thing that actually means "a new plunge": the quill
     * retracting clear of the target, with hysteresis so dither at the target
     * cannot re-trigger. Checked before the motor_running gate so a retraction
     * with the motor off still re-arms. */
    if (depth_target_fired && target > 0) {
        int32_t rearm_at = (int32_t)target - DEPTH_TARGET_REARM_0_1MM;
        if (rearm_at < 0) rearm_at = 0;
        if ((int32_t)current_depth <= rearm_at) {
            depth_target_fired = false;
        }
    }

    if (!motor_running) {
        /* REVIEW FIX (HIGH): the latch was cleared only when the fault cleared
         * or depth mode was off — never here. A stuck ADC fault stops the
         * motor once, sets the latch, and drops to IDLE (not ERROR), so one ON
         * press restarts the spindle with the latch still set: the stop block
         * below is skipped and the configured depth auto-stop is silently dead
         * for the rest of the session — exactly what the AUDIT FIX below
         * claims to prevent. Not drilling means the next run reports afresh. */
        depth_fault_reported = false;
        return;
    }

    if (!guard_closed) {
        depth_fault_reported = false;   /* same reasoning as the gate above */
        return;
    }

    /* REVIEW FIX (HIGH): the depth_mode gate used to sit ABOVE the sensor-fault
     * block, so with the factory-default DEPTH_MODE_OFF the fault was never
     * reported — while the tapping triggers consume the very same sensor
     * unconditionally. With the quill trigger armed and depth auto-stop off, a
     * loose quill-pot connector froze adc_poll() on its last good sample, so
     * check_quill_lift_wants_reverse() saw zero delta and never fired: the tap
     * kept driving into a blind hole behind a plausible depth reading, with no
     * message anywhere. Report whenever ANY consumer is armed. */
    const tapping_settings_t* tcfg = tapping_get_settings();
    /* REVIEW FIX: `target != 0` accepted a negative target that the re-arm
     * block above (and the tapping trigger) require to be > 0 — so it armed a
     * stop whose latch could never clear. One condition, everywhere. */
    const bool depth_autostop_armed = (depth_mode != 0 && target > 0);
    /* FIELD FIX 2026-08-30: peck_depth_stop was in this list, and it DEFAULTS
     * TO TRUE (settings.c) — so this was always true and the sensor-fault path
     * was armed on every machine regardless of whether anything consumed depth.
     * It is a sub-option OF the peck trigger ("stop at target depth vs complete
     * all cycles"), not a statement that depth is in use. It only means
     * anything when peck tapping is actually enabled.
     *
     * Also require tapping to be ARMED: a trigger configured but not armed is
     * not consuming the depth reading either. */
    STATE_LOCK();
    const bool tap_armed_now = g_state.tapping_armed;
    STATE_UNLOCK();
    /* REVIEW FIX (HIGH): step drill consumes the depth reading as well, and it
     * was missing from the arming condition — so with step drill on, depth mode
     * off and tapping unarmed, a loose quill-pot froze current_depth with no
     * stop, no event and no "DEPTH SENSOR!" screen, while check_step_drill_rpm()
     * went on computing step and diameter from the frozen value and its
     * target_diameter auto-stop could never fire. */
    const bool step_drill_uses_depth = settings_get()->step_drill.enabled;
    const bool tapping_uses_depth = tap_armed_now &&
                                    (tcfg->depth_trigger_enabled ||
                                     tcfg->quill_trigger_enabled ||
                                     (tcfg->peck_trigger_enabled &&
                                      tcfg->peck_depth_stop));

    /* AUDIT FIX (HIGH, task_depth.c:313): a depth-sensor fault used to bail
     * out here silently — no event, no LCD warning, no stop — so a quill-pot
     * connector shaking loose mid-drill simply disabled the configured "stop
     * at 20 mm" while the display went on showing a frozen depth. The operator
     * armed an automatic stop; if it can no longer be delivered they have to be
     * told, and the safe direction is to stop rather than to keep cutting past
     * an unknowable depth. Reported once per fault episode. */
    if ((depth_autostop_armed || tapping_uses_depth || step_drill_uses_depth) &&
        (depth_has_fault() || task_depth_has_fault())) {
        if (!depth_fault_reported) {
            depth_fault_reported = true;
            uart_puts("[DEPTH] Sensor fault - depth triggers unavailable, stopping motor\r\n");
            MOTOR_CMD_SEND_CRITICAL(CMD_MOTOR_STOP, 0);
            STATE_LOCK();
            g_state.motor_running = false;
            g_state.state = APP_STATE_IDLE;
            g_state.error_until = HAL_GetTick() + 30000;
            g_state.error_line1 = " DEPTH SENSOR!  ";
            g_state.error_line2 = " Check quill pot";
            STATE_UNLOCK();
        }
        return;
    }
    depth_fault_reported = false;

    if (!depth_autostop_armed) {
        return;   /* sensor is healthy; nothing further to do without a target */
    }

    if (current_depth >= target && !depth_target_fired) {
        depth_target_fired = true;

        if (depth_mode == 1) {
            MOTOR_CMD_SEND_CRITICAL(CMD_MOTOR_STOP, 0);
            STATE_LOCK();
            g_state.motor_running = false;
            g_state.state = APP_STATE_IDLE;
            STATE_UNLOCK();
        } else if (depth_mode == 2) {
            MOTOR_CMD_SEND_CRITICAL(CMD_MOTOR_STOP, 0);
            delay_ms(TAP_TRANSITION_MS);
            MOTOR_CMD(CMD_MOTOR_REVERSE, 0);
        }
        SEND_EVENT(EVT_DEPTH_TARGET);
    }
}

static void check_step_drill_rpm(void) {
    const settings_t* s = settings_get();

    if (!s->step_drill.enabled) {
        return;  // Step drill mode disabled
    }

    /* REVIEW FIX (HIGH): this never consulted the sensor-fault flag, so a
     * frozen depth reading was used to compute the step number, the current
     * diameter and the RPM the spindle is commanded to — silently, and with the
     * target-diameter auto-stop unable to fire. A depth we know to be stale is
     * not a depth. check_target_depth() reports and stops; this simply must not
     * act on it. */
    if (depth_has_fault() || task_depth_has_fault()) {
        return;
    }

    STATE_LOCK();
    bool motor_running = g_state.motor_running;
    bool guard_closed = g_state.guard_closed;
    STATE_UNLOCK();

    /* The stop below is latched so the CRITICAL send cannot fire every poll;
     * re-arm whenever the machine is not drilling, which is the same "a new
     * job is starting" edge the depth latch uses. */
    static bool step_stop_fired = false;
    if (!motor_running) {
        step_stop_fired = false;
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
        /* REVIEW FIX: this path has no fired-latch — it re-issues on every
         * 50 Hz poll while the diameter stays at target. With the CRITICAL
         * send that means each poll finding the queue full calls
         * motor_emergency_stop(), which takes the motor UART mutex with
         * portMAX_DELAY; a full queue usually means task_motor is mid-
         * transaction holding it, so task_depth would block indefinitely and
         * its heartbeat would go stale. The other three converted sites are
         * latched. Latch this one too, then the CRITICAL send is safe here. */
        if (!step_stop_fired) {
            step_stop_fired = true;
            MOTOR_CMD_SEND_CRITICAL(CMD_MOTOR_STOP, 0);
        }

        /* REVIEW FIX (MEDIUM): this cleared motor_running but left
         * g_state.state at APP_STATE_DRILLING, unlike the depth auto-stop
         * above. The next poll returns early on !motor_running so nothing ever
         * repaired it: the LCD read DRILLING with the spindle stopped, ON took
         * handle_btn_start's DRILLING branch and only stopped (so restarting
         * needed two presses), and every "busy" check refused MSYNC/MSAVE and
         * menu saves for good. */
        STATE_LOCK();
        g_state.motor_running = false;
        g_state.state = APP_STATE_IDLE;
        STATE_UNLOCK();

        // Send event for user notification
        SEND_EVENT(EVT_DEPTH_TARGET);  // Reuse depth target event
        return;
    }

    // Calculate target RPM: base_rpm * (start_dia / current_dia)
    // Avoid division by zero
    if (current_dia == 0) current_dia = s->step_drill.start_diameter;
    if (current_dia == 0) return;

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
/**
 * @brief Get raw ADC reading from depth sensor
 *
 * @return Raw 12-bit ADC value (0-4095) from PC1/ADC_CH11
 *
 * Thread safety: Safe from any task (reads cached value)
 * Update rate: 50Hz (depth task polling)
 */
uint16_t depth_get_raw_adc(void) {
    return raw_adc;
}
