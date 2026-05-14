/**
 * @file diagnostics.c
 * @brief System Diagnostics and Telemetry Implementation
 *
 * Phase 7: Enhanced system health monitoring
 */

#include "diagnostics.h"
#include "shared.h"
#include "stm32f1xx_hal.h"
#include <string.h>

// External UART functions
extern void uart_puts(const char* s);
extern void print_num(int32_t n);

/*===========================================================================*/
/* Module State                                                               */
/*===========================================================================*/

// [MODULE_LOCAL] Only accessed from diagnostic tracking functions
// No mutex needed - most increments are from single task contexts
static diagnostics_t diag;

/*===========================================================================*/
/* Public API Implementation                                                  */
/*===========================================================================*/

void diagnostics_init(void) {
    memset(&diag, 0, sizeof(diag));
    diag.boot_timestamp_ms = HAL_GetTick();
}

const diagnostics_t* diagnostics_get(void) {
    return &diag;
}

void diagnostics_uart_tx_timeout(void) {
    diag.uart_tx_timeouts++;
}

void diagnostics_uart_rx_timeout(void) {
    diag.uart_rx_timeouts++;
}

void diagnostics_uart_tx_bytes(uint32_t count) {
    diag.uart_bytes_sent += count;
}

void diagnostics_uart_rx_bytes(uint32_t count) {
    diag.uart_bytes_received += count;
}

void diagnostics_queue_overflow(bool is_motor_queue) {
    if (is_motor_queue) {
        diag.motor_queue_overflows++;
    } else {
        diag.event_queue_overflows++;
    }
}

void diagnostics_queue_update_peak(bool is_motor_queue, uint16_t depth) {
    if (is_motor_queue) {
        if (depth > diag.motor_queue_peak) {
            diag.motor_queue_peak = depth;
        }
    } else {
        if (depth > diag.event_queue_peak) {
            diag.event_queue_peak = depth;
        }
    }
}

void diagnostics_protocol_error(uint8_t error_type) {
    switch (error_type) {
        case 0: diag.protocol_checksum_errors++; break;
        case 1: diag.protocol_frame_errors++; break;
        case 2: diag.protocol_timeout_errors++; break;
    }
}

void diagnostics_protocol_sent(bool is_query) {
    if (is_query) {
        diag.protocol_queries_sent++;
    } else {
        diag.protocol_commands_sent++;
    }
}

void diagnostics_protocol_retry(void) {
    diag.protocol_retries++;
}

void diagnostics_mcb_comm(bool success) {
    if (success) {
        diag.mcb_comm_success++;
        diag.mcb_comm_failures = 0;  // Reset failure counter
    } else {
        diag.mcb_comm_failures++;
    }
    diag.mcb_total_queries++;
}

void diagnostics_watchdog_refresh(void) {
    diag.watchdog_refresh_count++;
}

uint32_t diagnostics_get_uptime_sec(void) {
    uint32_t now = HAL_GetTick();
    uint32_t uptime_ms = now - diag.boot_timestamp_ms;
    return uptime_ms / 1000;
}

/*===========================================================================*/
/* Diagnostic Reporting Functions                                            */
/*===========================================================================*/

