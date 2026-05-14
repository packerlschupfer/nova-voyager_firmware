/**
 * @file buzzer.c
 * @brief Buzzer / Sound Feedback Implementation
 *
 * Uses TIM1 PWM for tone generation on PA8.
 * Includes Siemens Taurus locomotive-style chromatic startup sound.
 */

#include "buzzer.h"
#include "config.h"
#include "stm32f1xx_hal.h"

#if BUZZER_ENABLED

/*===========================================================================*/
/* Private Variables                                                          */
/*===========================================================================*/

static TIM_HandleTypeDef htim_buzzer;
static bool buzzer_enabled = true;
static bool buzzer_initialized = false;

// Boot stage counter for progressive tones
static uint8_t boot_stage = 0;

// Taurus chromatic scale frequencies (G3 to G4, full octave)
// 8 tones for 8 boot stages - clean octave ascent
static const uint16_t taurus_freqs[8] = {
    196,  // G3  - start low
    220,  // A3
    247,  // B3
    262,  // C4  - middle C
    294,  // D4
    330,  // E4
    370,  // F#4
    392   // G4  - octave complete!
};

/*===========================================================================*/
/* Private Functions                                                          */
/*===========================================================================*/

static void pwm_start(uint16_t freq) {
    if (!buzzer_initialized || freq == 0) return;

    // Calculate period for desired frequency
    // Timer clock = 1MHz after prescaler
    // Period = 1000000 / freq
    uint32_t period = 1000000 / freq;
    if (period < 2) period = 2;
    if (period > 65535) period = 65535;

    // Direct register access for reliable frequency changes
    TIM1->CR1 &= ~TIM_CR1_CEN;      // Stop timer
    TIM1->ARR = period - 1;         // Set period
    TIM1->CCR1 = period / 2;        // 50% duty cycle
    TIM1->CNT = 0;                  // Reset counter
    TIM1->EGR = TIM_EGR_UG;         // Force update

    // Configure pin as AF
    GPIOA->CRH &= ~(0xF << 0);      // Clear PA8 config
    GPIOA->CRH |= (0xB << 0);       // PA8 = AF push-pull, 50MHz

    // Enable outputs
    TIM1->BDTR |= TIM_BDTR_MOE;     // Main output enable
    TIM1->CCER |= TIM_CCER_CC1E;    // Capture/compare output enable
    TIM1->CR1 |= TIM_CR1_CEN;       // Start timer
}

static void pwm_stop(void) {
    if (!buzzer_initialized) return;

    // Direct register access
    TIM1->CR1 &= ~TIM_CR1_CEN;      // Stop timer
    TIM1->BDTR &= ~TIM_BDTR_MOE;    // Disable main output
    TIM1->CCER &= ~TIM_CCER_CC1E;   // Disable capture/compare output

    // Reconfigure PA8 as GPIO output low
    GPIOA->CRH &= ~(0xF << 0);      // Clear PA8 config
    GPIOA->CRH |= (0x3 << 0);       // PA8 = output push-pull, 50MHz
    GPIOA->BRR = (1 << 8);          // PA8 low
}

// Simple busy-wait delay (HAL_Delay may not work before scheduler starts)
static void delay_ms(uint16_t ms) {
    // At 120MHz: calibrated for roughly 1ms per outer loop
    // Inner loop ~8 cycles, 15000 iterations ≈ 120000 cycles ≈ 1ms
    for (uint16_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 15000; j++) {
            __NOP();
        }
    }
}

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

void buzzer_init(void) {
    // Enable clocks
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    // Configure PA8 as TIM1_CH1 output (alternate function push-pull)
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = BUZZER_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(BUZZER_PORT, &gpio);

    // Configure TIM1 for PWM
    htim_buzzer.Instance = BUZZER_TIM;
#if USE_120MHZ
    htim_buzzer.Init.Prescaler = 119;  // 120MHz / 120 = 1MHz
#else
    htim_buzzer.Init.Prescaler = 71;   // 72MHz / 72 = 1MHz
#endif
    htim_buzzer.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim_buzzer.Init.Period = 999;    // Default 1kHz
    htim_buzzer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim_buzzer.Init.RepetitionCounter = 0;
    htim_buzzer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;  // Immediate ARR updates
    HAL_TIM_PWM_Init(&htim_buzzer);

    // Configure PWM channel
    TIM_OC_InitTypeDef sConfigOC = {0};
    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = 500;  // 50% duty cycle
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
    sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    HAL_TIM_PWM_ConfigChannel(&htim_buzzer, &sConfigOC, BUZZER_TIM_CHANNEL);

    // Enable TIM1 main output (required for advanced timers)
    __HAL_TIM_MOE_ENABLE(&htim_buzzer);

    buzzer_initialized = true;
    buzzer_enabled = true;
    boot_stage = 0;
}

