/**
 * @file task_ui.c
 * @brief UI Task - Buttons, Encoder, Display Coordination
 *
 * FreeRTOS task that handles:
 *   - Button polling and debouncing
 *   - Rotary encoder with quadrature decoding
 *   - Coordinating LCD updates via display.c and menu.c
 */

#include "shared.h"
#include "lcd.h"
#include "menu.h"
#include "display.h"
#include "buzzer.h"
#include "settings.h"
#include "encoder.h"
#include "stm32f1xx_hal.h"
#include "config.h"

// External debug output
extern void uart_puts(const char* s);

// Button beep helper - plays click if key_sound enabled
static void button_beep(void) {
    if (settings_get()->interface.key_sound) {
        buzzer_beep(BEEP_CLICK);
    }
}

// Long-press state structure (per button)
typedef struct {
    uint32_t press_start;   // Tick when button was pressed
    bool long_fired;        // Already fired long-press event?
} button_long_press_t;

/**
 * @brief Process long-press detection for a button
 *
 * Handles the complete long-press lifecycle:
 * 1. On press: Initialize timing, clear long_fired flag
 * 2. While held: Check if threshold exceeded, fire event once
 * 3. On release: Reset state (handled by caller)
 *
 * @param state Pointer to button's long-press state (maintains state between calls)
 * @param is_pressed True if button press event detected this cycle
 * @param is_held True if button is currently held down
 * @param now Current tick count (from HAL_GetTick())
 * @param threshold_ms Long-press threshold in milliseconds
 * @param long_event Event to fire on long-press detection
 * @return true if long-press was fired this cycle, false otherwise
 */
static bool process_button_long_press(
    button_long_press_t* state,
    bool is_pressed,
    bool is_held,
    uint32_t now,
    uint32_t threshold_ms,
    event_type_t long_event
) {
    // Button just pressed - start tracking
    if (is_pressed) {
        state->press_start = now;
        state->long_fired = false;
    }

    // Button held and threshold exceeded - fire once
    if (is_held && state->press_start > 0 && !state->long_fired) {
        if ((now - state->press_start) >= threshold_ms) {
            state->long_fired = true;
            button_beep();  // Feedback for long-press
            SEND_EVENT(long_event);
            return true;  // Long-press fired
        }
    }

    return false;  // No long-press this cycle
}

/*===========================================================================*/
/* Private Variables                                                          */
/*===========================================================================*/

static uint16_t prev_buttons = 0;
static volatile bool scheduler_running = false;

// Long-press detection states
#define LONG_PRESS_THRESHOLD_MS  500
static button_long_press_t f1_long_press = {0, false};
static button_long_press_t enc_long_press = {0, false};

/*===========================================================================*/
/* Button Reading                                                             */
/*===========================================================================*/

static uint16_t read_buttons(void) {
    uint16_t pb = GPIOB->IDR;
    uint16_t pc = GPIOC->IDR;
    uint16_t pd = GPIOD->IDR;
    uint16_t pa = GPIOA->IDR;

    uint16_t buttons = 0;

    // Active low buttons
    if (!(pb & (1 << 3)))  buttons |= 0x001;  // ZERO
    if (!(pb & (1 << 4)))  buttons |= 0x002;  // MENU
    if (!(pc & (1 << 10))) buttons |= 0x004;  // F1
    if (!(pc & (1 << 11))) buttons |= 0x008;  // F2
    if (!(pc & (1 << 12))) buttons |= 0x010;  // F3
    if (!(pd & (1 << 2)))  buttons |= 0x020;  // F4
    if (!(pa & (1 << 15))) buttons |= 0x040;  // Start/Stop (ON)
    if (!(pc & (1 << 15))) buttons |= 0x100;  // Encoder button

    // Active high
    if (pc & (1 << 2))     buttons |= 0x080;  // Guard open
    if (pc & (1 << 0))     buttons |= 0x200;  // Emergency stop

    // Foot pedal on PC3 (X11 connector) - active low
    if (!(pc & (1 << 3)))  buttons |= 0x400;  // Pedal pressed

    return buttons;
}

