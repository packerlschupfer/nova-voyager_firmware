/**
 * @file diagnostics.h
 * @brief System Diagnostics and Telemetry
 *
 * Phase 7: Enhanced system health monitoring
 *
 * Tracks:
 * - UART timeout errors (TX, RX)
 * - Queue statistics (overflows, peak depth)
 * - Protocol statistics (commands sent, retries, failures)
 * - System uptime and performance
 */

#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Error Counters                                                             */
/*===========================================================================*/

typedef struct {
    // UART errors (Phase 1.1 monitoring)
    uint32_t uart_tx_timeouts;          // TX timeout count
    uint32_t uart_rx_timeouts;          // RX timeout count
    uint32_t uart_bytes_sent;           // Total bytes transmitted
    uint32_t uart_bytes_received;       // Total bytes received

    // Queue errors (Phase 1.2 monitoring)
    uint32_t event_queue_overflows;     // Event queue full count
    uint32_t motor_queue_overflows;     // Motor queue full count
    uint16_t event_queue_peak;          // Peak queue depth (high-water mark)
    uint16_t motor_queue_peak;          // Peak motor queue depth

    // Protocol errors (Phase 6 monitoring)
    uint32_t protocol_checksum_errors;  // Bad checksum count
    uint32_t protocol_frame_errors;     // Invalid frame count
    uint32_t protocol_timeout_errors;   // Protocol response timeouts
    uint32_t protocol_commands_sent;    // Total commands sent
    uint32_t protocol_queries_sent;     // Total queries sent
    uint32_t protocol_retries;          // Total retry attempts

    // Motor communication (MCB health)
    uint32_t mcb_comm_failures;         // Consecutive failure counter
    uint32_t mcb_comm_success;          // Successful communications
    uint32_t mcb_total_queries;         // Total GF queries

    // System uptime
    uint32_t boot_timestamp_ms;         // When system booted (HAL_GetTick)
    uint32_t watchdog_refresh_count;    // Watchdog kick count
} diagnostics_t;

/*===========================================================================*/
/* Public API                                                                 */
/*===========================================================================*/

/**
 * @brief Initialize diagnostics system
 * Call once at boot to reset all counters
 */
void diagnostics_init(void);

/**
 * @brief Get diagnostics structure
 * @return Pointer to diagnostics data (read-only)
 */
const diagnostics_t* diagnostics_get(void);

/**
 * @brief Increment UART TX timeout counter
 */
void diagnostics_uart_tx_timeout(void);

/**
 * @brief Increment UART RX timeout counter
 */
void diagnostics_uart_rx_timeout(void);

/**
 * @brief Record UART bytes sent
 * @param count Number of bytes sent
 */
void diagnostics_uart_tx_bytes(uint32_t count);

/**
 * @brief Record UART bytes received
 * @param count Number of bytes received
 */
void diagnostics_uart_rx_bytes(uint32_t count);

/**
 * @brief Record queue overflow
 * @param is_motor_queue true for motor queue, false for event queue
 */
void diagnostics_queue_overflow(bool is_motor_queue);

/**
 * @brief Update queue high-water mark
 * @param is_motor_queue true for motor queue, false for event queue
 * @param depth Current queue depth
 */
void diagnostics_queue_update_peak(bool is_motor_queue, uint16_t depth);

/**
 * @brief Record protocol error
 * @param error_type 0=checksum, 1=frame, 2=timeout
 */
void diagnostics_protocol_error(uint8_t error_type);

/**
 * @brief Record protocol command sent
 * @param is_query true if query, false if command
 */
void diagnostics_protocol_sent(bool is_query);

/**
 * @brief Record protocol retry
 */
void diagnostics_protocol_retry(void);

/**
 * @brief Record MCB communication result
 * @param success true if successful, false if failed
 */
void diagnostics_mcb_comm(bool success);

/**
 * @brief Record watchdog refresh
 */
void diagnostics_watchdog_refresh(void);

/**
 * @brief Get system uptime in seconds
 * @return Uptime in seconds since boot
 */
uint32_t diagnostics_get_uptime_sec(void);

/**
 * @brief Print comprehensive diagnostics report
 * Outputs all counters, statistics, and system health info
 */
void diagnostics_print_report(void);

/**
 * @brief Print error summary
 * Outputs just error counters and recent issues
 */
void diagnostics_print_errors(void);

/**
 * @brief Print performance metrics
 * Outputs uptime, watchdog stats, queue utilization
 */
void diagnostics_print_performance(void);

#endif // DIAGNOSTICS_H
