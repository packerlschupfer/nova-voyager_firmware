/**
 * @file vibration.h
 * @brief 3-axis accelerometer on the shared PC4/PC5 I2C bus.
 *
 * Recovered from the original Teknatool firmware (R2P06k) by disassembly on
 * 2026-09-05, after the operator confirmed he had actually seen the "Excess
 * Vibration" warning fire — which is the only reason anyone kept looking.
 *
 * The sensor is NOT on the MCB and is NOT an ADC input. It is an I2C device at
 * 7-bit address 0x1D sharing the bus with the AT24C02 EEPROM. That is why no
 * MCB register scan ever found it and why there is no serial command carrying
 * the sensitivity level: the level is a local threshold applied to a locally
 * read sensor, and never leaves the HMI.
 */
#ifndef VIBRATION_H
#define VIBRATION_H

#include <stdint.h>
#include <stdbool.h>

/** 7-bit I2C address of the accelerometer (OEM: writes 0x3A, reads 0x3B). */
#define VIBRATION_I2C_ADDR   0x1D

/** Result of one evaluation, matching the OEM's three-way return. */
typedef enum {
    VIBRATION_OK        = 0,   /**< below both bands */
    VIBRATION_ELEVATED  = 1,   /**< OEM "Significant Vibration" */
    VIBRATION_EXCESS    = 2    /**< OEM "Excess Vibration" — it stops the motor */
} vibration_level_t;

/** Raw signed 10-bit axes, as the OEM assembles them. */
typedef struct {
    int16_t x;
    int16_t y;   /**< carries the OEM's +250 offset */
    int16_t z;
    bool    valid;
} vibration_axes_t;

/**
 * @brief Read all three axes.
 * @return false if the device did not respond (then axes.valid is false).
 */
bool vibration_read_axes(vibration_axes_t* axes);

/**
 * @brief Read and classify against the OEM thresholds for a sensitivity level.
 * @param sensitivity 0 = disabled (always VIBRATION_OK), 1 = LOW, 2 = MEDIUM,
 *        3 = HIGH — the same 0-3 the menu already stores in
 *        settings.sensor.vibration_sensitivity.
 */
vibration_level_t vibration_evaluate(uint8_t sensitivity);

/** WHO_AM_I register and its expected value (OEM checks exactly this). */
#define VIBRATION_REG_WHOAMI   0x0D
#define VIBRATION_WHOAMI_VALUE 0x5A

/**
 * @brief True only if an MMA845x actually identifies itself at 0x1D.
 * @note Checks WHO_AM_I, not just an address ACK — the OEM does the same and
 *       disables the feature when it fails.
 */
bool vibration_present(void);

#endif
