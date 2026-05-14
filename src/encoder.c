/**
 * @file encoder.c
 * @brief UI Rotary Encoder Implementation
 *
 * Uses EXTI interrupts for PC13/PC14 quadrature decoding.
 * Encoder on PC13/PC14 with button on PC15 (polled).
 * Returns detent clicks (4 quadrature counts = 1 detent).
 */

#include "FreeRTOS.h"
#include "queue.h"
#include "encoder.h"
#include "config.h"
#include "shared.h"

/*===========================================================================*/
/* Private Variables                                                         */
/*===========================================================================*/

static volatile int16_t position = 0;      // Accumulated detent clicks
static volatile int8_t delta = 0;          // Detent clicks since last read
static volatile int8_t raw_count = 0;      // Raw quadrature counts (0-3)
static volatile uint8_t last_state = 0;
static bool last_button = false;
static bool button_event = false;

// Button EXTI flags and debounce
// F1=PC10, F2=PC11, F3=PC12, ON=PA15, MENU=PB4
static volatile bool btn_f1_event = false;
static volatile bool btn_f2_event = false;
static volatile bool btn_f3_event = false;
static volatile bool btn_start_event = false;
static volatile bool btn_menu_event = false;
static volatile uint32_t btn_f1_last = 0;
static volatile uint32_t btn_f2_last = 0;
static volatile uint32_t btn_f3_last = 0;
static volatile uint32_t btn_start_last = 0;
static volatile uint32_t btn_menu_last = 0;
#define BTN_DEBOUNCE_MS 50

// Level-sensitive inputs (E-Stop=PC0, Guard=PC2) and Pedal (PC3)
static volatile bool estop_state = false;      // true = E-Stop active
static volatile bool estop_changed = false;
static volatile bool guard_state = false;      // true = Guard open
static volatile bool guard_changed = false;
static volatile bool pedal_state = false;      // true = Pedal pressed
static volatile bool pedal_changed = false;
static volatile uint32_t pedal_last_change = 0;  // Debounce timestamp
static volatile bool pedal_raw_state = false;     // Raw (undebounced) state
#define PEDAL_DEBOUNCE_MS 20  // 20ms debounce for foot pedal

// Encoder state machine lookup table
// Index: (old_state << 2) | new_state
// Value: direction (-1, 0, +1) for each quadrature transition
static const int8_t encoder_table[16] = {
    0,  +1, -1,  0,   // 00 -> 00, 01, 10, 11
   -1,   0,  0, +1,   // 01 -> 00, 01, 10, 11
   +1,   0,  0, -1,   // 10 -> 00, 01, 10, 11
    0,  -1, +1,  0    // 11 -> 00, 01, 10, 11
};

/*===========================================================================*/
/* Private Functions                                                         */
/*===========================================================================*/

// Process encoder state change (called from ISR or polling)
static void encoder_process_state(void) {
    uint16_t pc = GPIOC->IDR;
    uint8_t a = (pc & (1 << 13)) ? 1 : 0;
    uint8_t b = (pc & (1 << 14)) ? 1 : 0;
    uint8_t new_state = (a << 1) | b;

    if (new_state != last_state) {
        uint8_t index = (last_state << 2) | new_state;
        int8_t dir = encoder_table[index];
        last_state = new_state;

        if (dir != 0) {
            raw_count += dir;

            // Check for complete detent (4 counts = 1 click)
            if (raw_count >= ENC_COUNTS_PER_DETENT) {
                raw_count = 0;
                position++;
                delta++;
            } else if (raw_count <= -ENC_COUNTS_PER_DETENT) {
                raw_count = 0;
                position--;
                delta--;
            }
        }
    }
}

/*===========================================================================*/
/* Public Functions                                                          */
/*===========================================================================*/

// Debug counter for ISR calls
static volatile uint32_t isr_count = 0;

