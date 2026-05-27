/**
 * @file hw.h
 * @brief Hardware abstraction shim for games integration
 *
 * Maps the bare-metal games API to FreeRTOS firmware equivalents.
 * Games call these functions; we route to our existing drivers.
 */

#ifndef HW_H
#define HW_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f1xx_hal.h"
#include "shared.h"
#include "FreeRTOS.h"
#include "task.h"

/* Also feeds the UI heartbeat, which is why the games call it even where the
 * value is discarded: a game runs on task_ui with the normal display loop
 * suspended, so any wait that does not pass through here (a held-button
 * release spin, say) stops the heartbeat and the watchdog resets the machine
 * in ~7 s. */
static inline uint32_t millis(void) {
    HEARTBEAT_UPDATE_UI();
    return xTaskGetTickCount();
}

static inline int8_t encoder_read_delta(void) {
    extern int8_t encoder_get_delta(void);
    return encoder_get_delta();
}

static inline bool btn_f1_pressed(void) {
    return !(GPIOC->IDR & (1 << 10));
}

static inline bool btn_f2_pressed(void) {
    return !(GPIOC->IDR & (1 << 11));
}

static inline bool btn_f3_pressed(void) {
    return !(GPIOC->IDR & (1 << 12));
}

static inline bool btn_f4_pressed(void) {
    return !(GPIOD->IDR & (1 << 2));
}

static inline bool btn_on_pressed(void) {
    return !(GPIOA->IDR & (1 << 15));
}

static inline bool btn_menu_pressed(void) {
    return !(GPIOB->IDR & (1 << 4));
}

static inline bool btn_enc_pressed(void) {
    return !(GPIOC->IDR & (1 << 15));
}

static inline bool pedal_pressed(void) {
    return (GPIOC->IDR & (1 << 3)) != 0;
}

static inline uint16_t adc_read_raw(void) {
    extern uint16_t depth_get_raw_adc(void);
    return depth_get_raw_adc();
}

static inline void buzz(uint16_t freq_hz, uint16_t duration_ms) {
    extern void buzzer_tone(uint16_t freq, uint16_t duration_ms);
    buzzer_tone(freq_hz, duration_ms);
}

static inline void hw_init(void) {
    // No-op in FreeRTOS firmware — hardware already initialized
}

#endif /* HW_H */
