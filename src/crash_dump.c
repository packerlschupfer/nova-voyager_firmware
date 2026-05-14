/**
 * @file crash_dump.c
 * @brief Crash dump logging implementation
 */

#include "crash_dump.h"
#include "eeprom.h"
#include "stm32f1xx_hal.h"
#include <string.h>
#include <stdio.h>

/*===========================================================================*/
/* Configuration                                                              */
/*===========================================================================*/

#define CRASH_DUMP_EEPROM_ADDR  0x0800  // EEPROM address for crash dump

/*===========================================================================*/
/* Private Variables                                                          */
/*===========================================================================*/

static crash_dump_t current_crash;
static bool crash_exists = false;

/*===========================================================================*/
/* CRC Calculation                                                            */
/*===========================================================================*/

static uint16_t calc_crash_crc(const crash_dump_t* dump) {
    const uint8_t* data = (const uint8_t*)dump;
    size_t len = offsetof(crash_dump_t, checksum);
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/*===========================================================================*/
/* API Implementation                                                         */
/*===========================================================================*/

void crash_dump_init(void) {
    extern void uart_puts(const char* s);
    
    // Try to read crash dump from EEPROM
    crash_dump_t dump;
    bool read_ok = eeprom_read(CRASH_DUMP_EEPROM_ADDR, (uint8_t*)&dump, sizeof(dump));

    if (read_ok && dump.magic == CRASH_DUMP_MAGIC) {
        // Validate checksum
        uint16_t expected = calc_crash_crc(&dump);
        if (dump.checksum == expected) {
            // Valid crash dump found
            memcpy(&current_crash, &dump, sizeof(dump));
            crash_exists = true;
            
            uart_puts("\r\n!!! CRASH DUMP FOUND !!!\r\n");
            crash_dump_display();
            uart_puts("\r\nType 'CRASHCLEAR' to clear dump\r\n\r\n");
        }
    }
}

void crash_dump_log(crash_type_t type, uint32_t pc, uint32_t lr, uint32_t psr, const char* task_name) {
    crash_dump_t dump;
    memset(&dump, 0, sizeof(dump));

    dump.magic = CRASH_DUMP_MAGIC;
    dump.type = type;
    dump.timestamp = HAL_GetTick();
    dump.pc = pc;
    dump.lr = lr;
    dump.psr = psr;

    // Phase 1.3: Fixed string safety - use strncpy and ensure null termination
    if (task_name) {
        strncpy(dump.task_name, task_name, sizeof(dump.task_name) - 1);
        dump.task_name[sizeof(dump.task_name) - 1] = '\0';  // Ensure null termination
    } else {
        strncpy(dump.task_name, "UNKNOWN", sizeof(dump.task_name) - 1);
        dump.task_name[sizeof(dump.task_name) - 1] = '\0';  // Ensure null termination
    }

    // Calculate checksum
    dump.checksum = calc_crash_crc(&dump);

    // Write to EEPROM (best effort - may fail if EEPROM not available)
    eeprom_write(CRASH_DUMP_EEPROM_ADDR, (const uint8_t*)&dump, sizeof(dump));

    // Also save in RAM for display if EEPROM write failed
    memcpy(&current_crash, &dump, sizeof(dump));
    crash_exists = true;
}

bool crash_dump_exists(void) {
    return crash_exists;
}

const crash_dump_t* crash_dump_get(void) {
    return crash_exists ? &current_crash : NULL;
}

void crash_dump_clear(void) {
    // Clear EEPROM
    crash_dump_t empty;
    memset(&empty, 0, sizeof(empty));
    eeprom_write(CRASH_DUMP_EEPROM_ADDR, (const uint8_t*)&empty, sizeof(empty));

    // Clear RAM
    memset(&current_crash, 0, sizeof(current_crash));
    crash_exists = false;

    extern void uart_puts(const char* s);
    uart_puts("Crash dump cleared\r\n");
}

void crash_dump_display(void) {
    extern void uart_puts(const char* s);
    extern void diagnostics_print_errors(void);  // Phase 7: Show diagnostics

    if (!crash_exists) {
        uart_puts("No crash dump found\r\n\r\n");

        // Quick Win 2: Show diagnostics error summary when no crash
        uart_puts("System health check:\r\n");
        diagnostics_print_errors();
        return;
    }

    extern void uart_puts(const char* s);
    extern void uart_putc(char c);
    extern void print_num(int32_t n);
    extern void print_hex_byte(uint8_t b);

    uart_puts("=== CRASH DUMP ===\r\n");
    
    uart_puts("Type: ");
    switch (current_crash.type) {
        case CRASH_TYPE_HARD_FAULT:     uart_puts("HARD FAULT"); break;
        case CRASH_TYPE_STACK_OVERFLOW: uart_puts("STACK OVERFLOW"); break;
        case CRASH_TYPE_WATCHDOG:       uart_puts("WATCHDOG RESET"); break;
        case CRASH_TYPE_MALLOC_FAILED:  uart_puts("MALLOC FAILED"); break;
        case CRASH_TYPE_ASSERTION:      uart_puts("ASSERTION"); break;
        default:                        uart_puts("UNKNOWN"); break;
    }
    uart_puts("\r\n");

    uart_puts("Task: ");
    uart_puts(current_crash.task_name);
    uart_puts("\r\n");

    uart_puts("Timestamp: ");
    print_num(current_crash.timestamp);
    uart_puts(" ms\r\n");

    uart_puts("PC:  0x");
    for (int i = 3; i >= 0; i--) {
        print_hex_byte((current_crash.pc >> (i * 8)) & 0xFF);
    }
    uart_puts("\r\n");

    uart_puts("LR:  0x");
    for (int i = 3; i >= 0; i--) {
        print_hex_byte((current_crash.lr >> (i * 8)) & 0xFF);
    }
    uart_puts("\r\n");

    uart_puts("PSR: 0x");
    for (int i = 3; i >= 0; i--) {
        print_hex_byte((current_crash.psr >> (i * 8)) & 0xFF);
    }
    uart_puts("\r\n");

    uart_puts("==================\r\n\r\n");

    // Quick Win 2: Show diagnostics context with crash
    uart_puts("System state at crash:\r\n");
    diagnostics_print_errors();
}
