/**
 * @file motor.h
 * @brief Motor Controller Communication Interface
 *
 * Handles serial communication with the Nova Voyager motor controller.
 * Protocol derived from reverse engineering the original firmware.
 */

#ifndef MOTOR_H
#define MOTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"  // For motor_power_t and protocol constants

/*===========================================================================*/
/* Motor Error Codes                                                         */
/*===========================================================================*/

/**
 * @brief Motor operation error codes
 *
 * Used by motor communication functions to provide detailed error context.
 * All functions that return motor_error_t use MOTOR_OK (0) for success.
 */
typedef enum {
    MOTOR_OK = 0,                    ///< Operation successful
    MOTOR_ERR_UART_TX_TIMEOUT = 1,   ///< UART transmit timeout (per-byte or TC)
    MOTOR_ERR_UART_RX_TIMEOUT = 2,   ///< UART receive timeout (no response)
    MOTOR_ERR_INVALID_RESPONSE = 3,  ///< Response format invalid (bad STX, checksum)
    MOTOR_ERR_OUT_OF_RANGE = 4,      ///< Parameter out of valid range
    MOTOR_ERR_BUSY = 5,              ///< Motor busy, command rejected
    MOTOR_ERR_FAULT = 6,             ///< Motor controller fault state
    MOTOR_ERR_MAX_RETRIES = 7,       ///< Maximum retry attempts exceeded
    MOTOR_ERR_HARDWARE = 8,          ///< Hardware enable/disable failed
    MOTOR_ERR_INVALID_STATE = 9,     ///< Operation invalid in current motor state
} motor_error_t;

/*===========================================================================*/
/* Motor State                                                                */
/*===========================================================================*/

typedef enum {
    MOTOR_STOPPED = 0,
    MOTOR_FORWARD = 1,
    MOTOR_REVERSE = 2,
    MOTOR_BRAKING = 3
} motor_state_t;

typedef struct {
    motor_state_t state;
    uint16_t speed_rpm;         // Current speed in RPM (deprecated - use actual_rpm)
    uint16_t target_speed;      // Target speed in RPM
    uint16_t actual_rpm;        // Actual motor speed from CV feedback (added 2026-01-22)
    bool fault;                 // Motor controller fault
    bool overload;              // Overload detected
    bool jam_detected;          // Drill bit jam
    uint32_t last_update_ms;    // Last status update time
    uint16_t vibration;         // Vibration level (0-1000)
    uint16_t load_percent;      // Motor load percentage (0-100)
    uint16_t temperature;       // Motor temperature if available
    uint16_t raw_flags;         // Raw MCB status flags for error parsing
    bool rps_error;             // Rotor position sensor error
    bool pfc_fault;             // Power factor correction fault
    bool voltage_error;         // Voltage out of range
    bool overheat;              // Temperature warning
    uint8_t retry_count;        // Number of retries for last command
} motor_status_t;

/*===========================================================================*/
/* Error Handling                                                            */
/*===========================================================================*/

/**
 * @brief Get last motor operation error
 * @return Last error code from motor operations
 *
 * Functions that return bool (for backward compatibility) set this on failure.
 * Check this immediately after a function returns false for error details.
 *
 * Thread safety: Task-local (motor task only)
 */
motor_error_t motor_get_last_error(void);

/**
 * @brief Get human-readable error string
 * @param error Error code
 * @return Error description string (static, do not free)
 */
const char* motor_error_string(motor_error_t error);

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

/**
 * @brief Initialize motor controller communication
 * @return true if initialization successful
 */
bool motor_init(void);

/**
 * @brief Stop motor with brake
 */
void motor_stop(void);

/**
 * @brief Start motor in forward direction (CW)
 */
void motor_forward(void);

/**
 * @brief Start motor in reverse direction (CCW)
 */
void motor_reverse(void);

/**
 * @brief Enable motor (must be called after setting direction)
 */
void motor_start(void);

/**
 * @brief Set motor speed
 * @param rpm Speed in RPM (50-5500)
 */
void motor_set_speed(uint16_t rpm);

/**
 * @brief Get current motor speed
 * @return Current speed in RPM
 */
uint16_t motor_get_speed(void);

/**
 * @brief Get motor status
 * @return Pointer to motor status structure
 */
const motor_status_t* motor_get_status(void);

/**
 * @brief Update motor status (call periodically)
 * Polls motor controller for current state
 */
void motor_update(void);

/**
 * @brief Check if motor is running
 * @return true if motor is running (forward or reverse)
 */
bool motor_is_running(void);

/**
 * @brief Emergency stop - immediate brake
 */
void motor_emergency_stop(void);

/*===========================================================================*/
/* Spindle Hold (powered position lock)                                       */
/*===========================================================================*/

/**
 * @brief Start spindle hold - actively holds spindle position with low power
 * Use for tool changes or precision positioning (CL=10%)
 */
void motor_spindle_hold(void);

