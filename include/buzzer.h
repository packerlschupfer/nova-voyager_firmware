/**
 * @file buzzer.h
 * @brief Buzzer / Sound Feedback Module
 *
 * Provides audio feedback for button presses, errors, and notifications.
 * Uses PWM timer for tone generation.
 */

#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>
#include <stdbool.h>

/*===========================================================================*/
/* Buzzer Patterns                                                            */
/*===========================================================================*/

typedef enum {
    BEEP_NONE = 0,
    BEEP_CLICK,         // Short click for button press
    BEEP_CONFIRM,       // Confirmation beep
    BEEP_ERROR,         // Error beep (low tone)
    BEEP_WARNING,       // Warning (two short beeps)
    BEEP_STARTUP,       // Startup melody
    BEEP_SUCCESS,       // Success (ascending tones)
    BEEP_ALERT,         // Attention needed (three beeps)
    BEEP_TAURUS,        // Siemens Taurus locomotive chromatic scale (12 tones G→D)
    BEEP_BOOT_STAGE,    // Single rising tone for boot progress
} beep_pattern_t;

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

/**
 * @brief Initialize buzzer hardware
 */
void buzzer_init(void);

/**
 * @brief Play a single tone
 * @param freq Frequency in Hz (0 = silence)
 * @param duration_ms Duration in milliseconds
 */
void buzzer_tone(uint16_t freq, uint16_t duration_ms);

/**
 * @brief Play a beep pattern
 * @param pattern The beep pattern to play
 */
void buzzer_beep(beep_pattern_t pattern);

/**
 * @brief Play a beep pattern (non-blocking, call buzzer_update periodically)
 * @param pattern The beep pattern to play
 */
void buzzer_beep_async(beep_pattern_t pattern);

/**
 * @brief Update buzzer state (call from main loop for async beeps)
 */
void buzzer_update(void);

/**
 * @brief Stop any playing sound immediately
 */
void buzzer_stop(void);

/**
 * @brief Check if buzzer is currently playing
 * @return true if playing
 */
bool buzzer_is_playing(void);

/**
 * @brief Enable or disable buzzer
 * @param enabled true to enable
 */
void buzzer_set_enabled(bool enabled);

/**
 * @brief Check if buzzer is enabled
 * @return true if enabled
 */
bool buzzer_is_enabled(void);

#endif /* BUZZER_H */
