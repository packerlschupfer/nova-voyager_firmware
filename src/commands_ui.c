/**
 * @file commands_ui.c
 * @brief Menu and UI-related commands
 */

#include "commands_internal.h"
#include "display.h"
#include "encoder.h"
#include "diagnostics.h"  // Phase 7: Queue overflow tracking
#include "buzzer.h"       // For BEEP command

// From display.c
extern void ui_enter_menu(void);
extern void ui_exit_menu(void);

/*===========================================================================*/
/* Command Handlers                                                          */
/*===========================================================================*/

void cmd_menu(void) {
    STATE_LOCK();
    bool in_menu = g_state.menu_active;
    bool motor_on = g_state.motor_running;
    if (!in_menu && !motor_on) {
        g_state.menu_active = true;
        g_state.state = APP_STATE_MENU;
    }
    STATE_UNLOCK();
    if (in_menu) {
        uart_puts("Exiting menu...\r\n");
        ui_exit_menu();
    } else if (motor_on) {
        uart_puts("Cannot enter menu while motor running\r\n");
    } else {
        uart_puts("Entering menu...\r\n");
        ui_enter_menu();
    }
}

// Phase 1.2: Added queue overflow handling
void cmd_up(void) {
    if (xQueueSend(g_event_queue, &(uint32_t){EVT_ENC_CCW}, 0) != pdPASS) {
        uart_puts("WARN: Event queue full (UP)\r\n");
        diagnostics_queue_overflow(false);  // Phase 7: Track event queue overflow
        diagnostics_queue_overflow(false);  // Phase 7: Track event queue overflow
    } else {
        uart_puts("UP\r\n");
    }
}

void cmd_dn(void) {
    if (xQueueSend(g_event_queue, &(uint32_t){EVT_ENC_CW}, 0) != pdPASS) {
        uart_puts("WARN: Event queue full (DN)\r\n");
        diagnostics_queue_overflow(false);  // Phase 7: Track event queue overflow
    } else {
        uart_puts("DN\r\n");
    }
}

void cmd_ok(void) {
    if (xQueueSend(g_event_queue, &(uint32_t){EVT_BTN_ENCODER}, 0) != pdPASS) {
        uart_puts("WARN: Event queue full (OK)\r\n");
        diagnostics_queue_overflow(false);  // Phase 7: Track event queue overflow
    } else {
        uart_puts("OK\r\n");
    }
}

void cmd_f1(void) {
    if (xQueueSend(g_event_queue, &(uint32_t){EVT_BTN_F1}, 0) != pdPASS) {
        uart_puts("WARN: Event queue full (F1)\r\n");
        diagnostics_queue_overflow(false);  // Phase 7: Track event queue overflow
    } else {
        uart_puts("F1\r\n");
    }
}

void cmd_f2(void) {
    if (xQueueSend(g_event_queue, &(uint32_t){EVT_BTN_F2}, 0) != pdPASS) {
        uart_puts("WARN: Event queue full (F2)\r\n");
        diagnostics_queue_overflow(false);  // Phase 7: Track event queue overflow
    } else {
        uart_puts("F2\r\n");
    }
}

void cmd_f3(void) {
    if (xQueueSend(g_event_queue, &(uint32_t){EVT_BTN_F3}, 0) != pdPASS) {
        uart_puts("WARN: Event queue full (F3)\r\n");
        diagnostics_queue_overflow(false);  // Phase 7: Track event queue overflow
    } else {
        uart_puts("F3\r\n");
    }
}

void cmd_f4(void) {
    if (xQueueSend(g_event_queue, &(uint32_t){EVT_BTN_F4}, 0) != pdPASS) {
        uart_puts("WARN: Event queue full (F4)\r\n");
        diagnostics_queue_overflow(false);  // Phase 7: Track event queue overflow
    } else {
        uart_puts("F4\r\n");
    }
}

#ifdef BUILD_DEBUG
void cmd_enc(void) {
    uint32_t isr = encoder_get_isr_count();
    int16_t pos = encoder_get_position();
    int8_t d = encoder_get_delta();
    uart_puts("ISR: ");
    print_num(isr);
    uart_puts(" pos: ");
    print_num(pos);
    uart_puts(" delta: ");
    print_num(d);
    uart_puts("\r\nEXTI_PR: ");
    print_num(EXTI->PR);
    uart_puts(" IMR: ");
    print_num(EXTI->IMR);
    uart_puts(" RTSR: ");
    print_num(EXTI->RTSR);
    uart_puts(" FTSR: ");
    print_num(EXTI->FTSR);
    uart_puts("\r\nAFIO_EXTICR4: ");
    print_num(AFIO->EXTICR[3]);
    uart_puts(" PC_IDR: ");
    print_num(GPIOC->IDR);
    uart_puts(" NVIC_ISER1: ");
    print_num(NVIC->ISER[1]);
    uart_puts("\r\n");
}
#endif

// Audio feedback commands
void cmd_beep(void) {
    uart_puts("Beep!\r\n");
    buzzer_beep(BEEP_SUCCESS);
}

void cmd_buzz(void) {
    uart_puts("Buzzer test PA8...\r\n");
    // Direct GPIO toggle for buzzer testing
    uint32_t saved = GPIOA->CRH;
    GPIOA->CRH &= ~(0xF << 0);
    GPIOA->CRH |= (0x3 << 0);
    for (int n = 0; n < 3; n++) {
        for (int beep = 0; beep < 2000; beep++) {
            GPIOA->BSRR = (1 << 8);
            for (volatile int i = 0; i < 50; i++);
            GPIOA->BRR = (1 << 8);
            for (volatile int i = 0; i < 50; i++);
        }
        for (volatile int i = 0; i < 500000; i++);
    }
    GPIOA->CRH = saved;
    uart_puts("Done\r\n");
}