void encoder_init(void) {
    // GPIO already configured by ui_init_buttons() in task_ui.c
    // Read initial encoder state
    uint16_t pc = GPIOC->IDR;
    uint8_t a = (pc & (1 << 13)) ? 1 : 0;
    uint8_t b = (pc & (1 << 14)) ? 1 : 0;
    last_state = (a << 1) | b;

    position = 0;
    delta = 0;
    raw_count = 0;
    isr_count = 0;

    // Enable AFIO clock for EXTI configuration
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;

    // Configure EXTI10-15 for buttons and encoder
    // AFIO_EXTICR3: EXTI8-11, AFIO_EXTICR4: EXTI12-15
    // Each uses 4 bits, GPIOA=0x00, GPIOB=0x01, GPIOC=0x02

    // EXTICR3: bits [11:8]=EXTI10, bits [15:12]=EXTI11
    AFIO->EXTICR[2] &= ~((0xFU << 8) | (0xFU << 12));
    AFIO->EXTICR[2] |= (0x02U << 8) | (0x02U << 12);  // EXTI10,11 = GPIOC (F1,F2)

    // EXTICR4: bits [3:0]=EXTI12, [7:4]=EXTI13, [11:8]=EXTI14, [15:12]=EXTI15
    AFIO->EXTICR[3] &= ~0xFFFFU;  // Clear all
    AFIO->EXTICR[3] |= (0x02U << 0);   // EXTI12 = GPIOC (F3)
    AFIO->EXTICR[3] |= (0x02U << 4);   // EXTI13 = GPIOC (Encoder A)
    AFIO->EXTICR[3] |= (0x02U << 8);   // EXTI14 = GPIOC (Encoder B)
    AFIO->EXTICR[3] |= (0x00U << 12);  // EXTI15 = GPIOA (ON/Start = PA15)

    // Encoder (PC13/14): both edges for quadrature
    EXTI->RTSR |= (1 << 13) | (1 << 14);
    EXTI->FTSR |= (1 << 13) | (1 << 14);

    // Buttons: falling edge only (active low, press = falling)
    // PC10(F1), PC11(F2), PC12(F3), PA15(ON/Start)
    EXTI->FTSR |= (1 << 10) | (1 << 11) | (1 << 12) | (1 << 15);

    // Clear pending and enable interrupts for lines 10-15
    EXTI->PR = (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15);
    EXTI->IMR |= (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15);

    // Priority 6 = lower than FreeRTOS max syscall (5)
    NVIC_SetPriority(EXTI15_10_IRQn, 6);
    NVIC_EnableIRQ(EXTI15_10_IRQn);

    // Configure EXTI4 for MENU button (PB4)
    // EXTICR2: bits [3:0]=EXTI4, GPIOB=0x01
    AFIO->EXTICR[1] &= ~(0xFU << 0);
    AFIO->EXTICR[1] |= (0x01U << 0);  // EXTI4 = GPIOB

    EXTI->FTSR |= (1 << 4);           // Falling edge (active low)
    EXTI->PR = (1 << 4);              // Clear pending
    EXTI->IMR |= (1 << 4);            // Enable interrupt

    NVIC_SetPriority(EXTI4_IRQn, 6);
    NVIC_EnableIRQ(EXTI4_IRQn);

    // Configure EXTI0 for E-Stop (PC0) - both edges, level-sensitive
    // EXTICR1: bits [3:0]=EXTI0, GPIOC=0x02
    AFIO->EXTICR[0] &= ~(0xFU << 0);
    AFIO->EXTICR[0] |= (0x02U << 0);  // EXTI0 = GPIOC
    EXTI->RTSR |= (1 << 0);           // Rising edge (released)
    EXTI->FTSR |= (1 << 0);           // Falling edge (pressed)
    EXTI->PR = (1 << 0);
    EXTI->IMR |= (1 << 0);
    // Read initial state (PC0 active high)
    estop_state = (GPIOC->IDR & (1 << 0)) != 0;
    NVIC_SetPriority(EXTI0_IRQn, 6);
    NVIC_EnableIRQ(EXTI0_IRQn);

    // Configure EXTI2 for Guard (PC2) - both edges, level-sensitive
    // EXTICR1: bits [11:8]=EXTI2, GPIOC=0x02
    AFIO->EXTICR[0] &= ~(0xFU << 8);
    AFIO->EXTICR[0] |= (0x02U << 8);  // EXTI2 = GPIOC
    EXTI->RTSR |= (1 << 2);           // Rising edge (opened)
    EXTI->FTSR |= (1 << 2);           // Falling edge (closed)
    EXTI->PR = (1 << 2);
    EXTI->IMR |= (1 << 2);
    // Read initial state (PC2 active high = guard open)
    guard_state = (GPIOC->IDR & (1 << 2)) != 0;
    NVIC_SetPriority(EXTI2_IRQn, 6);
    NVIC_EnableIRQ(EXTI2_IRQn);

    // Configure EXTI3 for Foot Pedal (PC3) - both edges
    // EXTICR1: bits [15:12]=EXTI3, GPIOC=0x02
    AFIO->EXTICR[0] &= ~(0xFU << 12);
    AFIO->EXTICR[0] |= (0x02U << 12); // EXTI3 = GPIOC
    EXTI->RTSR |= (1 << 3);           // Rising edge (released)
    EXTI->FTSR |= (1 << 3);           // Falling edge (pressed)
    EXTI->PR = (1 << 3);
    EXTI->IMR |= (1 << 3);
    // Read initial state (PC3 active high for NC wiring)
    pedal_state = (GPIOC->IDR & (1 << 3)) != 0;
    NVIC_SetPriority(EXTI3_IRQn, 6);
    NVIC_EnableIRQ(EXTI3_IRQn);
}

