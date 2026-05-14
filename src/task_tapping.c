#include "logging.h"
#include <string.h>
/**
 * @file task_tapping.c
 * @brief Tapping Trigger System - Combinable Triggers
 *
 * NEW ARCHITECTURE: Combinable triggers (can enable multiple simultaneously)
 *
 * AUTOMATIC TRIGGERS (combinable):
 * - Depth: Stop/reverse at target depth
 * - Load Increase: KR spike detection (blind holes, excessive resistance)
 * - Load Slip: CV overshoot detection (through-hole exit)
 * - Clutch Slip: Load plateau detection (torque limiter engaged)
 * - Quill: Auto-reverse based on quill direction (manual feed sensing)
 * - Peck: Timed forward/reverse cycles
 *
 * MANUAL TRIGGERS:
 * - Pedal: Manual chip break or hold reversal
 *
 * LEGACY: Still uses mode-based switch for compatibility during transition.
// Trigger-based logic IMPLEMENTED - parallel monitoring, priority resolution
 */

#include "shared.h"
#include "config.h"
#include "tapping.h"
#include "settings.h"
#include "motor.h"
#include "serial_console.h"
#include <stdio.h>

/*===========================================================================*/
/* Constants                                                                  */
/*===========================================================================*/

// Use config.h constant: TAP_DEPTH_DEADBAND_MM (2.0mm = 20 in 0.1mm units)
#define TAP_DEADBAND_0_1MM      TAP_DEPTH_DEADBAND_MM
#define TAP_POLL_MS             50      // State machine update interval
#define TAP_LOAD_SAMPLE_MS      50      // Load sampling interval
#define THROUGH_HOLE_DEBOUNCE   3       // N consecutive low-load readings
#define TAP_TRANSITION_TIMEOUT_MS  1000 // SAFETY: Max time in TRANSITION state (1s)

/*===========================================================================*/
/* Private Variables                                                          */
/*===========================================================================*/

static int16_t tap_prev_depth = 0;
static uint8_t tap_internal_state = TAP_STATE_IDLE;
static TickType_t tap_transition_start = 0;
static TickType_t tap_phase_start = 0;          // For peck timing
static uint8_t tap_peck_cycle = 0;              // Current peck cycle
static uint8_t tap_load_baseline = 0;           // Learned baseline load (KR)
static uint8_t tap_low_load_count = 0;          // Through-hole debounce counter
static uint32_t tap_fwd_time_ms = 0;            // Peck forward time
static uint32_t tap_rev_time_ms = 0;            // Peck reverse time
static uint16_t tap_brake_delay_ms = 100;       // Cached brake delay from settings

// CV overshoot detection (discovered 2026-01-25 via logic analyzer)
// Through-hole detection uses CV overshoot, not just KR threshold
static uint16_t tap_cv_baseline = 0;            // Learned baseline CV (target speed)
static TickType_t tap_baseline_start = 0;       // When baseline learning started
static bool tap_baseline_learned = false;       // True after ~4s stable baseline
static uint8_t tap_cv_overshoot_count = 0;      // Debounce counter for CV overshoot

// Clutch slip detection (torque limiter)
static uint8_t tap_clutch_prev_load = 0;        // Previous load reading
static TickType_t tap_clutch_plateau_start = 0; // When plateau detected
static bool tap_clutch_plateau_active = false;  // True when load plateaued

#define TAP_BASELINE_LEARN_MS        4000       // 4 seconds to learn baseline (matches original)
#define CLUTCH_LOAD_MIN              30         // Minimum load to consider plateau (30%)
#define CLUTCH_LOAD_DELTA_MAX        5          // Max load change during plateau (5%)
#define DEPTH_AT_TOP_MM              10         // Depth threshold for "at top" position (1.0mm)
#define PECK_INTER_CYCLE_DELAY_MS    200        // Delay between PECK cycles (motor recovery)
#define LOAD_BASELINE_EMA_ALPHA      8          // EMA filter: new_baseline = (old*7 + new)/8