void diagnostics_print_report(void) {
    uart_puts("\r\n╔═══════════════════════════════════════╗\r\n");
    uart_puts("║     SYSTEM DIAGNOSTICS REPORT         ║\r\n");
    uart_puts("╚═══════════════════════════════════════╝\r\n\r\n");

    // Uptime
    uint32_t uptime = diagnostics_get_uptime_sec();
    uart_puts("UPTIME: ");
    print_num(uptime / 3600);
    uart_puts("h ");
    print_num((uptime % 3600) / 60);
    uart_puts("m ");
    print_num(uptime % 60);
    uart_puts("s\r\n");

    uart_puts("Watchdog refreshes: ");
    print_num(diag.watchdog_refresh_count);
    uart_puts("\r\n\r\n");

    // UART statistics
    uart_puts("─── UART Statistics ───\r\n");
    uart_puts("TX bytes: ");
    print_num(diag.uart_bytes_sent);
    uart_puts("\r\n");
    uart_puts("RX bytes: ");
    print_num(diag.uart_bytes_received);
    uart_puts("\r\n");
    uart_puts("TX timeouts: ");
    print_num(diag.uart_tx_timeouts);
    uart_puts("\r\n");
    uart_puts("RX timeouts: ");
    print_num(diag.uart_rx_timeouts);
    uart_puts("\r\n\r\n");

    // Queue statistics
    uart_puts("─── Queue Statistics ───\r\n");

    if (g_event_queue) {
        UBaseType_t evt_msgs = uxQueueMessagesWaiting(g_event_queue);
        UBaseType_t evt_total = evt_msgs + uxQueueSpacesAvailable(g_event_queue);
        uart_puts("Event Queue: ");
        print_num(evt_msgs);
        uart_puts(" / ");
        print_num(evt_total);
        uart_puts(" (peak: ");
        print_num(diag.event_queue_peak);
        uart_puts(")\r\n");
    }

    if (g_motor_cmd_queue) {
        UBaseType_t mot_msgs = uxQueueMessagesWaiting(g_motor_cmd_queue);
        UBaseType_t mot_total = mot_msgs + uxQueueSpacesAvailable(g_motor_cmd_queue);
        uart_puts("Motor Queue: ");
        print_num(mot_msgs);
        uart_puts(" / ");
        print_num(mot_total);
        uart_puts(" (peak: ");
        print_num(diag.motor_queue_peak);
        uart_puts(")\r\n");
    }

    uart_puts("Event overflows: ");
    print_num(diag.event_queue_overflows);
    uart_puts("\r\n");
    uart_puts("Motor overflows: ");
    print_num(diag.motor_queue_overflows);
    uart_puts("\r\n\r\n");

    // Protocol statistics
    uart_puts("─── Protocol Statistics ───\r\n");
    uart_puts("Commands sent: ");
    print_num(diag.protocol_commands_sent);
    uart_puts("\r\n");
    uart_puts("Queries sent: ");
    print_num(diag.protocol_queries_sent);
    uart_puts("\r\n");
    uart_puts("Retries: ");
    print_num(diag.protocol_retries);
    uart_puts("\r\n");
    uart_puts("Checksum errors: ");
    print_num(diag.protocol_checksum_errors);
    uart_puts("\r\n");
    uart_puts("Frame errors: ");
    print_num(diag.protocol_frame_errors);
    uart_puts("\r\n");
    uart_puts("Timeouts: ");
    print_num(diag.protocol_timeout_errors);
    uart_puts("\r\n\r\n");

    // MCB communication health
    uart_puts("─── MCB Communication ───\r\n");
    uart_puts("Total queries: ");
    print_num(diag.mcb_total_queries);
    uart_puts("\r\n");
    uart_puts("Successful: ");
    print_num(diag.mcb_comm_success);
    uart_puts("\r\n");
    uart_puts("Failed: ");
    print_num(diag.mcb_total_queries - diag.mcb_comm_success);
    uart_puts("\r\n");

    if (diag.mcb_total_queries > 0) {
        uint32_t success_rate = (diag.mcb_comm_success * 100) / diag.mcb_total_queries;
        uart_puts("Success rate: ");
        print_num(success_rate);
        uart_puts("%\r\n");
    }

    uart_puts("\r\n");
}