void buzzer_tone(uint16_t freq, uint16_t duration_ms) {
    if (!buzzer_enabled || !buzzer_initialized) return;

    if (freq > 0) {
        pwm_start(freq);
        delay_ms(duration_ms);
        pwm_stop();
    } else {
        delay_ms(duration_ms);
    }
}

void buzzer_beep(beep_pattern_t pattern) {
    if (!buzzer_enabled) return;

    switch (pattern) {
        case BEEP_CLICK:
            buzzer_tone(TONE_CLICK, BEEP_SHORT);
            break;

        case BEEP_CONFIRM:
            buzzer_tone(TONE_SUCCESS, BEEP_MEDIUM);
            break;

        case BEEP_ERROR:
            buzzer_tone(TONE_ERROR, BEEP_LONG);
            break;

        case BEEP_WARNING:
            buzzer_tone(TONE_ERROR, BEEP_SHORT);
            buzzer_tone(0, 50);
            buzzer_tone(TONE_ERROR, BEEP_SHORT);
            break;

        case BEEP_STARTUP:
            // Simple ascending startup (C-E-G)
            buzzer_tone(523, 80);   // C5
            buzzer_tone(659, 80);   // E5
            buzzer_tone(784, 120);  // G5
            break;

        case BEEP_SUCCESS:
            buzzer_tone(1000, 50);
            buzzer_tone(1500, 50);
            buzzer_tone(2000, 100);
            break;

        case BEEP_ALERT:
            buzzer_tone(2000, BEEP_SHORT);
            buzzer_tone(0, 50);
            buzzer_tone(2000, BEEP_SHORT);
            buzzer_tone(0, 50);
            buzzer_tone(2000, BEEP_SHORT);
            break;

        case BEEP_TAURUS:
            // Siemens Taurus ES 64 U2 locomotive chromatic scale
            // 8 tones ascending from G3 to G4 (full octave)
            for (int i = 0; i < 8; i++) {
                buzzer_tone(taurus_freqs[i], 80);  // 80ms per tone
                buzzer_tone(0, 15);  // 15ms gap between notes
            }
            break;

        case BEEP_BOOT_STAGE:
            // Progressive boot tone - plays through Taurus scale
            // 8 boot stages = 8 Taurus tones (G3 to G4)
            if (boot_stage < 8) {
                buzzer_tone(taurus_freqs[boot_stage], 50);  // 50ms per tone (was 120)
                boot_stage++;
            }
            break;

        default:
            break;
    }
}

void buzzer_beep_async(beep_pattern_t pattern) {
    // For now, just call synchronous version
    // Async implementation can be added if needed for runtime beeps
    buzzer_beep(pattern);
}

void buzzer_update(void) {
    // Placeholder for async state machine
    // Not needed for boot sequence (blocking is OK during init)
}

void buzzer_stop(void) {
    pwm_stop();
}

bool buzzer_is_playing(void) {
    return false;  // Simplified - blocking implementation
}

void buzzer_set_enabled(bool enabled) {
    buzzer_enabled = enabled;
    if (!enabled) {
        buzzer_stop();
    }
}

bool buzzer_is_enabled(void) {
    return buzzer_enabled;
}

#else  // BUZZER_ENABLED == 0

// Stub implementations when buzzer is disabled
void buzzer_init(void) {}
void buzzer_tone(uint16_t freq, uint16_t duration_ms) { (void)freq; (void)duration_ms; }
void buzzer_beep(beep_pattern_t pattern) { (void)pattern; }
void buzzer_beep_async(beep_pattern_t pattern) { (void)pattern; }
void buzzer_update(void) {}
void buzzer_stop(void) {}
bool buzzer_is_playing(void) { return false; }
void buzzer_set_enabled(bool enabled) { (void)enabled; }
bool buzzer_is_enabled(void) { return false; }

#endif  // BUZZER_ENABLED