/*===========================================================================*/
/* Helper Functions                                                           */
/*===========================================================================*/

/**
 * @brief Print timestamped log message (uses FreeRTOS ticks)
 */
static void peck_log(const char* msg) {
    char buf[16];
    uint32_t ticks = xTaskGetTickCount();
    uint32_t ms = ticks * portTICK_PERIOD_MS;
    snprintf(buf, sizeof(buf), "[%5lu.%02lu] ", ms / 1000, (ms % 1000) / 10);
    uart_puts(buf);
    uart_puts(msg);
}

/**
 * @brief Stop motor for tapping (fast stop, no post-sync overhead)
 */
static void tap_motor_stop(void) {
    MOTOR_CMD(CMD_MOTOR_STOP_FAST, 0);
}

/**
 * @brief Load peck timing from settings (direct millisecond values)
 */
static void calc_peck_timing(void) {
    const settings_t* s = settings_get();

    tap_fwd_time_ms = s->tapping.peck_fwd_ms;
    tap_rev_time_ms = s->tapping.peck_rev_ms;

    if (tap_fwd_time_ms < 50) tap_fwd_time_ms = 50;
    if (tap_rev_time_ms < 50) tap_rev_time_ms = 50;

    char buf[48];
    uint32_t ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    snprintf(buf, sizeof(buf), "[%5lu.%02lu] PECK FWD=%lums REV=%lums\r\n",
             ms / 1000, (ms % 1000) / 10, tap_fwd_time_ms, tap_rev_time_ms);
    uart_puts(buf);
}

/*===========================================================================*/
/* Trigger State Structure                                                   */
/*===========================================================================*/

typedef struct {
    const char* name;           // Trigger name for logging
    bool wants_reverse;         // Trigger wants to reverse (from cutting)
    bool wants_forward;         // Trigger wants to forward (from reversing)
    bool wants_complete;        // Trigger wants to complete cycle
    uint8_t priority;           // Priority level (higher = more important)
} trigger_result_t;

/*===========================================================================*/
/* Trigger Detection Functions (Pure - No Side Effects)                      */
/*===========================================================================*/

/**
 * @brief Check if pedal wants to trigger action
 */
static bool check_pedal_wants_action(bool pedal_pressed, bool in_cutting, bool in_reversing,
                                     pedal_action_t action) {
    if (action == PEDAL_ACTION_HOLD) {
        return (in_cutting && pedal_pressed);  // Trigger reverse when pressed in cutting
    } else {  // PEDAL_ACTION_CHIP_BREAK
        return (in_cutting && pedal_pressed);  // Same for chip break (duration differs)
    }
}

/**
 * @brief Check if depth trigger wants reversal
 */
static bool check_depth_wants_reverse(int16_t current_depth, int16_t target_depth, bool in_cutting) {
    return in_cutting && target_depth > 0 && current_depth >= target_depth;
}

/**
 * @brief Check if quill lift wants reversal
 */
static bool check_quill_lift_wants_reverse(int16_t current_depth, bool in_cutting) {
    if (!in_cutting) return false;
    int16_t depth_delta = current_depth - tap_prev_depth;
    const int16_t TRIGGER_UP = -(TAP_DEADBAND_0_1MM + TAP_HYSTERESIS_MM);
    return depth_delta < TRIGGER_UP;
}

/**
 * @brief Check if quill push wants forward (when reversing)
 */
static bool check_quill_push_wants_forward(int16_t current_depth, bool in_reversing) {
    if (!in_reversing) return false;
    int16_t depth_delta = current_depth - tap_prev_depth;
    const int16_t TRIGGER_DOWN = TAP_DEADBAND_0_1MM + TAP_HYSTERESIS_MM;
    return depth_delta > TRIGGER_DOWN;
}

/**
 * @brief Check if load increase wants reversal (KR spike)
 */
static bool check_load_increase_wants_reverse(uint8_t motor_load, uint8_t threshold, bool in_cutting) {
    if (!in_cutting || !tap_baseline_learned) return false;
    return motor_load > tap_load_baseline + threshold;
}

