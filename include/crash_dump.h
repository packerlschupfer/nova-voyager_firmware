/**
 * @file crash_dump.h
 * @brief Crash dump logging for post-mortem analysis
 *
 * Logs crash information to EEPROM for diagnosing field failures
 */

#ifndef CRASH_DUMP_H
#define CRASH_DUMP_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Crash Dump Structure                                                      */
/*===========================================================================*/

#define CRASH_DUMP_MAGIC    0xDEAD

typedef enum {
    CRASH_TYPE_NONE = 0,
    CRASH_TYPE_HARD_FAULT,
    CRASH_TYPE_STACK_OVERFLOW,
    CRASH_TYPE_WATCHDOG,
    CRASH_TYPE_MALLOC_FAILED,
    CRASH_TYPE_ASSERTION
} crash_type_t;

typedef struct {
    uint16_t magic;          // CRASH_DUMP_MAGIC if valid
    crash_type_t type;       // Crash type
    uint32_t timestamp;      // HAL_GetTick() at crash
    uint32_t pc;             // Program counter
    uint32_t lr;             // Link register
    uint32_t psr;            // Program status register
    char task_name[16];      // Task that crashed
    uint16_t checksum;       // CRC16 of dump
} crash_dump_t;

/*===========================================================================*/
/* API Functions                                                              */
/*===========================================================================*/

/**
 * @brief Initialize crash dump system
 * Checks for existing crash dump and displays if found
 */
void crash_dump_init(void);

/**
 * @brief Log crash to EEPROM
 * @param type Crash type
 * @param pc Program counter
 * @param lr Link register
 * @param psr Program status register
 * @param task_name Name of crashed task (NULL if unknown)
 */
void crash_dump_log(crash_type_t type, uint32_t pc, uint32_t lr, uint32_t psr, const char* task_name);

/**
 * @brief Check if crash dump exists
 * @return true if valid crash dump found
 */
bool crash_dump_exists(void);

/**
 * @brief Get crash dump
 * @return Pointer to crash dump structure (NULL if none)
 */
const crash_dump_t* crash_dump_get(void);

/**
 * @brief Clear crash dump from EEPROM
 */
void crash_dump_clear(void);

/**
 * @brief Display crash dump on serial console
 */
void crash_dump_display(void);

#endif // CRASH_DUMP_H
