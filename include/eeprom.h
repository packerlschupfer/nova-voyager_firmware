/**
 * @file eeprom.h
 * @brief I2C EEPROM Driver Interface
 *
 * Supports AT24C02 EEPROM (256 bytes) via bit-bang I2C on PC4(SCL)/PC5(SDA).
 * Original Teknatool firmware uses this bus. PB6/PB14 path is write-protected.
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

// AT24C02: 256 bytes, 8-byte pages, 1-byte addressing
// Defined in config.h: EEPROM_SIZE, EEPROM_PAGE_SIZE, EEPROM_ADDR_SIZE

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
 * @brief Read one register from any device on the PC4/PC5 I2C bus.
 * @note The bus is shared: the AT24C02 sits at 0x50 and the OEM firmware's
 *       vibration accelerometer at 0x1D. Uses the same mutex as the EEPROM
 *       accessors, so it is safe against a concurrent settings write.
 */
bool i2c_read_device_reg(uint8_t addr7, uint8_t reg, uint8_t* value);

/** @brief Slow the bit-banged bus by this factor (1 = normal). Test hook. */
void i2c_set_slow_factor(uint16_t factor);

/** @brief Register read ignoring ACK — reproduces the OEM's read for testing. */
bool i2c_read_device_reg_noack(uint8_t addr7, uint8_t reg, uint8_t* value);

/** @brief Address-only presence probe — the honest question for a bus scan. */
bool i2c_probe_device(uint8_t addr7);

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

#endif /* EEPROM_H */
