/**
 * @file commands_ui.c
 * @brief Menu and UI-related commands
 */

#include "commands_internal.h"
#include "lcd.h"
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
        // AUDIT FIX (LOW, commands_ui.c:45): duplicate call double-counted
        // event-queue overflows in the STATS report. Every sibling handler
        // (cmd_dn/cmd_ok/cmd_f1..f4) calls it once.
        diagnostics_queue_overflow(false);
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

/**
 * @brief Dump the LCD text shadow — what the row-painting path last wrote.
 *
 * The point of this is automated testing: with MENU/UP/DN/OK able to drive the
 * UI from the console, this is the missing half — being able to READ the
 * result and assert on it instead of asking the operator what the panel says.
 *
 * See the caveat on lcd_shadow_snapshot(): lcd_print_at() bypasses the shadow, so a
 * full-screen flow can leave rows stale. Rows are printed inside | | so
 * trailing spaces are visible, because column alignment is usually the thing
 * under test.
 */
/* The transient error/warning banner is drawn through lcd_print_at(), which
 * bypasses the dirty-row shadow and invalidates it — so the shadow dump above
 * structurally CANNOT show it, and prints "shadow invalid" for exactly the
 * screens one most wants to inspect. That is how the clutch-slip ALERT banner
 * ended up verified only by its console line.
 *
 * The banner's source of truth is observable even when the shadow is not:
 * g_state.error_line1/2 and error_until are what display.c renders from. Report
 * those directly, so "is the banner up, and what does it say" is answerable. */
static void lcd_report_error_banner(void) {
    STATE_LOCK();
    const uint32_t until = g_state.error_until;
    const char* l1 = g_state.error_line1;
    const char* l2 = g_state.error_line2;
    STATE_UNLOCK();

    const uint32_t now = HAL_GetTick();
    uart_puts("Banner: ");
    /* Signed difference so a tick wrap reads as expired, not as ~49 days left. */
    if (until != 0 && (int32_t)(until - now) > 0) {
        uart_puts("ACTIVE, ");
        print_num((int32_t)(until - now));
        uart_puts(" ms left\r\n");
        uart_puts("  1 |"); uart_puts(l1 ? l1 : "(null)"); uart_puts("|\r\n");
        uart_puts("  2 |"); uart_puts(l2 ? l2 : "(null)"); uart_puts("|\r\n");
    } else {
        uart_puts("none\r\n");
    }
}

void cmd_lcd(void) {
    char snap[LCD_ROWS][LCD_COLS];
    const bool have = lcd_shadow_snapshot(snap);

    uart_puts("--- LCD text shadow ---\r\n");
    for (uint8_t row = 0; row < LCD_ROWS; row++) {
        const char* r = have ? snap[row] : NULL;
        if (!r) {
            /* break, NOT return: an invalid shadow is exactly the case where
             * the banner report matters, because a full-screen flow painting
             * over the rows is what invalidates it. Returning here skipped the
             * banner in the one situation the report was added for. */
            uart_puts("  (shadow invalid - a full-screen flow has painted over it)\r\n");
            break;
        }
        uart_putc('0' + row);
        uart_puts(" |");
        for (uint8_t c = 0; c < LCD_COLS; c++) {
            char ch = r[c];
            /* CGRAM codes and the 2-byte symbol pairs are not printable ASCII;
             * show them as '.' rather than emitting control bytes into the
             * operator's terminal. */
            uart_putc((ch >= 0x20 && ch < 0x7F) ? ch : '.');
        }
        uart_puts("|\r\n");
    }
    lcd_report_error_banner();
}
