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

    /* Belt and braces for the stale-press_start hazard above: a button that is
     * not physically down cannot be mid-press, whatever edges were seen or
     * missed. long_fired is deliberately NOT cleared here — the caller still
     * has to consult it this cycle to suppress the short-press event, and the
     * next press clears it. */
    if (!is_held) {
        state->press_start = 0;
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

/* Consecutive identical polls required before a polled button changes state.
 * The task polls every 2 ms, so this is ~10 ms — the same order as the
 * BTN_DEBOUNCE_MS the EXTI buttons use. */
#define BTN_STABLE_POLLS 5

/* Raw (undebounced) button levels from the most recent read_buttons().
 *
 * REVIEW FIX (HIGH): the long-press detector takes `is_pressed` from the EXTI
 * one-shot — which the ISR sets on the edge itself, since BTN_DEBOUNCE_MS there
 * is a re-trigger lockout and not a settling delay — and `is_held` from the
 * debounced word, which needs BTN_STABLE_POLLS identical 2 ms polls. Once the
 * debounce was added the two could never be true in the same call: the press
 * poll set press_start and the `!is_held` line wiped it in the next statement,
 * and because the EXTI event is a consumed one-shot it was never re-armed. F1
 * long-press (store the dialled RPM into a favourite slot) was dead in every
 * build, and on release the short-press path fired instead, so holding F1
 * CYCLED favourites rather than storing one.
 *
 * The debounce is there to clean up EDGES. Whether a button is still down is a
 * level, so the hold test reads the raw pins. */
static uint16_t s_raw_buttons = 0;

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

    /* REVIEW FIX (MEDIUM): the guard (0x080), E-Stop (0x200) and pedal (0x400)
     * level bits used to be sampled here. Nothing consumed any of them — the
     * authoritative state comes from encoder.c's EXTI handlers and its
     * encoder_estop_active()/encoder_guard_open()/pedal API — and the pedal bit
     * disagreed with encoder.c about polarity anyway: this read PC3 LOW as
     * pressed while encoder.c uses HIGH ("active high for NC wiring").
     *
     * They were actively harmful once the debounce below arrived: the stability
     * test compares the WHOLE word, so a chattering or floating pedal input
     * pinned stable_polls at zero and froze `debounced` — suppressing the
     * encoder press/release and F1 release edges the menu depends on. A level
     * input that changes on its own must not gate a button debounce. */

    /* REVIEW FIX (HIGH): these are read straight off the pins at the 500 Hz
     * task rate with no debounce whatsoever, while every EXTI-driven button
     * gets BTN_DEBOUNCE_MS in the ISR. The encoder push-button (0x100) is the
     * worst of them: process_buttons() acts on its RELEASE edge to confirm a
     * menu selection, so a single bouncy press produced two edges and the menu
     * entered an edit and immediately confirmed it — or toggled fine/coarse
     * twice, back to where it started. Require the whole word to hold still
     * before it counts.
     *
     * The guard and E-Stop bits ride along in this word; delaying them ~10 ms
     * costs nothing, because the hardware cutoff is done in the EXTI ISRs and
     * this copy only feeds the UI. */
    static uint16_t debounced = 0;
    static uint16_t last_raw = 0;
    static uint8_t  stable_polls = 0;

    /* The undebounced sample. "Is the button still down?" is a LEVEL question
     * and must not wait for the debounce — see s_raw_buttons below. */
    s_raw_buttons = buttons;

    if (buttons != last_raw) {
        last_raw = buttons;
        stable_polls = 0;
    } else if (stable_polls < BTN_STABLE_POLLS) {
        stable_polls++;
    }
    if (stable_polls >= BTN_STABLE_POLLS) {
        debounced = buttons;
    }
    return debounced;
}