/**
 * @brief Check if load slip wants reversal (CV overshoot - through-hole)
 * PURE: No state modification - returns detection result only
 */
static bool check_load_slip_wants_reverse(uint16_t cv_percent, bool in_cutting, int16_t current_depth) {
    if (!in_cutting || !tap_baseline_learned || current_depth < 50) return false;

    uint16_t actual_cv = motor_get_actual_rpm();
    uint16_t overshoot_threshold = (tap_cv_baseline * cv_percent) / 100;

    // Pure detection - caller handles debounce state update
    return actual_cv > overshoot_threshold;
}

/**
 * @brief Check if clutch slip wants reversal (load plateau)
 * PURE: Returns detection result, caller updates state
 */
static bool check_clutch_wants_reverse(uint8_t motor_load, uint16_t plateau_ms, bool in_cutting) {
    if (!in_cutting) return false;
    if (motor_load < CLUTCH_LOAD_MIN) return false;

    // Check load change (plateau detection)
    uint8_t load_delta = (motor_load > tap_clutch_prev_load) ?
                         (motor_load - tap_clutch_prev_load) :
                         (tap_clutch_prev_load - motor_load);

    // If load changed significantly, not a plateau
    if (load_delta > CLUTCH_LOAD_DELTA_MAX) return false;

    // Check if plateau active and duration exceeded
    if (tap_clutch_plateau_active) {
        uint32_t duration = (xTaskGetTickCount() - tap_clutch_plateau_start) * portTICK_PERIOD_MS;
        return duration >= plateau_ms;
    }

    // Plateau starting (caller will update state)
    return false;
}

/*===========================================================================*/
/* Unified Action Execution Functions                                         */
/*===========================================================================*/

/**
 * @brief Execute transition to reversing state
 */
static void execute_trigger_reverse(const char* reason) {
    if (tap_internal_state != TAP_STATE_CUTTING) return;

    extern void uart_puts(const char* s);
    LOG_TAP_DEBUG("Reversing: ");
    uart_puts(reason);
    uart_puts("\r\n");

    tap_motor_stop();
    tap_internal_state = TAP_STATE_TRANSITION;
    tap_transition_start = xTaskGetTickCount();
    STATE_LOCK();
    g_state.tap_state = TAP_STATE_TRANSITION;
    STATE_UNLOCK();
}

/**
 * @brief Execute transition to forward state
 */
static void execute_trigger_forward(const char* reason) {
    if (tap_internal_state != TAP_STATE_REVERSING) return;

    extern void uart_puts(const char* s);
    LOG_TAP_DEBUG("Forward: ");
    uart_puts(reason);
    uart_puts("\r\n");

    tap_motor_stop();
    tap_internal_state = TAP_STATE_TRANSITION;
    tap_transition_start = xTaskGetTickCount();
}

/**
 * @brief Execute completion action (stop or reverse out)
 */
static void execute_completion(completion_action_t action, const char* trigger_name) {
    extern void uart_puts(const char* s);
    extern void print_num(int32_t n);

    switch (action) {
        case COMPLETION_STOP:
            LOG_TAP_DEBUG("Complete (");
            uart_puts(trigger_name);
            uart_puts("): STOP\r\n");
            tap_motor_stop();
            tap_internal_state = TAP_STATE_IDLE;
            STATE_LOCK();
            g_state.tap_state = TAP_STATE_IDLE;
            g_state.motor_running = false;
            g_state.state = APP_STATE_IDLE;
            STATE_UNLOCK();
            break;

        case COMPLETION_REVERSE_OUT:
            LOG_TAP_DEBUG("Complete (");
            uart_puts(trigger_name);
            uart_puts("): REVERSE_OUT to top\r\n");
            tap_motor_stop();
            tap_internal_state = TAP_STATE_TRANSITION;
            tap_transition_start = xTaskGetTickCount();
            // Transition will start reverse, then stop when at top
            break;

        case COMPLETION_REVERSE_TIMED:
            LOG_TAP_DEBUG("Complete (");
            uart_puts(trigger_name);
            uart_puts("): REVERSE_TIMED\r\n");
            // Start timed reverse
            MOTOR_CMD(CMD_MOTOR_TAP_REVERSE, 0);
            tap_internal_state = TAP_STATE_REVERSING;
            tap_transition_start = xTaskGetTickCount();
            STATE_LOCK();
            g_state.motor_forward = false;
            STATE_UNLOCK();
            break;
    }
}

