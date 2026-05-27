/**
 * @file log.h
 * @brief Logging macros with compile-time level control
 *
 * LOG_LEVEL defines which messages are compiled in:
 *   0 = None (all logging disabled)
 *   1 = Errors only
 *   2 = Errors + Warnings
 *   3 = Errors + Warnings + Info + Debug (verbose)
 *
 * Usage:
 *   LOG_ERROR("Motor fault: %d", code);
 *   LOG_WARN("Low battery");
 *   LOG_INFO("Starting motor");
 *   LOG_DEBUG("RPM=%d, load=%d", rpm, load);
 */

#ifndef LOG_H
#define LOG_H

// Default log level if not defined
#ifndef LOG_LEVEL
    #ifdef BUILD_DEBUG
        #define LOG_LEVEL 3  // Verbose in debug builds
    #else
        #define LOG_LEVEL 1  // Errors only in release
    #endif
#endif

// External UART functions
extern void uart_puts(const char* s);
extern void uart_putc(char c);

// Simple number printing (no printf dependency)
static inline void log_print_num(int32_t n) {
    if (n < 0) {
        uart_putc('-');
        n = -n;
    }
    if (n == 0) {
        uart_putc('0');
        return;
    }
    char buf[12];
    int i = 0;
    while (n > 0 && i < 11) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) uart_putc(buf[--i]);
}

// Log level 1: Errors
#if LOG_LEVEL >= 1
    #define LOG_ERROR(msg) do { uart_puts("[ERR] "); uart_puts(msg); uart_puts("\r\n"); } while(0)
    #define LOG_ERROR_NUM(msg, n) do { uart_puts("[ERR] "); uart_puts(msg); log_print_num(n); uart_puts("\r\n"); } while(0)
#else
    #define LOG_ERROR(msg) ((void)0)
    #define LOG_ERROR_NUM(msg, n) ((void)0)
#endif

// Log level 2: Warnings
#if LOG_LEVEL >= 2
    #define LOG_WARN(msg) do { uart_puts("[WRN] "); uart_puts(msg); uart_puts("\r\n"); } while(0)
    #define LOG_WARN_NUM(msg, n) do { uart_puts("[WRN] "); uart_puts(msg); log_print_num(n); uart_puts("\r\n"); } while(0)
#else
    #define LOG_WARN(msg) ((void)0)
    #define LOG_WARN_NUM(msg, n) ((void)0)
#endif

// Log level 3: Info and Debug
#if LOG_LEVEL >= 3
    #define LOG_INFO(msg) do { uart_puts("[INF] "); uart_puts(msg); uart_puts("\r\n"); } while(0)
    #define LOG_INFO_NUM(msg, n) do { uart_puts("[INF] "); uart_puts(msg); log_print_num(n); uart_puts("\r\n"); } while(0)
    #define LOG_DEBUG(msg) do { uart_puts("[DBG] "); uart_puts(msg); uart_puts("\r\n"); } while(0)
    #define LOG_DEBUG_NUM(msg, n) do { uart_puts("[DBG] "); uart_puts(msg); log_print_num(n); uart_puts("\r\n"); } while(0)
#else
    #define LOG_INFO(msg) ((void)0)
    #define LOG_INFO_NUM(msg, n) ((void)0)
    #define LOG_DEBUG(msg) ((void)0)
    #define LOG_DEBUG_NUM(msg, n) ((void)0)
#endif

// Stack high water mark reporting (debug builds only)
#ifdef ENABLE_STACK_WATERMARK
    #include "FreeRTOS.h"
    #include "task.h"
    #define LOG_STACK_WATERMARK(task) do { \
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(task); \
        uart_puts("[STK] "); uart_puts(pcTaskGetName(task)); uart_puts(": "); \
        log_print_num(hwm); uart_puts(" words free\r\n"); \
    } while(0)
#else
    #define LOG_STACK_WATERMARK(task) ((void)0)
#endif

#endif /* LOG_H */