/**
 * @brief Start safety spindle hold - higher current for E-Stop/Guard conditions
 * Automatically engaged when E-Stop pressed or Guard opened (CL=12%)
 */
void motor_spindle_hold_safety(void);

/**
 * @brief Release spindle hold and reset to normal operation
 */
void motor_spindle_release(void);

/**
 * @brief Check if spindle hold is currently active
 * @return true if spindle is being held
 */
bool motor_is_spindle_hold_active(void);

/**
 * @brief Enable motor hardware (set MOTOR_ENABLE pin HIGH)
 * SAFETY: Call this ONLY when safe to run motor (E-Stop clear, guard closed)
 */
void motor_hardware_enable(void);

/**
 * @brief Disable motor hardware (set MOTOR_ENABLE pin LOW)
 * SAFETY: Immediately cuts power to motor controller
 */
void motor_hardware_disable(void);

/**
 * @brief Check if motor hardware is enabled
 * @return true if MOTOR_ENABLE pin is HIGH
 */
bool motor_hardware_is_enabled(void);

/**
 * @brief Get current vibration level
 * @return Vibration level (0-1000, higher = more vibration)
 */
uint16_t motor_get_vibration(void);

/**
 * @brief Get current load percentage
 * @return Load percentage (0-100%)
 */
uint16_t motor_get_load(void);

/**
 * @brief Check if vibration is excessive
 * @param threshold Threshold value (0-1000)
 * @return true if vibration exceeds threshold
 */
bool motor_vibration_exceeds(uint16_t threshold);

/**
 * @brief Set motor PID parameters
 * @param speed_kp Speed proportional gain (x100)
 * @param speed_ki Speed integral gain (x100)
 * @param voltage_kp Voltage Kp (x100)
 * @param voltage_ki Voltage Ki (x100)
 */
void motor_set_pid(int16_t speed_kp, int16_t speed_ki, int16_t voltage_kp, int16_t voltage_ki);

/**
 * @brief Set motor current limit
 * @param limit_ma Current limit in milliamps
 */
void motor_set_current_limit(uint16_t limit_ma);

/**
 * @brief Set motor IR compensation parameters
 * @param ir_gain IR gain (x100)
 * @param ir_offset IR offset
 */
void motor_set_ir_comp(int16_t ir_gain, int16_t ir_offset);

/**
 * @brief Enable or disable motor braking
 * @param enabled true to enable braking
 */
void motor_set_braking(bool enabled);

/**
 * @brief Set pulse max (PWM duty cycle limit)
 * @param value Pulse max value (factory: 185)
 */
void motor_set_pulse_max(uint16_t value);

/**
 * @brief Set advance max
 * @param value Advance max value (factory: 85)
 */
void motor_set_advance_max(uint16_t value);

/**
 * @brief Restore all MCB parameters to factory defaults
 * Bypasses HMI menu limits - sends directly to MCB
 */
void motor_restore_mcb_defaults(void);

/**
 * @brief Factory reset MCB EEPROM (discovered 2026-01-25)
 *
 * Performs full factory reset of motor controller EEPROM:
 * - RS=1 × 6 (prepare for reset)
 * - EE command (EEPROM Execute)
 * - RS=1 × 7 (wait/confirm)
 * - Wait ~0.7s for MCB to complete reset
 *
 * WARNING: This erases all motor tuning parameters!
 * MCB will not respond during reset (~1.5s total).
 *
 * @return true if reset completed successfully
 */
bool motor_factory_reset(void);

/**
 * @brief Save MCB parameters to MCB EEPROM
 */
void motor_save_mcb_params(void);

/**
 * @brief Set speed ramp rate
 * @param ramp_rate RPM per second (50-2000)
 */
void motor_set_speed_ramp(uint16_t ramp_rate);

/**
 * @brief Set torque ramp rate
 * @param ramp_rate Torque ramp value (50-2000)
 */
void motor_set_torque_ramp(uint16_t ramp_rate);

/**
 * @brief Set motor profile (SOFT/NORMAL/HARD)
 * @param profile motor_profile_t value (0=SOFT, 1=NORMAL, 2=HARD)
 *
 * Profiles control torque behavior during acceleration:
 * - SOFT: Gentle acceleration, low torque (good for delicate materials)
 * - NORMAL: Balanced acceleration (general purpose)
 * - HARD: Aggressive acceleration, high torque (heavy drilling)
 *
 * Call this before motor_start() to set desired profile.
 */
void motor_set_profile(uint8_t profile);

/**
 * @brief Set power output level (CL command) - DEPRECATED
 * @param level 0=Low(20%), 1=Med(50%), 2=High(70%)
 * @deprecated Use motor_set_power_level() instead for proper SE commit
 */
void motor_set_power_output(uint8_t level);