/**
 * @brief Complete transition after brake delay
 */
static void complete_transition(bool was_forward, int16_t depth) {
    if (tap_internal_state != TAP_STATE_TRANSITION) return;
    if ((xTaskGetTickCount() - tap_transition_start) < pdMS_TO_TICKS(tap_brake_delay_ms)) return;

    if (was_forward) {
        // Was cutting, now reverse
        MOTOR_CMD(CMD_MOTOR_TAP_REVERSE, 0);
        tap_internal_state = TAP_STATE_REVERSING;
        tap_phase_start = xTaskGetTickCount();  // Reset timer for reverse duration
        STATE_LOCK();
        g_state.tap_state = TAP_STATE_REVERSING;
        g_state.motor_forward = false;
        STATE_UNLOCK();
    } else {
        // Was reversing, now forward
        MOTOR_CMD(CMD_MOTOR_TAP_FORWARD, 0);
        tap_internal_state = TAP_STATE_CUTTING;
        tap_phase_start = xTaskGetTickCount();  // Reset timer for forward duration
        STATE_LOCK();
        g_state.tap_state = TAP_STATE_CUTTING;
        g_state.motor_forward = true;
        STATE_UNLOCK();
    }
    tap_prev_depth = depth;  // Update reference
}

/*===========================================================================*/
/* Task Entry Point                                                           */
/*===========================================================================*/