// Get ISR count for debugging
uint32_t encoder_get_isr_count(void) {
    return isr_count;
}

void encoder_update(void) {
    // Quadrature handled by ISR, just poll button here
    uint16_t pc = GPIOC->IDR;
    bool btn = !(pc & (1 << 15));  // PC15 active low
    if (btn && !last_button) {
        button_event = true;
    }
    last_button = btn;
}

int16_t encoder_get_position(void) {
    return position;
}

int8_t encoder_get_delta(void) {
    int8_t d = delta;
    delta = 0;
    return d;
}

void encoder_reset(void) {
    position = 0;
    delta = 0;
}

bool encoder_button_pressed(void) {
    return !(GPIOC->IDR & (1 << 15));  // PC15 active low
}

bool encoder_button_clicked(void) {
    if (button_event) {
        button_event = false;
        return true;
    }
    return false;
}

/*===========================================================================*/
/* Interrupt Handler                                                         */
/*===========================================================================*/

void EXTI15_10_IRQHandler(void) {
    uint32_t pending = EXTI->PR;
    uint32_t now = HAL_GetTick();

    // Clear all pending bits we handle (write 1 to clear)
    EXTI->PR = pending & ((1 << 10) | (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) | (1 << 15));

    // Encoder (PC13/PC14)
    if (pending & ((1 << 13) | (1 << 14))) {
        isr_count++;
        encoder_process_state();
    }

    // F1 button (PC10) with debounce
    if ((pending & (1 << 10)) && (now - btn_f1_last >= BTN_DEBOUNCE_MS)) {
        btn_f1_last = now;
        btn_f1_event = true;
    }

    // F2 button (PC11) with debounce
    if ((pending & (1 << 11)) && (now - btn_f2_last >= BTN_DEBOUNCE_MS)) {
        btn_f2_last = now;
        btn_f2_event = true;
    }

    // F3 button (PC12) with debounce
    if ((pending & (1 << 12)) && (now - btn_f3_last >= BTN_DEBOUNCE_MS)) {
        btn_f3_last = now;
        btn_f3_event = true;
    }

    // ON/Start button (PA15) with debounce
    if ((pending & (1 << 15)) && (now - btn_start_last >= BTN_DEBOUNCE_MS)) {
        btn_start_last = now;
        btn_start_event = true;
    }
}

// Check and clear button events (called from task_ui)
bool encoder_f1_clicked(void) {
    if (btn_f1_event) {
        btn_f1_event = false;
        return true;
    }
    return false;
}

