/**
 * @file serial_console.c
 * @brief Serial console and UART communication implementation
 */

#include "serial_console.h"
#include "commands.h"
#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdint.h>
#include <stdbool.h>

/* Command table is defined in commands.c */
extern const cmd_entry_t cmd_table[];
extern SemaphoreHandle_t g_uart_mutex;  // Protects UART output

/*===========================================================================*/
/* UART with interrupt-driven receive ring buffer                            */
/*===========================================================================*/

#define UART_RX_BUF_SIZE 64
static volatile uint8_t uart_rx_buf[UART_RX_BUF_SIZE];
static volatile uint8_t uart_rx_head = 0;  // Write position (ISR)
static volatile uint8_t uart_rx_tail = 0;  // Read position (main)

// Phase 1.1: Added timeout protection to prevent infinite loops
void uart_putc(char c) {
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(100);  // 100ms timeout

    while (!(USART1->SR & USART_SR_TXE)) {
        // Check if scheduler is running before using tick count
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
            if ((xTaskGetTickCount() - start) >= timeout_ticks) {
                return;  // Timeout - drop character rather than hang
            }
        }
    }
    USART1->DR = c;
}

void uart_puts(const char* s) {
    // Protect console output with mutex to prevent task interleaving
    if (g_uart_mutex && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreTake(g_uart_mutex, portMAX_DELAY);
        while (*s) uart_putc(*s++);
        xSemaphoreGive(g_uart_mutex);
    } else {
        // Before scheduler starts or if mutex not created, no protection needed
        while (*s) uart_putc(*s++);
    }
}

int uart_getc_nonblocking(void) {
    if (uart_rx_head == uart_rx_tail) {
        return -1;  // Empty
    }
    uint8_t c = uart_rx_buf[uart_rx_tail];
    uart_rx_tail = (uart_rx_tail + 1) % UART_RX_BUF_SIZE;
    return c;
}

// USART1 interrupt handler - called on each received byte
void USART1_IRQHandler(void) {
    if (USART1->SR & USART_SR_RXNE) {
        uint8_t c = USART1->DR & 0xFF;
        uint8_t next = (uart_rx_head + 1) % UART_RX_BUF_SIZE;
        if (next != uart_rx_tail) {  // Not full
            uart_rx_buf[uart_rx_head] = c;
            uart_rx_head = next;
        }
    }
    // Clear overrun error if set
    if (USART1->SR & USART_SR_ORE) {
        (void)USART1->DR;
    }
}

/*===========================================================================*/
/* Motor UART Direct Access (for console debugging)                          */
/*===========================================================================*/

#define MOTOR_USART USART3

// Phase 1.1: Added timeout protection
void motor_putc(uint8_t c) {
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(100);  // 100ms timeout

    while (!(MOTOR_USART->SR & USART_SR_TXE)) {
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
            if ((xTaskGetTickCount() - start) >= timeout_ticks) {
                return;  // Timeout - drop character
            }
        }
    }
    MOTOR_USART->DR = c;
}

int motor_getc_timeout(uint32_t timeout_loops) {
    while (timeout_loops--) {
        if (MOTOR_USART->SR & USART_SR_RXNE) {
            return MOTOR_USART->DR & 0xFF;
        }
    }
    return -1;
}

int motor_read_resp(uint8_t* buf, int max_len) {
    int len = 0;
    int consecutive_timeouts = 0;
    while (len < max_len && consecutive_timeouts < 3) {
        int c = motor_getc_timeout(100000);  // Longer timeout
        if (c < 0) {
            consecutive_timeouts++;
        } else {
            consecutive_timeouts = 0;
            buf[len++] = c;
        }
    }
    return len;
}

/*===========================================================================*/
/* Utility Functions                                                          */
/*===========================================================================*/

void print_hex_byte(uint8_t b) {
    uart_putc("0123456789ABCDEF"[b >> 4]);
    uart_putc("0123456789ABCDEF"[b & 0xF]);
}

void print_num(int32_t n) {
    char buf[12];
    int i = 0;
    if (n < 0) { uart_putc('-'); n = -n; }
    if (n == 0) { uart_putc('0'); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) uart_putc(buf[--i]);
}

/*===========================================================================*/
/* Console Command Buffer (defined in commands.c)                            */
/*===========================================================================*/

// Command buffer accessors - implemented in commands.c
extern char* get_cmd_buf(void);
extern uint8_t get_cmd_idx(void);
extern void set_cmd_idx(uint8_t idx);

// Forward declaration for mutual recursion
static void process_serial_char(int c);

// M5: Case-insensitive command prefix match
// Returns true if cmd_buf starts with the given prefix (case-insensitive)
static bool cmd_match(const char* prefix) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();
    for (int i = 0; prefix[i] != '\0'; i++) {
        if (i >= cmd_idx) return false;
        char c = cmd_buf[i];
        char p = prefix[i];
        // Convert both to uppercase for comparison
        if (c >= 'a' && c <= 'z') c -= 32;
        if (p >= 'a' && p <= 'z') p -= 32;
        if (c != p) return false;
    }
    return true;
}

// M5: Check if command matches exactly (no extra chars except space/args)
static bool cmd_is(const char* cmd) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();
    int len = 0;
    while (cmd[len]) len++;
    // Must match prefix and either end there or have space for args
    return cmd_match(cmd) && (cmd_idx == len || (cmd_idx > len && cmd_buf[len] == ' '));
}

// Get command argument as integer (after space)
int cmd_get_arg_int(int arg_start) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();
    int val = 0;
    for (int i = arg_start; i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9'; i++) {
        val = val * 10 + (cmd_buf[i] - '0');
    }
    return val;
}

/*===========================================================================*/
/* Command Processing                                                         */
/*===========================================================================*/

void check_serial_commands(void) {
    // Read ALL available characters to prevent dropping at 9600 baud
    int c;
    while ((c = uart_getc_nonblocking()) >= 0) {
        process_serial_char(c);
    }
}

static void process_serial_char(int c) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    // Echo character
    uart_putc((char)c);

    // Handle backspace
    if (c == 0x7F || c == 0x08) {
        if (cmd_idx > 0) set_cmd_idx(cmd_idx - 1);
        return;
    }

    // Handle newline - process command using table lookup
    if (c == '\r' || c == '\n') {
        uart_puts("\r\n");
        cmd_buf[cmd_idx] = '\0';

        if (cmd_idx == 0) {
            // Empty command - show help
            uart_puts("Commands: DFU, RESET, HELP, STATUS\r\n");
        } else {
            // Search command table for matching command
            bool found = false;
            for (int i = 0; cmd_table[i].name != NULL; i++) {
                if (cmd_is(cmd_table[i].name)) {
                    cmd_table[i].handler();
                    found = true;
                    break;
                }
            }
            if (!found) {
                uart_puts("Unknown: ");
                uart_puts(cmd_buf);
                uart_puts("\r\nType HELP for commands\r\n");
            }
        }

        set_cmd_idx(0);
        uart_puts("> ");
        return;
    }

    // Add character to buffer (with bounds checking)
    #define CMD_BUF_SIZE 32  // Must match commands.c
    if (cmd_idx < CMD_BUF_SIZE - 1) {
        cmd_buf[cmd_idx] = (char)c;
        set_cmd_idx(cmd_idx + 1);
    } else {
        // Buffer full - reject additional characters
        uart_puts("\r\n[CMD] Buffer full!\r\n");
        set_cmd_idx(0);  // Reset buffer
        uart_puts("> ");
    }
}