void diagnostics_print_errors(void) {
    uart_puts("\r\n═══ ERROR SUMMARY ═══\r\n\r\n");

    bool any_errors = false;

    // UART errors
    if (diag.uart_tx_timeouts > 0 || diag.uart_rx_timeouts > 0) {
        uart_puts("⚠ UART Timeouts:\r\n");
        uart_puts("  TX: ");
        print_num(diag.uart_tx_timeouts);
        uart_puts(" | RX: ");
        print_num(diag.uart_rx_timeouts);
        uart_puts("\r\n");
        any_errors = true;
    }

    // Queue errors
    if (diag.event_queue_overflows > 0 || diag.motor_queue_overflows > 0) {
        uart_puts("⚠ Queue Overflows:\r\n");
        uart_puts("  Event: ");
        print_num(diag.event_queue_overflows);
        uart_puts(" | Motor: ");
        print_num(diag.motor_queue_overflows);
        uart_puts("\r\n");
        any_errors = true;
    }

    // Protocol errors
    uint32_t proto_errors = diag.protocol_checksum_errors +
                           diag.protocol_frame_errors +
                           diag.protocol_timeout_errors;
    if (proto_errors > 0) {
        uart_puts("⚠ Protocol Errors:\r\n");
        uart_puts("  Checksum: ");
        print_num(diag.protocol_checksum_errors);
        uart_puts(" | Frame: ");
        print_num(diag.protocol_frame_errors);
        uart_puts(" | Timeout: ");
        print_num(diag.protocol_timeout_errors);
        uart_puts("\r\n");
        any_errors = true;
    }

    // MCB communication health
    uint32_t mcb_failures = diag.mcb_total_queries - diag.mcb_comm_success;
    if (mcb_failures > 0) {
        uart_puts("⚠ MCB Communication:\r\n");
        uart_puts("  Failed: ");
        print_num(mcb_failures);
        uart_puts(" / ");
        print_num(diag.mcb_total_queries);
        uint32_t fail_rate = (mcb_failures * 100) / diag.mcb_total_queries;
        uart_puts(" (");
        print_num(fail_rate);
        uart_puts("% failure rate)\r\n");
        any_errors = true;
    }

    if (!any_errors) {
        uart_puts("✓ No errors detected\r\n");
    }

    uart_puts("\r\n");
}

void diagnostics_print_performance(void) {
    uart_puts("\r\n═══ PERFORMANCE METRICS ═══\r\n\r\n");

    // Uptime
    uint32_t uptime = diagnostics_get_uptime_sec();
    uart_puts("Uptime: ");
    print_num(uptime / 3600);
    uart_puts(":");
    uint32_t mins = (uptime % 3600) / 60;
    if (mins < 10) uart_puts("0");
    print_num(mins);
    uart_puts(":");
    uint32_t secs = uptime % 60;
    if (secs < 10) uart_puts("0");
    print_num(secs);
    uart_puts("\r\n\r\n");

    // Watchdog
    uart_puts("Watchdog refreshes: ");
    print_num(diag.watchdog_refresh_count);
    if (uptime > 0) {
        uart_puts(" (");
        print_num(diag.watchdog_refresh_count / uptime);
        uart_puts(" /sec)");
    }
    uart_puts("\r\n\r\n");

    // Queue utilization
    uart_puts("Queue Peak Depth:\r\n");
    uart_puts("  Event: ");
    print_num(diag.event_queue_peak);
    uart_puts(" / 32 (");
    print_num((diag.event_queue_peak * 100) / 32);
    uart_puts("%)\r\n");
    uart_puts("  Motor: ");
    print_num(diag.motor_queue_peak);
    uart_puts(" / 16 (");
    print_num((diag.motor_queue_peak * 100) / 16);
    uart_puts("%)\r\n\r\n");

    // Protocol throughput
    if (uptime > 0) {
        uart_puts("Protocol Throughput:\r\n");
        uart_puts("  Commands/sec: ");
        print_num(diag.protocol_commands_sent / uptime);
        uart_puts("\r\n");
        uart_puts("  Queries/sec: ");
        print_num(diag.protocol_queries_sent / uptime);
        uart_puts("\r\n");
        uart_puts("  Total/sec: ");
        print_num((diag.protocol_commands_sent + diag.protocol_queries_sent) / uptime);
        uart_puts("\r\n\r\n");
    }

    // UART throughput
    if (uptime > 0) {
        uart_puts("UART Throughput:\r\n");
        uart_puts("  TX: ");
        print_num(diag.uart_bytes_sent / uptime);
        uart_puts(" bytes/sec\r\n");
        uart_puts("  RX: ");
        print_num(diag.uart_bytes_received / uptime);
        uart_puts(" bytes/sec\r\n");
    }

    uart_puts("\r\n");
}