bool encoder_f2_clicked(void) {
    if (btn_f2_event) {
        btn_f2_event = false;
        return true;
    }
    return false;
}

bool encoder_f3_clicked(void) {
    if (btn_f3_event) {
        btn_f3_event = false;
        return true;
    }
    return false;
}

bool encoder_start_clicked(void) {
    if (btn_start_event) {
        btn_start_event = false;
        return true;
    }
    return false;
}

bool encoder_menu_clicked(void) {
    if (btn_menu_event) {
        btn_menu_event = false;
        return true;
    }
    return false;
}

// EXTI4 handler for MENU button (PB4)
void EXTI4_IRQHandler(void) {
    uint32_t now = HAL_GetTick();

    // Clear pending bit
    EXTI->PR = (1 << 4);

    // MENU button with debounce
    if (now - btn_menu_last >= BTN_DEBOUNCE_MS) {
        btn_menu_last = now;
        btn_menu_event = true;
    }
}

// EXTI0 handler for E-Stop (PC0) - level-sensitive
// CRITICAL SAFETY: Immediately cut motor power on E-Stop activation
void EXTI0_IRQHandler(void) {
    EXTI->PR = (1 << 0);  // Clear pending

    // Read current state (active high = E-Stop engaged)
    estop_state = (GPIOC->IDR & (1 << 0)) != 0;

    // CRITICAL: Hardware motor cutoff - set MOTOR_ENABLE pin LOW immediately
    // This provides hardware-level safety independent of UART communication
    if (estop_state) {
        // E-Stop engaged - disable motor NOW (active HIGH enable, so set LOW)
        GPIOD->BSRR = (1 << (4 + 16));  // BR4 = reset PD4
    }
    // Note: Motor enable will be restored by main task after E-Stop cleared

    estop_changed = true;

    // Send high-priority event to main task for state update
    event_type_t evt = EVT_BTN_ESTOP;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    extern QueueHandle_t g_event_queue;
    xQueueSendFromISR(g_event_queue, &evt, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// EXTI2 handler for Guard (PC2) - level-sensitive
// SAFETY: Immediately stop motor if guard opened
void EXTI2_IRQHandler(void) {
    EXTI->PR = (1 << 2);  // Clear pending

    // Read current state (active high = guard open)
    guard_state = (GPIOC->IDR & (1 << 2)) != 0;

    // SAFETY: If guard opened, disable motor immediately
    if (guard_state) {
        // Guard opened - disable motor hardware NOW
        GPIOD->BSRR = (1 << (4 + 16));  // BR4 = reset PD4 (motor enable)
    }

    guard_changed = true;

    // Send event to main task for state update
    event_type_t evt = EVT_BTN_GUARD;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    extern QueueHandle_t g_event_queue;
    xQueueSendFromISR(g_event_queue, &evt, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// EXTI3 handler for Foot Pedal (PC3) with 20ms debounce
void EXTI3_IRQHandler(void) {
    EXTI->PR = (1 << 3);  // Clear pending

    // Read pin state (active high for NC wiring)
    bool new_state = (GPIOC->IDR & (1 << 3)) != 0;

    // Update immediately (no debounce - foot pedals are slow)
    pedal_state = new_state;
    pedal_changed = (new_state != pedal_raw_state);
    pedal_raw_state = new_state;
}

// Accessor functions for level-sensitive inputs
bool encoder_estop_active(void) {
    return estop_state;
}

bool encoder_estop_changed(void) {
    if (estop_changed) {
        estop_changed = false;
        return true;
    }
    return false;
}

bool encoder_guard_open(void) {
    return guard_state;
}

bool encoder_guard_changed(void) {
    if (guard_changed) {
        guard_changed = false;
        return true;
    }
    return false;
}

bool encoder_pedal_pressed(void) {
    return pedal_state;
}

bool encoder_pedal_changed(void) {
    if (pedal_changed) {
        pedal_changed = false;
        return true;
    }
    return false;
}