static void process_buttons(uint16_t buttons) {
    uint16_t pressed = buttons & ~prev_buttons;  // Rising edge (polled)
    uint16_t released = prev_buttons & ~buttons;  // Falling edge
    prev_buttons = buttons;

    // Check EXTI-based button events (with hardware debounce)
    bool f1_exti = encoder_f1_clicked();
    bool f2_exti = encoder_f2_clicked();
    bool f3_exti = encoder_f3_clicked();
    bool start_exti = encoder_start_clicked();
    bool menu_exti = encoder_menu_clicked();

    // Merge EXTI events into pressed mask
    if (f1_exti) pressed |= 0x004;
    if (f2_exti) pressed |= 0x008;
    if (f3_exti) pressed |= 0x010;
    if (start_exti) pressed |= 0x040;
    if (menu_exti) pressed |= 0x002;

    // Long-press detection for F1 and Encoder buttons
    uint32_t now = HAL_GetTick();

    // F1 long-press (for favorite speed cycling)
    bool f1_held = (buttons & 0x004) != 0;
    process_button_long_press(
        &f1_long_press,
        (pressed & 0x004) != 0,  // is_pressed
        f1_held,                 // is_held
        now,
        LONG_PRESS_THRESHOLD_MS,
        EVT_BTN_F1_LONG
    );

    if (released & 0x004) {
        // F1 released - reset tracking
        f1_long_press.press_start = 0;
    }

    // Encoder button long-press (for status screen)
    bool enc_held = (buttons & 0x100) != 0;
    process_button_long_press(
        &enc_long_press,
        (pressed & 0x100) != 0,  // is_pressed
        enc_held,                // is_held
        now,
        LONG_PRESS_THRESHOLD_MS,
        EVT_BTN_ENC_LONG
    );

    if (released & 0x100) {
        // Encoder button released - reset tracking
        enc_long_press.press_start = 0;
    }

    // Check if we're in menu mode
    STATE_LOCK();
    bool in_menu = g_state.menu_active;
    STATE_UNLOCK();

    // Beep on any user button press (not guard/e-stop/pedal, not F1/ENC - handled separately)
    if (pressed & 0x07B) {  // ZERO, MENU, F2-F4, START (not F1, not ENCODER)
        button_beep();
    }

    if (in_menu) {
        // Handle menu-specific button events
        if (pressed & 0x002) {
            // MENU button exits menu
            menu_exit();
            return;
        }
        if (pressed & 0x004) {
            // F1 = back in menu (only short press)
            if (!f1_long_press.long_fired) {
                menu_back();
            }
            return;
        }
        if ((released & 0x100) && !enc_long_press.long_fired) {
            // Encoder button = select/confirm (only on release, if not long)
            button_beep();
            menu_click();
            return;
        }
        // Don't send other events while in menu
        return;
    }

    // Normal mode - send events for button presses
    if (pressed & 0x001) SEND_EVENT(EVT_BTN_ZERO);
    if (pressed & 0x002) SEND_EVENT(EVT_BTN_MENU);
    if (pressed & 0x008) SEND_EVENT(EVT_BTN_F2);
    if (pressed & 0x010) SEND_EVENT(EVT_BTN_F3);
    if (pressed & 0x020) SEND_EVENT(EVT_BTN_F4);
    if (pressed & 0x040) SEND_EVENT(EVT_BTN_START);

    // F1 short press on release (if not long-pressed)
    if ((released & 0x004) && !f1_long_press.long_fired) {
        button_beep();
        SEND_EVENT(EVT_BTN_F1);
    }

    // Encoder short press on release (if not long-pressed)
    if ((released & 0x100) && !enc_long_press.long_fired) {
        button_beep();
        SEND_EVENT(EVT_BTN_ENCODER);
    }

    // Guard, E-stop, and Pedal use EXTI - read state from interrupt-updated vars
    const settings_t* settings = settings_get();

    STATE_LOCK();
    // Guard: only check if enabled in settings (default: enabled)
    if (settings && settings->sensor.guard_check_enabled) {
        g_state.guard_closed = !encoder_guard_open();
    } else {
        g_state.guard_closed = true;  // Always "closed" if disabled
    }

    // E-Stop: always checked (critical safety, no disable option)
    g_state.estop_active = encoder_estop_active();

    // Pedal: only read if enabled in settings AND not in simulation mode
    if (!g_state.sim_mode && settings && settings->sensor.pedal_enabled) {
        g_state.pedal_pressed = encoder_pedal_pressed();
    } else {
        g_state.pedal_pressed = false;  // Always "not pressed" if disabled
    }
    STATE_UNLOCK();

    // Send event on guard or E-stop state change (EXTI-detected)
    if (encoder_guard_changed()) SEND_EVENT(EVT_BTN_GUARD);
    if (encoder_estop_changed()) SEND_EVENT(EVT_BTN_ESTOP);
}

