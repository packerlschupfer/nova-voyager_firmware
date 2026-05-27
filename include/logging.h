/**
 * @file logging.h
 * @brief Conditional logging framework for production builds
 */

#ifndef LOGGING_H
#define LOGGING_H

#include "serial_console.h"

/*===========================================================================*/
/* Log Levels                                                                */
/*===========================================================================*/

typedef enum {
    LOG_NONE = 0,    // No logging
    LOG_ERROR = 1,   // Errors only
    LOG_WARN = 2,    // Warnings and errors
    LOG_INFO = 3,    // Info, warnings, errors
    LOG_DEBUG = 4    // Everything (verbose)
} log_level_t;

// Set log level at compile time (override in platformio.ini)
#ifndef LOG_LEVEL
    #ifdef BUILD_DEBUG
        #define LOG_LEVEL LOG_DEBUG  // Verbose in debug builds
    #else
        #define LOG_LEVEL LOG_INFO   // Production: info and above
    #endif
#endif

/*===========================================================================*/
/* Logging Macros                                                            */
/*===========================================================================*/

// Conditional logging - only outputs if level >= LOG_LEVEL
#define LOG_ERROR_MSG(msg)   do { if (LOG_LEVEL >= LOG_ERROR) { uart_puts("[ERROR] "); uart_puts(msg); } } while(0)
#define LOG_WARN_MSG(msg)    do { if (LOG_LEVEL >= LOG_WARN)  { uart_puts("[WARN] ");  uart_puts(msg); } } while(0)
#define LOG_INFO_MSG(msg)    do { if (LOG_LEVEL >= LOG_INFO)  { uart_puts("[INFO] ");  uart_puts(msg); } } while(0)
#define LOG_DEBUG_MSG(msg)   do { if (LOG_LEVEL >= LOG_DEBUG) { uart_puts("[DEBUG] "); uart_puts(msg); } } while(0)

// With tag prefix
#define LOG_ERROR_TAG(tag, msg)  do { if (LOG_LEVEL >= LOG_ERROR) { uart_puts("["); uart_puts(tag); uart_puts("] "); uart_puts(msg); } } while(0)
#define LOG_WARN_TAG(tag, msg)   do { if (LOG_LEVEL >= LOG_WARN)  { uart_puts("["); uart_puts(tag); uart_puts("] "); uart_puts(msg); } } while(0)
#define LOG_INFO_TAG(tag, msg)   do { if (LOG_LEVEL >= LOG_INFO)  { uart_puts("["); uart_puts(tag); uart_puts("] "); uart_puts(msg); } } while(0)
#define LOG_DEBUG_TAG(tag, msg)  do { if (LOG_LEVEL >= LOG_DEBUG) { uart_puts("["); uart_puts(tag); uart_puts("] "); uart_puts(msg); } } while(0)

// Convenience aliases for common subsystems
#define LOG_TAP_DEBUG(msg)   LOG_DEBUG_TAG("TAP", msg)
#define LOG_MOTOR_DEBUG(msg) LOG_DEBUG_TAG("MOTOR", msg)
#define LOG_UI_DEBUG(msg)    LOG_DEBUG_TAG("UI", msg)
#define LOG_DEPTH_DEBUG(msg) LOG_DEBUG_TAG("DEPTH", msg)

/*===========================================================================*/
/* Usage Notes                                                               */
/*===========================================================================*/

// Example usage:
//   LOG_INFO_MSG("System initialized\r\n");
//   LOG_DEBUG_TAG("TAP", "Baseline learned\r\n");
//   LOG_ERROR_MSG("Motor timeout!\r\n");
//
// In production builds (LOG_LEVEL=INFO):
//   - LOG_DEBUG calls compile to nothing (zero overhead)
//   - LOG_INFO/WARN/ERROR still output
//
// To set log level in platformio.ini:
//   build_flags = -DLOG_LEVEL=LOG_INFO

#endif // LOGGING_H
