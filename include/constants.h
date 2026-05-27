/**
 * @file constants.h
 * @brief Centralized magic number constants
 *
 * Extracts magic numbers from codebase into named constants
 * Improves code readability and maintainability
 */

#ifndef CONSTANTS_H
#define CONSTANTS_H

/*===========================================================================*/
/* Timing Constants (milliseconds)                                           */
/*===========================================================================*/

#define DELAY_DEBOUNCE_MS           50      // Button debounce delay
#define DELAY_BRAKE_MS              100     // Motor brake/stop delay
#define DELAY_BOOT_STAGE_MS         500     // Boot stage delays
#define DELAY_ERROR_DISPLAY_MS      1500    // Error message display duration
#define DELAY_WARNING_DISPLAY_MS    2000    // Warning message display
#define DELAY_STATUS_DISPLAY_MS     2000    // Status info display

/*===========================================================================*/
/* Depth Sensor Constants                                                     */
/*===========================================================================*/

#define DEPTH_RESOLUTION_0_1MM      1       // 0.1mm resolution units
#define DEPTH_MM_TO_UNITS(mm)       ((mm) * 10)  // Convert mm to 0.1mm units
#define DEPTH_UNITS_TO_MM(units)    ((units) / 10.0f)

/*===========================================================================*/
/* Motor Control Constants                                                    */
/*===========================================================================*/

#define MOTOR_STOP_DELAY_MS         100     // Delay after stop command
#define MOTOR_START_DELAY_MS        100     // Delay after start command
#define MOTOR_DIRECTION_DELAY_MS    100     // Delay between direction changes

/*===========================================================================*/
/* Queue and Buffer Sizes                                                     */
/*===========================================================================*/

#define EVENT_QUEUE_SIZE            32      // Event queue depth
#define MOTOR_CMD_QUEUE_SIZE        16      // Motor command queue depth
#define UART_RX_BUFFER_SIZE         64      // UART RX ring buffer
#define MOTOR_UART_BUFFER_SIZE_VAL  32      // Motor UART buffer size

/*===========================================================================*/
/* Percentage Thresholds                                                      */
/*===========================================================================*/

#define PERCENT_MIN                 0
#define PERCENT_MAX                 100
#define LOAD_THRESHOLD_DEFAULT_PCT  60      // 60% load increase triggers tap reverse
#define STACK_MARGIN_CRITICAL_PCT   20      // <20% stack margin is critical
#define STACK_MARGIN_WARNING_PCT    30      // <30% stack margin is warning

/*===========================================================================*/
/* Retry and Timeout Values                                                   */
/*===========================================================================*/

#define RETRY_COUNT_MAX             3       // Maximum retry attempts
#define TIMEOUT_SHORT_MS            10      // Short timeout (checksum byte)
#define TIMEOUT_MEDIUM_MS           100     // Medium timeout (motor command)
#define TIMEOUT_LONG_MS             250     // Long timeout (motor response)
#define TIMEOUT_VERY_LONG_MS        500     // Very long timeout (status query)

/*===========================================================================*/
/* Array Sizes                                                                */
/*===========================================================================*/

#define LCD_LINE_LENGTH             16      // LCD 16 characters per line
#define LCD_LINE_BUFFER_SIZE        17      // +1 for null terminator
#define TASK_NAME_MAX_LENGTH        16      // Max task name length
#define COMMAND_BUFFER_SIZE         32      // Serial command buffer

/*===========================================================================*/
/* Conversion Factors                                                         */
/*===========================================================================*/

#define MS_PER_SECOND               1000
#define SECONDS_PER_MINUTE          60
#define RPM_TO_RPS(rpm)             ((rpm) / 60.0f)
#define RPS_TO_RPM(rps)             ((rps) * 60.0f)

/*===========================================================================*/
/* Bit Manipulation                                                           */
/*===========================================================================*/

#define BIT(n)                      (1U << (n))
#define SET_BIT(reg, bit)           ((reg) |= BIT(bit))
#define CLEAR_BIT(reg, bit)         ((reg) &= ~BIT(bit))
#define READ_BIT(reg, bit)          (((reg) & BIT(bit)) != 0)
#define TOGGLE_BIT(reg, bit)        ((reg) ^= BIT(bit))

#endif // CONSTANTS_H