static void process_buttons(uint16_t buttons) {
    /* REVIEW FIX (HIGH): this block used to sit at the END of the function,
     * after the in-menu branch's unconditional `return` — so with the menu open
     * it never ran. That is the one place it matters: SEND_EVENT_ISR() drops
     * silently on a full queue, console START is not blocked by menu_active, and
     * a single dropped EVT_BTN_ESTOP means handle_btn_estop() never runs and the
     * motor queue is never purged. The hardware cutoff still holds, but the
     * software recovery waited for the operator to close the menu. It depends on
     * nothing computed below, so it belongs first.
     */
    // AUDIT FIX (HIGH, encoder.c:343): E-Stop and guard events used to be
    // one-shot — encoder_*_changed() cleared its flag on read, SEND_EVENT
    // dropped silently on queue-full. A single drop meant handle_btn_estop
    // never ran, the motor command queue was never purged, and the tapping
    // task (which doesn't check estop_active mid-cycle) kept the spindle
    // running under an engaged E-Stop. Fix: track the last state the queue
    // *accepted* and re-send until it does. The change flag is now just a
    // latency hint; the level state is authoritative.
    {
        static bool last_estop_sent = false;
        static bool last_guard_sent = true;   // guard_closed = !guard_open, starts closed
        static bool estop_init = false;
        static bool guard_init = false;

        bool estop_now = encoder_estop_active();
        bool guard_closed_now = !encoder_guard_open();

        if (!estop_init || estop_now != last_estop_sent) {
            event_type_t e = EVT_BTN_ESTOP;
            if (xQueueSend(g_event_queue, &e, 0) == pdTRUE) {
                last_estop_sent = estop_now;
                estop_init = true;
            }
            (void)encoder_estop_changed();  // consume optimizer flag once accepted
        }
        if (!guard_init || guard_closed_now != last_guard_sent) {
            event_type_t e = EVT_BTN_GUARD;
            if (xQueueSend(g_event_queue, &e, 0) == pdTRUE) {
                last_guard_sent = guard_closed_now;
                guard_init = true;
            }
            (void)encoder_guard_changed();
        }
    }

    // AUDIT FIX (CRITICAL, task_ui.c:125): the polled edge detector merged
    // with the EXTI-debounced path caused every press of F1/F2/F3/MENU/START
    // to fire TWICE — the EXTI event and the undebounced polled rising-edge
    // could arrive in the same or adjacent 2 ms cycles. For START (a
    // start/stop toggle) that meant one physical press stopped the motor
    // and then immediately restarted it. Fix: mask the EXTI-managed buttons
    // out of the polled edge detector; they now come exclusively from EXTI
    // (with pin-level requalification at read time — see encoder.c).
    const uint16_t EXTI_MANAGED = 0x002 | 0x004 | 0x008 | 0x010 | 0x040;
    uint16_t pressed  = (buttons & ~prev_buttons) & ~EXTI_MANAGED;
    /* AUDIT FIX (HIGH, task_ui.c:135): `released` used to be masked with
     * ~EXTI_MANAGED too. That mask exists to stop PRESSES double-firing (the
     * EXTI event plus the polled rising edge), but EXTI reports clicks only —
     * it never reports a release — so the polled falling edge is the ONLY
     * source of one. Masking F1 (0x004) out of it made line 253's
     * `if ((released & 0x004) && !long_fired) SEND_EVENT(EVT_BTN_F1)` dead
     * code: F1 never cycled favourite speeds outside the menu. It also meant
     * f1_long_press.press_start was never reset on release, so a later press
     * whose EXTI event was filtered out (50 ms debounce, or the pin-level
     * requalification in encoder_f1_clicked()) saw a stale press_start already
     * older than the 500 ms threshold and fired EVT_BTN_F1_LONG — overwriting
     * a favourite slot on what the operator felt as a plain tap.
     *
     * Only F1 (0x004) and the encoder button (0x100, not EXTI-managed) act on
     * releases, so unmasking cannot reintroduce the double-press it was added
     * for. */
    uint16_t released = (prev_buttons & ~buttons);
    prev_buttons = buttons;

    // Check EXTI-based button events (with hardware debounce + pin-level filter)
    bool f1_exti = encoder_f1_clicked();
    bool f2_exti = encoder_f2_clicked();
    bool f3_exti = encoder_f3_clicked();
    bool start_exti = encoder_start_clicked();
    bool menu_exti = encoder_menu_clicked();

    // Merge EXTI events into pressed mask (sole source for these buttons)
    if (f1_exti) pressed |= 0x004;
    if (f2_exti) pressed |= 0x008;
    if (f3_exti) pressed |= 0x010;
    if (start_exti) pressed |= 0x040;
    if (menu_exti) pressed |= 0x002;

    // Long-press detection for F1 and Encoder buttons
    uint32_t now = HAL_GetTick();

    // F1 long-press (for favorite speed cycling)
    bool f1_held = (s_raw_buttons & 0x004) != 0;   // level, not debounced
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
    bool enc_held = (s_raw_buttons & 0x100) != 0;  // level, not debounced
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

    // AUDIT FIX (LOW, task_ui.c:209): refresh guard/estop/pedal in g_state
    // BEFORE the in-menu early return, so an E-Stop engaged while the menu
    // is open still updates g_state.estop_active/guard_closed for the rest
    // of the code (display, handlers) even though menu_draw won't show it.
    // Physical safety already handled by the EXTI ISR; this fixes the
    // stale-indicator defect.
    {
        const settings_t* s_refresh = settings_get();
        STATE_LOCK();
        if (s_refresh && s_refresh->sensor.guard_check_enabled) {
            g_state.guard_closed = !encoder_guard_open();
        } else {
            g_state.guard_closed = true;
        }
        g_state.estop_active = encoder_estop_active();
        /* Hardware owns the pedal only when NOT simulating. The else branch
         * used to clear pedal_pressed unconditionally, which included the
         * sim_mode case — so TAPSIM P set the flag and this cleared it on the
         * very next UI refresh, milliseconds later. The simulated pedal could
         * never stay pressed long enough for task_tapping to see it, which
         * made TAPSIM's pedal option useless for testing any pedal behaviour.
         * In sim mode TAPSIM owns the flag; leave it alone. */
        if (!g_state.sim_mode) {
            g_state.pedal_pressed = (s_refresh && s_refresh->sensor.pedal_enabled)
                                    ? encoder_pedal_pressed()
                                    : false;
        }
        STATE_UNLOCK();
    }

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

    // Guard/E-stop/pedal state refresh moved to top of process_buttons()
    // — AUDIT FIX (LOW, task_ui.c:209) so g_state stays fresh even in menu.

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

/* Console menu input is DEFERRED to this task.
 *
 * REVIEW FIX (CRITICAL): these four wrappers used to call straight into menu.c
 * from whichever task invoked them — and the console path (events.c,
 * commands_ui.c) runs in task_main while the front panel runs here. So
 * menu_index/menu_editing/menu_submenu had two concurrent mutators and no lock;
 * menu_busy_flag only suppressed drawing. A preemption between
 * `menu_submenu = item->submenu_id` and `menu_index = 0` in menu_click_impl()
 * left a stale index against a shorter submenu, and menu_click_impl() indexed
 * the item table with no bounds check at all — in the editing branch that is
 * `*item->value = menu_edit_value`, a write through a pointer read from past
 * the end of the array.
 *
 * A mutex was the wrong answer: a menu action can hold the CPU for seconds
 * (EEPROM save, 2 s confirmation screens) and task_ui's heartbeat would go
 * stale. Single-task ownership costs nothing instead — the console posts a
 * request, task_ui applies it in order on its next 2 ms tick, and the whole
 * class of race is gone rather than serialised. It also removes the need for
 * menu_is_busy(), since the action and the repaint are now the same task.
 *
 * The ring drops requests when full rather than blocking; sixteen deep is far
 * more than the console can produce between two 2 ms drains. */
typedef struct {
    uint8_t op;
    int8_t  arg;
} menu_req_t;

enum { MENU_OP_ENTER = 1, MENU_OP_EXIT, MENU_OP_CLICK, MENU_OP_ROTATE };

#define MENU_REQ_QUEUE_LEN 16u
static menu_req_t s_menu_req[MENU_REQ_QUEUE_LEN];
static volatile uint8_t s_menu_req_head = 0;
static volatile uint8_t s_menu_req_tail = 0;

static void menu_req_post(uint8_t op, int8_t arg) {
    taskENTER_CRITICAL();
    const uint8_t next = (uint8_t)((s_menu_req_head + 1u) % MENU_REQ_QUEUE_LEN);
    if (next != s_menu_req_tail) {
        s_menu_req[s_menu_req_head].op = op;
        s_menu_req[s_menu_req_head].arg = arg;
        s_menu_req_head = next;
    }
    taskEXIT_CRITICAL();
}

/* Called from this task only, before the frame is drawn. */
static void menu_req_drain(void) {
    for (;;) {
        menu_req_t r;
        taskENTER_CRITICAL();
        if (s_menu_req_tail == s_menu_req_head) {
            taskEXIT_CRITICAL();
            return;
        }
        r = s_menu_req[s_menu_req_tail];
        s_menu_req_tail = (uint8_t)((s_menu_req_tail + 1u) % MENU_REQ_QUEUE_LEN);
        taskEXIT_CRITICAL();

        switch (r.op) {
            case MENU_OP_ENTER:  menu_enter();        break;
            case MENU_OP_EXIT:   menu_exit();         break;
            case MENU_OP_CLICK:  menu_click();        break;
            case MENU_OP_ROTATE: menu_rotate(r.arg);  break;
            default:                                  break;
        }
    }
}

void ui_enter_menu(void) {
    menu_req_post(MENU_OP_ENTER, 0);
}

void ui_exit_menu(void) {
    menu_req_post(MENU_OP_EXIT, 0);
}

void ui_menu_rotate(int8_t delta) {
    menu_req_post(MENU_OP_ROTATE, delta);
}

void ui_menu_click(void) {
    menu_req_post(MENU_OP_CLICK, 0);
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

        /* Apply any console menu input before this iteration can draw. */
        menu_req_drain();

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