/*===========================================================================*/
/* Encoder Reading                                                            */
/*===========================================================================*/

static void process_encoder(void) {
    // Update encoder state machine (handles detent threshold internally)
    encoder_update();

    // Get detent clicks since last call
    int8_t clicks = encoder_get_delta();
    if (clicks == 0) return;

    STATE_LOCK();
    bool in_menu = g_state.menu_active;
    STATE_UNLOCK();

    if (in_menu) {
        menu_rotate(clicks);  // +1 = CW/down, -1 = CCW/up
    } else {
        // Send individual events for each click
        while (clicks > 0) {
            SEND_EVENT(EVT_ENC_CW);
            clicks--;
        }
        while (clicks < 0) {
            SEND_EVENT(EVT_ENC_CCW);
            clicks++;
        }
    }
}

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

void ui_init_buttons(void) {
    // Enable GPIO clocks
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN | RCC_APB2ENR_IOPDEN;

    // Configure button inputs with pull-ups
    // F1 (PC10), F2 (PC11), F3 (PC12), Encoder (PC15)
    GPIOC->CRH &= ~(0xF << 8);   GPIOC->CRH |= (0x8 << 8);   // PC10
    GPIOC->CRH &= ~(0xF << 12);  GPIOC->CRH |= (0x8 << 12);  // PC11
    GPIOC->CRH &= ~(0xF << 16);  GPIOC->CRH |= (0x8 << 16);  // PC12
    GPIOC->CRH &= ~(0xFFF << 20); GPIOC->CRH |= (0x888 << 20); // PC13,14,15
    GPIOC->ODR |= (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15);

    // F4 (PD2)
    GPIOD->CRL &= ~(0xF << 8);   GPIOD->CRL |= (0x8 << 8);
    GPIOD->ODR |= (1 << 2);

    // ZERO (PB3), MENU (PB4)
    GPIOB->CRL &= ~(0xF << 12);  GPIOB->CRL |= (0x8 << 12);  // PB3
    GPIOB->CRL &= ~(0xF << 16);  GPIOB->CRL |= (0x8 << 16);  // PB4
    GPIOB->ODR |= (1 << 3) | (1 << 4);

    // Start/Stop (PA15)
    GPIOA->CRH &= ~(0xFU << 28); GPIOA->CRH |= (0x8U << 28);
    GPIOA->ODR |= (1 << 15);

    // Guard (PC2), E-Stop (PC0), Foot Pedal (PC3 on X11)
    GPIOC->CRL &= ~(0xF << 0);   GPIOC->CRL |= (0x8 << 0);   // PC0 E-Stop
    GPIOC->CRL &= ~(0xF << 8);   GPIOC->CRL |= (0x8 << 8);   // PC2 Guard
    GPIOC->CRL &= ~(0xF << 12);  GPIOC->CRL |= (0x8 << 12);  // PC3 Pedal
    GPIOC->ODR |= (1 << 0) | (1 << 2) | (1 << 3);  // Pull-ups

    // Initialize encoder state machine
    encoder_init();
}

void ui_scheduler_started(void) {
    scheduler_running = true;
}

void ui_enter_menu(void) {
    menu_enter();
}

void ui_exit_menu(void) {
    menu_exit();
}

void ui_menu_rotate(int8_t delta) {
    menu_rotate(delta);
}

void ui_menu_click(void) {
    menu_click();
}

/*===========================================================================*/
/* Task Entry Point                                                           */
/*===========================================================================*/

void task_ui(void *pvParameters) {
    (void)pvParameters;

    DEBUG_PRINT("UI task started\r\n");

    TickType_t last_display_update = 0;
    const TickType_t display_interval = pdMS_TO_TICKS(UI_DISPLAY_INTERVAL_MS);

    for (;;) {
        // CRITICAL SAFETY: Update task heartbeat for watchdog monitoring
        HEARTBEAT_UPDATE_UI();

        // Read and process buttons
        uint16_t buttons = read_buttons();
        process_buttons(buttons);

        // Read encoder
        process_encoder();

        // Update display periodically
        TickType_t now = xTaskGetTickCount();
        if ((now - last_display_update) >= display_interval) {
            last_display_update = now;

            STATE_LOCK();
            bool in_menu = g_state.menu_active;
            STATE_UNLOCK();

            if (in_menu) {
                menu_draw();
            } else {
                display_update();
            }
        }

        // Poll at ~500 Hz for responsive encoder
        delay_ms(2);
    }
}
