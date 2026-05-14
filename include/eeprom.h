/**
 * @file eeprom.h
 * @brief I2C EEPROM Driver Interface
 *
 * Supports AT24Cxx series EEPROMs (AT24C32, AT24C64, etc.)
 * Used for persistent settings storage matching original Teknatool firmware.
 */

#ifndef EEPROM_H
#define EEPROM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*===========================================================================*/
/* Configuration                                                              */
/*===========================================================================*/

// I2C EEPROM device address (7-bit, without R/W bit)
// AT24Cxx default is 0x50 (A0=A1=A2=0)
#define EEPROM_I2C_ADDR         0x50

// EEPROM size and page configuration
// AT24C32: 4KB (4096 bytes), 32-byte pages, 2-byte addressing
// AT24C64: 8KB (8192 bytes), 32-byte pages, 2-byte addressing
#define EEPROM_SIZE             4096
#define EEPROM_PAGE_SIZE        32
#define EEPROM_ADDR_SIZE        2       // 2-byte addressing for > 256 bytes

// Timeout values
#define EEPROM_TIMEOUT_MS       100
#define EEPROM_WRITE_CYCLE_MS   5       // Max write cycle time

/*===========================================================================*/
/* Public Types                                                               */
/*===========================================================================*/

typedef enum {
    EEPROM_OK = 0,
    EEPROM_ERROR,
    EEPROM_BUSY,
    EEPROM_TIMEOUT,
    EEPROM_NOT_FOUND
} eeprom_status_t;

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

/**
 * @brief Initialize I2C peripheral for EEPROM communication
 * @return true if EEPROM detected, false otherwise
 */
bool eeprom_init(void);

/**
 * @brief Check if EEPROM is present and responding
 * @return true if EEPROM responds to address
 */
bool eeprom_is_present(void);

/**
 * @brief Read bytes from EEPROM
 * @param addr Starting address in EEPROM
 * @param data Buffer to store read data
 * @param len Number of bytes to read
 * @return EEPROM_OK on success
 */
eeprom_status_t eeprom_read(uint16_t addr, uint8_t* data, size_t len);

/**
 * @brief Write bytes to EEPROM
 * @param addr Starting address in EEPROM
 * @param data Data to write
 * @param len Number of bytes to write
 * @return EEPROM_OK on success
 * @note Handles page boundary crossing automatically
 */
eeprom_status_t eeprom_write(uint16_t addr, const uint8_t* data, size_t len);

/**
 * @brief Read a single byte from EEPROM
 * @param addr Address to read from
 * @param value Pointer to store read value
 * @return EEPROM_OK on success
 */
eeprom_status_t eeprom_read_byte(uint16_t addr, uint8_t* value);

/**
 * @brief Write a single byte to EEPROM
 * @param addr Address to write to
 * @param value Value to write
 * @return EEPROM_OK on success
 */
eeprom_status_t eeprom_write_byte(uint16_t addr, uint8_t value);

/**
 * @brief Erase EEPROM (fill with 0xFF)
 * @return EEPROM_OK on success
 */
eeprom_status_t eeprom_erase(void);

/**
 * @brief Wait for EEPROM write cycle to complete
 * @return EEPROM_OK when ready
 */
eeprom_status_t eeprom_wait_ready(void);

#endif /* EEPROM_H */
