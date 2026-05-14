/**
 * @file eeprom.c
 * @brief I2C EEPROM Driver Implementation
 *
 * Supports AT24Cxx series EEPROMs via I2C1.
 * Pin configuration: PB6 = SCL, PB7 = SDA
 */

#include "eeprom.h"
#include "config.h"
#include <string.h>

extern void uart_puts(const char* s);

/*===========================================================================*/
/* Private Variables                                                          */
/*===========================================================================*/

static I2C_HandleTypeDef hi2c_eeprom;
static bool eeprom_initialized = false;

/*===========================================================================*/
/* I2C Configuration                                                          */
/*===========================================================================*/

static void i2c_gpio_init(void) {
    GPIO_InitTypeDef gpio = {0};

    // Enable clocks
    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    // Configure I2C pins (PB6 = SCL, PB7 = SDA)
    // For I2C, use open-drain with external pull-ups
    gpio.Pin = EEPROM_SCL_PIN | EEPROM_SDA_PIN;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(EEPROM_I2C_PORT, &gpio);
}

static bool i2c_init(void) {
    i2c_gpio_init();

    hi2c_eeprom.Instance = EEPROM_I2C;
    hi2c_eeprom.Init.ClockSpeed = EEPROM_I2C_SPEED;
    hi2c_eeprom.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c_eeprom.Init.OwnAddress1 = 0;
    hi2c_eeprom.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c_eeprom.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c_eeprom.Init.OwnAddress2 = 0;
    hi2c_eeprom.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c_eeprom.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&hi2c_eeprom) != HAL_OK) {
        return false;
    }

    return true;
}

/*===========================================================================*/
/* Private Functions                                                          */
/*===========================================================================*/

/**
 * @brief Wait for EEPROM to be ready (poll acknowledge)
 */
static eeprom_status_t wait_for_eeprom(void) {
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < EEPROM_TIMEOUT_MS) {
        // Try to communicate with EEPROM
        if (HAL_I2C_IsDeviceReady(&hi2c_eeprom, EEPROM_I2C_ADDR << 1,
                                   1, EEPROM_WRITE_CYCLE_MS) == HAL_OK) {
            return EEPROM_OK;
        }
    }

    return EEPROM_TIMEOUT;
}

/**
 * @brief Write data within a single page
 */
static eeprom_status_t write_page(uint16_t addr, const uint8_t* data, size_t len) {
    uint8_t buffer[EEPROM_PAGE_SIZE + EEPROM_ADDR_SIZE];

    if (len > EEPROM_PAGE_SIZE) {
        len = EEPROM_PAGE_SIZE;
    }

    // Prepare buffer with address and data
    buffer[0] = (addr >> 8) & 0xFF;     // Address high byte
    buffer[1] = addr & 0xFF;             // Address low byte
    memcpy(&buffer[2], data, len);

    // Transmit address + data
    if (HAL_I2C_Master_Transmit(&hi2c_eeprom, EEPROM_I2C_ADDR << 1,
                                 buffer, len + EEPROM_ADDR_SIZE,
                                 EEPROM_TIMEOUT_MS) != HAL_OK) {
        return EEPROM_ERROR;
    }

    // Wait for write cycle to complete
    return wait_for_eeprom();
}

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

bool eeprom_init(void) {
    // Skip EEPROM for now - I2C hangs; use flash storage instead
    return false;
}

bool eeprom_is_present(void) {
    if (!eeprom_initialized) {
        return false;
    }

    return (HAL_I2C_IsDeviceReady(&hi2c_eeprom, EEPROM_I2C_ADDR << 1,
                                   3, EEPROM_TIMEOUT_MS) == HAL_OK);
}

eeprom_status_t eeprom_read(uint16_t addr, uint8_t* data, size_t len) {
    if (!eeprom_initialized || data == NULL || len == 0) {
        return EEPROM_ERROR;
    }

    if (addr + len > EEPROM_SIZE) {
        return EEPROM_ERROR;
    }

    uint8_t addr_buf[EEPROM_ADDR_SIZE];
    addr_buf[0] = (addr >> 8) & 0xFF;
    addr_buf[1] = addr & 0xFF;

    // Send address (write phase)
    if (HAL_I2C_Master_Transmit(&hi2c_eeprom, EEPROM_I2C_ADDR << 1,
                                 addr_buf, EEPROM_ADDR_SIZE,
                                 EEPROM_TIMEOUT_MS) != HAL_OK) {
        return EEPROM_ERROR;
    }

    // Read data
    if (HAL_I2C_Master_Receive(&hi2c_eeprom, EEPROM_I2C_ADDR << 1,
                                data, len, EEPROM_TIMEOUT_MS) != HAL_OK) {
        return EEPROM_ERROR;
    }

    return EEPROM_OK;
}

eeprom_status_t eeprom_write(uint16_t addr, const uint8_t* data, size_t len) {
    if (!eeprom_initialized || data == NULL || len == 0) {
        return EEPROM_ERROR;
    }

    if (addr + len > EEPROM_SIZE) {
        return EEPROM_ERROR;
    }

    size_t written = 0;

    while (written < len) {
        // Calculate bytes to write in this page
        uint16_t page_offset = (addr + written) % EEPROM_PAGE_SIZE;
        size_t bytes_to_write = EEPROM_PAGE_SIZE - page_offset;

        if (bytes_to_write > (len - written)) {
            bytes_to_write = len - written;
        }

        // Write this chunk
        eeprom_status_t status = write_page(addr + written,
                                            data + written,
                                            bytes_to_write);
        if (status != EEPROM_OK) {
            return status;
        }

        written += bytes_to_write;
    }

    return EEPROM_OK;
}

eeprom_status_t eeprom_read_byte(uint16_t addr, uint8_t* value) {
    return eeprom_read(addr, value, 1);
}

eeprom_status_t eeprom_write_byte(uint16_t addr, uint8_t value) {
    return eeprom_write(addr, &value, 1);
}

eeprom_status_t eeprom_erase(void) {
    uint8_t page_buf[EEPROM_PAGE_SIZE];
    memset(page_buf, 0xFF, EEPROM_PAGE_SIZE);

    for (uint16_t addr = 0; addr < EEPROM_SIZE; addr += EEPROM_PAGE_SIZE) {
        eeprom_status_t status = write_page(addr, page_buf, EEPROM_PAGE_SIZE);
        if (status != EEPROM_OK) {
            return status;
        }
    }

    return EEPROM_OK;
}

eeprom_status_t eeprom_wait_ready(void) {
    return wait_for_eeprom();
}