/**
 * @brief Set motor power level with SE command commit (discovered 2026-01-25)
 * @param level motor_power_t value (MOTOR_POWER_LOW/MED/HIGH/MAX)
 * @return true if power level set and committed successfully
 *
 * Controls maximum power/current the motor can draw:
 * - MOTOR_POWER_LOW (20%): Light materials, may stall at low RPM!
 * - MOTOR_POWER_MED (50%): General drilling
 * - MOTOR_POWER_HIGH (70%): Heavy-duty drilling (factory default)
 * - MOTOR_POWER_MAX (100%): Full torque
 *
 * Uses proper SE (Set Enable) command to commit parameter change.
 * Sequence: CL=<value> → SE=CL → CL? (verify)
 */
bool motor_set_power_level(motor_power_t level);

/**
 * @brief Set thermal threshold for current reduction (TH command)
 * @param threshold_c Temperature in °C (40-100)
 *
 * MCB will reduce current when heatsink exceeds this temperature.
 * Default: 60°C (current reduction), ~100°C (shutdown)
 */
void motor_set_thermal_threshold(uint8_t threshold_c);

/**
 * @brief Set vibration sensor sensitivity (VG + VS commands)
 * @param level 0=OFF, 1=LOW, 2=MED, 3=HIGH
 *
 * Controls vibration detection sensitivity:
 * - OFF: Vibration sensor disabled (VS=0)
 * - LOW: Low sensitivity VG=85
 * - MED: Medium sensitivity VG=170
 * - HIGH: High sensitivity VG=261 (default)
 *
 * MCB will shut down motor if vibration threshold exceeded.
 */
void motor_set_vibration_sensitivity(uint8_t level);

/**
 * @brief Sync all motor settings from settings module to MCB
 * Sends PID, ramps, current limit, IR comp, etc.
 */
void motor_sync_settings(void);

/**
 * @brief Sync motor settings and save to MCB EEPROM
 * Call this after changing settings to persist to motor controller
 */
void motor_sync_and_save(void);

/**
 * @brief MCB parameter values structure
 */
typedef struct {
    int32_t pulse_max;
    int32_t adv_max;
    int32_t ir_gain;
    int32_t ir_offset;
    int32_t cur_lim;
    int32_t spd_rmp;
    int32_t trq_rmp;
    int32_t voltage_kp;
    int32_t voltage_ki;
    bool valid;  // True if successfully read
} mcb_params_t;

/**
 * @brief Read a single parameter from MCB
 * @param cmd Command code (e.g., CMD_GET_PULSE_MAX)
 * @return Parameter value, or -1 on error
 */
int32_t motor_read_param(uint16_t cmd);

/**
 * @brief Read all MCB parameters
 * @param params Pointer to structure to fill
 * @return true if successful
 */
bool motor_read_mcb_params(mcb_params_t* params);

/*===========================================================================*/
/* New Protocol Functions (discovered 2026-01-22)                            */
/*===========================================================================*/

/**
 * @brief Get actual motor RPM from CV feedback
 * @return Actual RPM, or 0 if not available
 */
uint16_t motor_get_actual_rpm(void);

/**
 * @brief Set actual motor RPM (called by CV response parser)
 * @param rpm Actual RPM from CV feedback
 */
void motor_set_actual_rpm(uint16_t rpm);

/**
 * @brief Send KR (Keep Running) heartbeat command
 * @param param KR parameter (0=stopped, 9-30=running, 100=startup)
 */
void motor_send_keep_running(uint8_t param);

/**
 * @brief Send S2 (Speed 2) command
 * @param rpm Speed value (typically 900)
 */
void motor_send_speed_2(uint16_t rpm);

/**
 * @brief CV burst confidence check (discovered 2026-01-25)
 *
 * Before stopping at depth, original firmware queries CV 3× rapidly (~50ms apart).
 * This appears to be a "confidence check" before committing to a direction change.
 *
 * @return Average CV value from burst queries
 */
uint16_t motor_cv_confidence_check(void);

/**
 * @brief Get MCB firmware version string
 * @return Version string (e.g., "B1.7") or "unknown" if not queried
 */
const char* motor_get_version(void);

/**
 * @brief Get MCB heatsink temperature
 * @return Temperature in degrees Celsius (0 if not available)
 */
uint16_t motor_get_temperature(void);

/*===========================================================================*/
/* Low-level Protocol Functions (internal use)                                */
/*===========================================================================*/

/**
 * @brief Send command to motor controller with retry logic
 * @param cmd Command code (e.g., CMD_STOP)
 * @param param Parameter value (0 if none)
 * @return true if command sent successfully after retries, false on max retries
 */
bool motor_send_command_with_retry(uint16_t cmd, int16_t param);

/**
 * @brief Send command to motor controller (single attempt, no retry)
 * @param cmd Command code (e.g., CMD_STOP)
 * @param param Parameter value (0 if none)
 * @return true if command sent successfully
 */
bool motor_send_command(uint16_t cmd, int16_t param);

/**
 * @brief Read response from motor controller
 * @param timeout_ms Timeout in milliseconds
 * @return Response value or -1 on timeout
 */
int32_t motor_read_response(uint32_t timeout_ms);

#endif /* MOTOR_H */
