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
#include "eeprom_layout.h"

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

/* AUDIT FIX (CRITICAL, found 2026-08-30): this was an unpacked struct — 36
 * bytes with padding — written at 0xDC, while the custom settings block ran to
 * 0xE8. The two overlapped by 13 bytes, so logging a crash corrupted the
 * settings checksum and the next boot threw the settings away.
 *
 * The EEPROM has 80 bytes for the custom firmware and both records have to fit
 * (see include/eeprom_layout.h, which now asserts the split at build time).
 * Packing gets this to 19 bytes, at the cost of two fields:
 *
 *  - timestamp: HAL_GetTick() at the crash, i.e. milliseconds since boot. Read
 *    only at the next boot, where "how long had it been up" is nice to have
 *    and nothing acts on it.
 *  - psr: the fault handler already prints CFSR/HFSR/BFAR live over the
 *    console, which is where the useful fault-status bits are.
 *
 * pc, lr, the crash type and the task name are what identify a crash, and they
 * are all still here. task_name is 6 bytes because the longest real name is
 * "Motor" (5 + NUL); the writer truncates. */
typedef struct __attribute__((packed)) {
    uint16_t magic;          // CRASH_DUMP_MAGIC if valid
    uint8_t  type;           // crash_type_t
    uint32_t pc;             // Program counter
    uint32_t lr;             // Link register
    char     task_name[6];   // Task that crashed (Main/Motor/UI/Depth/Tap/HEAP)
    uint16_t checksum;       // CRC16 of dump
} crash_dump_t;

/* REVIEW FIX: the settings side of this EEPROM boundary has three
 * _Static_asserts and the crash side had none. crash_dump_t is exactly 19
 * bytes today; one added field and eeprom_write() fails its
 * "addr + len > EEPROM_SIZE" check, returning an error both call sites
 * discard — crash logging would just stop working, silently. */
_Static_assert(sizeof(crash_dump_t) == EE_CRASH_SIZE,
               "crash_dump_t must exactly fill the EEPROM crash region "
               "(EE_CRASH_BASE..0xFF) — see include/eeprom_layout.h");

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