void task_tapping(void *pvParameters) {
    (void)pvParameters;

    for (;;) {
        // CRITICAL SAFETY: Update task heartbeat for watchdog monitoring
        HEARTBEAT_UPDATE_TAPPING();

        // Get current mode and state
        STATE_LOCK();
        // Mode removed - using triggers;
        app_state_t app_state = g_state.state;
        // Use simulated depth if in sim mode
        int16_t current_depth = g_state.sim_mode ? g_state.sim_depth : g_state.current_depth;
        int16_t target_depth = g_state.target_depth;
        bool pedal_pressed = g_state.pedal_pressed;
        bool motor_forward = g_state.motor_forward;
        bool motor_running = g_state.motor_running;
        uint8_t motor_load = g_state.motor_load;
        bool guard_closed = g_state.guard_closed;  // SAFETY: Check guard status
        STATE_UNLOCK();

        // CRITICAL SAFETY: Abort tapping if guard opens during operation
        if (!guard_closed && (app_state == APP_STATE_TAPPING || tap_internal_state != TAP_STATE_IDLE)) {
            extern void uart_puts(const char* s);
            LOG_TAP_DEBUG("Guard opened - aborting tapping cycle\r\n");
            tap_motor_stop();
            tap_internal_state = TAP_STATE_IDLE;
            STATE_LOCK();
            g_state.state = APP_STATE_IDLE;
            g_state.tap_state = TAP_STATE_IDLE;
            STATE_UNLOCK();
            delay_ms(TAP_POLL_MS);
            continue;
        }

        // Check if any triggers are enabled
        const tapping_settings_t* tap_cfg = tapping_get_settings();
        bool any_triggers = tap_cfg->depth_trigger_enabled ||
                           tap_cfg->load_increase_enabled ||
                           tap_cfg->load_slip_enabled ||
                           tap_cfg->clutch_slip_enabled ||
                           tap_cfg->quill_trigger_enabled ||
                           tap_cfg->peck_trigger_enabled ||
                           tap_cfg->pedal_enabled;

        // Only process if triggers enabled and in tapping state
        if (!any_triggers || app_state != APP_STATE_TAPPING) {
            tap_internal_state = TAP_STATE_IDLE;
            tap_prev_depth = current_depth;
            tap_peck_cycle = 0;
            tap_load_baseline = 0;
            tap_low_load_count = 0;
            tap_cv_baseline = 0;
            tap_baseline_learned = false;
            tap_cv_overshoot_count = 0;
            delay_ms(TAP_POLL_MS);
            continue;
        }

        // Get brake delay from settings (cached for performance)
        {
            const settings_t* s = settings_get();
            tap_brake_delay_ms = s->tapping.brake_delay_ms;
            if (tap_brake_delay_ms < 50) tap_brake_delay_ms = 50;
        }

        // ===================================================================
        // COMBINABLE TRIGGER SYSTEM - TRUE SIMULTANEOUS MONITORING
        // ALL enabled triggers check their conditions each cycle
        // ALL can contribute to reversal decision (OR logic)
        // Priority used only when multiple fire simultaneously
        // ===================================================================

        // Check which triggers are enabled
        bool pedal_enabled = tap_cfg->pedal_enabled;
        bool quill_enabled = tap_cfg->quill_trigger_enabled;
        bool depth_enabled = tap_cfg->depth_trigger_enabled;
        bool load_inc_enabled = tap_cfg->load_increase_enabled;
        bool load_slip_enabled = tap_cfg->load_slip_enabled;
        bool peck_enabled = tap_cfg->peck_trigger_enabled;

        // Legacy mode compatibility removed - triggers set directly via menu/settings

        // ===================================================================
        // TRUE PARALLEL TRIGGER MONITORING
        // Check ALL enabled triggers, use priority only when multiple fire
        // ===================================================================

        // Read state once for all trigger checks
        STATE_LOCK();
        motor_running = g_state.motor_running;
        motor_forward = g_state.motor_forward;
        current_depth = g_state.sim_mode ? g_state.sim_depth : g_state.current_depth;
        target_depth = g_state.target_depth;
        motor_load = g_state.motor_load;
        pedal_pressed = g_state.pedal_pressed;
        uint16_t target_rpm_snapshot = g_state.target_rpm;
        STATE_UNLOCK();

        bool in_cutting = (tap_internal_state == TAP_STATE_CUTTING);
        bool in_reversing = (tap_internal_state == TAP_STATE_REVERSING);
        bool in_idle = (tap_internal_state == TAP_STATE_IDLE);

        // Check ALL enabled triggers (parallel detection)
        bool pedal_wants = pedal_enabled &&
                          check_pedal_wants_action(pedal_pressed, in_cutting, in_reversing,
                                                   tap_cfg->pedal_action);
        bool depth_wants = depth_enabled &&
                          check_depth_wants_reverse(current_depth, target_depth, in_cutting);
        bool quill_lift_wants = quill_enabled &&
                               check_quill_lift_wants_reverse(current_depth, in_cutting);
        bool quill_push_wants = quill_enabled &&
                               check_quill_push_wants_forward(current_depth, in_reversing);
        bool load_inc_wants = load_inc_enabled &&
                             check_load_increase_wants_reverse(motor_load,
                                                              tap_cfg->load_increase_threshold,
                                                              in_cutting);
        bool load_slip_wants = load_slip_enabled &&
                              check_load_slip_wants_reverse(tap_cfg->load_slip_cv_percent,
                                                            in_cutting, current_depth);
        bool clutch_wants = tap_cfg->clutch_slip_enabled &&
                           check_clutch_wants_reverse(motor_load, tap_cfg->clutch_plateau_ms,
                                                     in_cutting);

        // Update detection state (debounce counters, plateau tracking)
        // This separates detection (pure) from state updates (side effects)
        if (load_slip_enabled && in_cutting && tap_baseline_learned) {
            uint16_t actual_cv = motor_get_actual_rpm();
            uint16_t overshoot_threshold = (tap_cv_baseline * tap_cfg->load_slip_cv_percent) / 100;
            if (actual_cv > overshoot_threshold) {
                tap_cv_overshoot_count++;
            } else {
                tap_cv_overshoot_count = 0;
            }
        }

        if (tap_cfg->clutch_slip_enabled && in_cutting) {
            if (motor_load < CLUTCH_LOAD_MIN) {
                tap_clutch_plateau_active = false;
            } else {
                uint8_t load_delta = (motor_load > tap_clutch_prev_load) ?
                                     (motor_load - tap_clutch_prev_load) :
                                     (tap_clutch_prev_load - motor_load);
                if (load_delta > CLUTCH_LOAD_DELTA_MAX) {
                    tap_clutch_plateau_active = false;
                    tap_clutch_plateau_start = xTaskGetTickCount();
                } else if (!tap_clutch_plateau_active) {
                    tap_clutch_plateau_active = true;
                    tap_clutch_plateau_start = xTaskGetTickCount();
                }
                tap_clutch_prev_load = motor_load;
            }
        }

        // Determine active trigger (priority resolution)
        // PRIORITY ORDER (highest to lowest): Pedal > Quill > Depth > Load Inc > Load Slip > Clutch
        // Rationale: Manual override (pedal) > Operator intent (quill) > Safety limits (depth/load)
        const char* active_trigger = NULL;
        if (pedal_wants) active_trigger = "PEDAL";
        else if (quill_lift_wants || quill_push_wants) active_trigger = "QUILL";
        else if (depth_wants) active_trigger = "DEPTH";
        else if (load_inc_wants) active_trigger = "LOAD_INC";
        else if (load_slip_wants) active_trigger = "LOAD_SLIP";
        else if (clutch_wants) active_trigger = "CLUTCH";

        // ===================================================================
        // UNIFIED STATE MACHINE (Replaces mode-specific handlers)
        // ===================================================================

        MOTOR_CONTROL_LOCK();

        // STATE 1: IDLE → CUTTING (motor start)
        if (tap_internal_state == TAP_STATE_IDLE && motor_running && motor_forward) {
            tap_internal_state = TAP_STATE_CUTTING;
            tap_prev_depth = current_depth;

            // Initialize PECK timing
            if (peck_enabled) {
                calc_peck_timing();
                tap_phase_start = xTaskGetTickCount();  // Start PECK forward timer
                tap_peck_cycle = 0;  // Reset cycle counter
            }

            // Initialize baselines for load triggers
            if (load_inc_enabled || load_slip_enabled) {
                tap_load_baseline = motor_load;
                tap_cv_baseline = target_rpm_snapshot;
                tap_baseline_start = xTaskGetTickCount();
                tap_baseline_learned = false;
                tap_cv_overshoot_count = 0;
                tap_low_load_count = 0;
            }

            STATE_LOCK();
            g_state.tap_state = TAP_STATE_CUTTING;
            STATE_UNLOCK();
        }

        // STATE 2: CUTTING → Check triggers → TRANSITION
        else if (tap_internal_state == TAP_STATE_CUTTING) {
            // Update baselines for load triggers
            if (load_inc_enabled || load_slip_enabled) {
                // Learn baseline for ~4 seconds
                if (!tap_baseline_learned) {
                    uint32_t elapsed = (xTaskGetTickCount() - tap_baseline_start) * portTICK_PERIOD_MS;
                    if (elapsed >= TAP_BASELINE_LEARN_MS) {
                        tap_baseline_learned = true;
                        uint16_t actual_cv = motor_get_actual_rpm();
                        tap_cv_baseline = actual_cv > 0 ? actual_cv : target_rpm_snapshot;
                    }
                }

                // Update KR baseline (exponential moving average, alpha=1/8)
                if (motor_load < tap_load_baseline + 10) {
                    tap_load_baseline = (tap_load_baseline * (LOAD_BASELINE_EMA_ALPHA-1) + motor_load) / LOAD_BASELINE_EMA_ALPHA;
                }
            }

            // Update quill reference when drilling
            if (quill_enabled) {
                int16_t depth_delta = current_depth - tap_prev_depth;
                const int16_t RELEASE_THRESHOLD = TAP_DEADBAND_0_1MM;
                if (depth_delta > RELEASE_THRESHOLD) {
                    tap_prev_depth = current_depth;
                }
            }

            // PECK timer-based trigger (integrated with main state machine)
            if (peck_enabled && !active_trigger) {
                uint32_t cutting_duration = (xTaskGetTickCount() - tap_phase_start) * portTICK_PERIOD_MS;
                if (cutting_duration >= tap_fwd_time_ms) {
                    active_trigger = "PECK_TIMER";
                }
            }

            // Execute reversal if any trigger fired
            if (active_trigger) {
                execute_trigger_reverse(active_trigger);
            }
        }

        // STATE 3: TRANSITION → REVERSING/FORWARD (brake delay)
        else if (tap_internal_state == TAP_STATE_TRANSITION) {
            STATE_LOCK();
            bool was_forward = g_state.motor_forward;
            STATE_UNLOCK();

            complete_transition(was_forward, current_depth);
        }

        // STATE 4: REVERSING → Check completion
        else if (tap_internal_state == TAP_STATE_REVERSING) {
            // Update quill reference when reversing
            if (quill_enabled) {
                int16_t depth_delta = current_depth - tap_prev_depth;
                if (depth_delta < -TAP_DEADBAND_0_1MM) {
                    tap_prev_depth = current_depth;
                }
            }

            // PECK timer-based forward trigger (integrated)
            bool peck_wants_forward = false;
            if (peck_enabled) {
                uint32_t reversing_duration = (xTaskGetTickCount() - tap_phase_start) * portTICK_PERIOD_MS;
                if (reversing_duration >= tap_rev_time_ms) {
                    tap_peck_cycle++;
                    // Check if more cycles needed
                    if (tap_peck_cycle < tap_cfg->peck_cycles || tap_cfg->peck_cycles == 0) {
                        peck_wants_forward = true;
                    } else {
                        // PECK cycles complete - use completion action
                        execute_completion((completion_action_t)tap_cfg->peck_completion_action, "PECK");
                    }
                }
            }

            // Check if should return to forward
            if (quill_push_wants) {
                execute_trigger_forward("QUILL_PUSH");
            }
            else if (peck_wants_forward) {
                delay_ms(PECK_INTER_CYCLE_DELAY_MS);  // Inter-cycle recovery
                execute_trigger_forward("PECK_NEXT_CYCLE");
            }
            // Check completion conditions
            else if (pedal_enabled && tap_cfg->pedal_action == PEDAL_ACTION_HOLD &&
                     !pedal_pressed && current_depth <= DEPTH_AT_TOP_MM) {
                // Pedal HOLD mode: complete when pedal released and at top
                tap_motor_stop();
                tap_internal_state = TAP_STATE_IDLE;
                STATE_LOCK();
                g_state.tap_state = TAP_STATE_IDLE;
                g_state.motor_running = false;
                g_state.state = APP_STATE_IDLE;
                STATE_UNLOCK();
            }
        }

        MOTOR_CONTROL_UNLOCK();


        // SAFETY: Transition timeout - if stuck in TRANSITION state for >1s, force stop
        if (tap_internal_state == TAP_STATE_TRANSITION &&
            (xTaskGetTickCount() - tap_transition_start) >= pdMS_TO_TICKS(TAP_TRANSITION_TIMEOUT_MS)) {
            uart_puts("TAPPING: Transition timeout - forcing stop!\r\n");
            MOTOR_CONTROL_LOCK();
            tap_motor_stop();
            MOTOR_CONTROL_UNLOCK();
            tap_internal_state = TAP_STATE_IDLE;
            STATE_LOCK();
            g_state.tap_state = TAP_STATE_IDLE;
            g_state.motor_running = false;
            g_state.state = APP_STATE_IDLE;
            STATE_UNLOCK();
        }

        delay_ms(TAP_POLL_MS);
    }
}
