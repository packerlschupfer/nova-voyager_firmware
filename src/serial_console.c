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

/* Bounded pre-scheduler TX spin. A generous upper bound, not a tuned one: one
 * character is ~1 ms at 9600 and ~87 us at CONSOLE_BAUD=115200, so raising the
 * baud only makes this bound roomier. It exists to stop a dead or held-off TX
 * line hanging the boot banner, not to time anything. */
#define UART_TX_SPIN_LIMIT 200000u
static volatile uint8_t uart_rx_buf[UART_RX_BUF_SIZE];
static volatile uint8_t uart_rx_head = 0;  // Write position (ISR)
static volatile uint8_t uart_rx_tail = 0;  // Read position (main)

// Phase 1.1: Added timeout protection to prevent infinite loops
void uart_putc(char c) {
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(100);  // 100ms timeout

    /* REVIEW FIX: the timeout test used to be nested inside the
     * scheduler-running guard, so every character printed before
     * vTaskStartScheduler() — the whole boot banner, including the clock-fault
     * message — could spin here forever on a dead or held-off TX line.
     * uart_putc_raw() already gets this right with a spin counter. */
    uint32_t spins = 0;
    while (!(USART1->SR & USART_SR_TXE)) {
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
            if ((xTaskGetTickCount() - start) >= timeout_ticks) {
                return;  // Timeout - drop character rather than hang
            }
        } else if (++spins >= UART_TX_SPIN_LIMIT) {
            return;  // Pre-scheduler: bounded spin, then drop the character
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

/* Fault-context UART output.
 *
 * uart_putc()/uart_puts() call FreeRTOS APIs — xTaskGetTickCount() for the
 * timeout and a mutex around the string. Calling any of that from HardFault
 * (fixed priority -1, above configMAX_SYSCALL_INTERRUPT_PRIORITY) is illegal
 * and does not return. That is why the fault handler printed nothing on
 * 2026-08-29 even though the fault itself was raised correctly, and very likely
 * why two real lockups yielded no console output to work from.
 *
 * These touch only the USART registers and a spin counter. Safe from any
 * context, including one where the kernel is already broken.
 */
void uart_putc_raw(char c) {
    uint32_t spins = 200000u;   /* bounded: never hang the dying-system path */
    while (!(USART1->SR & USART_SR_TXE) && spins) { spins--; }
    USART1->DR = (uint8_t)c;
}

void uart_puts_raw(const char* s) {
    while (*s) { uart_putc_raw(*s++); }
}

int uart_peek_nonblocking(void) {
    if (uart_rx_head == uart_rx_tail) {
        return -1;  // Empty
    }
    return uart_rx_buf[uart_rx_tail];
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
    /* ONE read of DR, not two.
     *
     * The old handler read DR in the RXNE branch and then, if ORE was set,
     * read DR a second time to "clear the overrun". But on STM32F1 the
     * SR-then-DR read in the first branch already clears ORE — and the byte
     * sitting in DR on an overrun is VALID: ORE means a further byte arrived
     * while RXNE was still pending, so the byte that gets lost is the new one,
     * not the one in the register. The second read therefore threw away a good
     * character, making every overrun cost two characters and leaving the
     * command stream desynchronised rather than merely short.
     *
     * That never showed at 9600, where a character takes ~1 ms and an overrun
     * effectively cannot happen. It starts to matter as the baud rises: at
     * 115200 a character is ~87 us, and this handler runs at NVIC priority 6,
     * which FreeRTOS masks inside every critical section (BASEPRI = 5). Any
     * long critical section can therefore overrun the receiver.
     *
     * Reading DR unconditionally when either flag is set clears both and keeps
     * the byte. A dropped byte on overrun is unavoidable; corrupting the
     * stream on top of it is not. */
    uint32_t sr = USART1->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t c = (uint8_t)(USART1->DR & 0xFF);
        uint8_t next = (uart_rx_head + 1) % UART_RX_BUF_SIZE;
        if (next != uart_rx_tail) {  // Not full
            uart_rx_buf[uart_rx_head] = c;
            uart_rx_head = next;
        }
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
    /* REVIEW FIX: this kept the unbounded signed accumulator its siblings were
     * audited and fixed for — "SET 99999999999" overflowed int, which is
     * undefined behaviour, and handed the caller whatever fell out. Accumulate
     * wide and saturate. */
    int32_t val = 0;
    for (int i = arg_start; i < cmd_idx && cmd_buf[i] >= '0' && cmd_buf[i] <= '9'; i++) {
        val = val * 10 + (cmd_buf[i] - '0');
        if (val > INT16_MAX) {
            return INT16_MAX;   // saturate; every caller range-checks anyway
        }
    }
    return (int)val;
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

/* AUDIT FIX (MEDIUM, serial_console.c:282): an over-long line used to print
 * "Buffer full!", reset cmd_idx to 0, and then keep accumulating the REST of
 * that same line into the freshly emptied buffer — so when the newline finally
 * arrived, the tail of the rejected line was looked up and executed as a
 * command. Pasting a long line into the terminal could therefore run something
 * the operator never typed. Once a line is rejected, everything up to the next
 * newline is discarded. */
static bool discarding_line = false;

static void process_serial_char(int c) {
    char* cmd_buf = get_cmd_buf();
    uint8_t cmd_idx = get_cmd_idx();

    // Echo character
    uart_putc((char)c);

    if (discarding_line) {
        if (c == '\r' || c == '\n') {
            discarding_line = false;
            set_cmd_idx(0);
            uart_puts("\r\n> ");
        }
        return;
    }

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
                    /* REVIEW FIX (HIGH): CMD_FLAG_DEBUG only ever hid a command
                     * from the HELP listing — dispatch ignored it entirely, so a
                     * command marked debug-only still RAN in a release build.
                     * That is a trap: marking MQ (which assembles arbitrary MCB
                     * command frames from two console characters — "MQ ST" is a
                     * start) as debug looked like it removed it from release and
                     * did nothing of the kind. Every other debug command is
                     * inside #ifdef BUILD_DEBUG, so honouring the flag here
                     * changes exactly the entries that are not, and makes the
                     * flag mean what its name says. */
                    #ifndef BUILD_DEBUG
                    if (cmd_table[i].flags & CMD_FLAG_DEBUG) {
                        uart_puts("Debug-only command (not available in this build)\r\n");
                        found = true;
                        break;
                    }
                    #endif
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
        // Buffer full - reject this line and everything up to the next newline.
        uart_puts("\r\n[CMD] Buffer full - line discarded\r\n");
        set_cmd_idx(0);
        discarding_line = true;
    }
}
