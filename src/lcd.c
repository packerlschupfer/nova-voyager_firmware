/**
 * @file lcd.c
 * @brief ST7920 16x4 Character LCD Driver
 *
 * Driver for the Nova Voyager's ST7920-based 16x4 character LCD.
 * 8-bit parallel interface on GPIOA (data) and GPIOB (control).
 * Text mode uses 8x16 HCGROM font (not HD44780 5x8).
 * CGRAM is 16x16 / 4 chars / 2-byte display codes.
 */

#include "lcd.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "lcd_graphics.h"
#include "nv_splash.h"
#include "shared.h"  // For delay_ms() in graphics tests
#include "stm32f1xx_hal.h"
#include <string.h>

// External debug output
extern void uart_puts(const char* s);

/*===========================================================================*/
/* Hardware Macros                                                            */
/*===========================================================================*/

#define LCD_RS_HIGH()   (GPIOB->BSRR = (1 << 0))
#define LCD_RS_LOW()    (GPIOB->BRR  = (1 << 0))
#define LCD_RW_HIGH()   (GPIOB->BSRR = (1 << 1))
#define LCD_RW_LOW()    (GPIOB->BRR  = (1 << 1))
#define LCD_E_HIGH()    (GPIOB->BSRR = (1 << 2))
#define LCD_E_LOW()     (GPIOB->BRR  = (1 << 2))

/*===========================================================================*/
/* Private Functions                                                          */
/*===========================================================================*/

// Busy-wait microsecond delay
// Calibrated for GD32F303 @ 120MHz with -Os (~8 cycles/iter on M4)
// 1μs = 120MHz / 8cycles = 15 iterations → multiply by 12 for margin
static void lcd_delay_us(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 12; i++);
}

// Write byte to LCD data bus with enable pulse
// Timing (120MHz, -Os): RS setup ~2μs >> 100ns min; E-high ~2μs >> 450ns min;
// E-low ~150μs >> 70μs min; total cycle ~152μs >> 72μs min spec.
static void lcd_write_byte(uint8_t data) {
    GPIOA->ODR = (GPIOA->ODR & 0xFF00) | data;
    lcd_delay_us(2);   // RS/data setup: ~2μs (need ≥100ns)
    LCD_E_HIGH();
    lcd_delay_us(2);   // E-high: ~2μs (need ≥450ns)
    LCD_E_LOW();
    lcd_delay_us(125); // E-low: ~150μs (need ≥70μs for 72μs cycle)
}

/*===========================================================================*/
/* Public Functions                                                           */
/*===========================================================================*/

/* REVIEW FIX: the LCD had no lock at all, and it is driven from task_ui (50 Hz
 * display + menu), from console handlers in task_main, and from the game task.
 * lcd_write_byte() sets RS and latches the data byte microseconds later, and
 * GPIOA->ODR is a read-modify-write — so a preemption mid-byte latches a data
 * byte as a COMMAND and leaves the controller in an arbitrary mode.
 *
 * The previous attempt at this suspended task_ui around SELFTEST. That was the
 * wrong shape and review found three defects in it: the suspend could land
 * inside uart_puts() and strand g_uart_mutex (deadlock, then watchdog reset);
 * the eTaskGetState guard was a TOCTOU that still allowed two writers when a
 * game was already running; and a >2 s suspension pushed heartbeat_ui past its
 * deadline and raised a false "UI stuck" alarm. Locking the bus removes all
 * three at once — nothing needs suspending.
 *
 * RECURSIVE because the public functions nest: lcd_print_at() calls
 * lcd_set_cursor() then lcd_print(). Scheduler-aware because lcd_init() runs
 * before vTaskStartScheduler(). */
static SemaphoreHandle_t s_lcd_mutex = NULL;
static StaticSemaphore_t s_lcd_mutex_buf;

void lcd_lock_init(void) {
    if (!s_lcd_mutex) {
        s_lcd_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_lcd_mutex_buf);
    }
}

static bool lcd_lock(void) {
    if (s_lcd_mutex && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xPortIsInsideInterrupt() == pdFALSE) {
        return xSemaphoreTakeRecursive(s_lcd_mutex, portMAX_DELAY) == pdTRUE;
    }
    return false;
}

static void lcd_unlock(bool taken) {
    if (taken) {
        xSemaphoreGiveRecursive(s_lcd_mutex);
    }
}

void lcd_delay_ms(uint32_t ms) {
    /* Busy-wait delay - safe before the FreeRTOS scheduler starts.
     * ~10 cycles/iteration, so one millisecond is SYSCLK/10 iterations.
     * REVIEW FIX: was a fixed 12000, correct only at 120 MHz; the 72 MHz
     * envs ran every boot-path delay 67 % long. */
    const uint32_t iters_per_ms = SYSCLK_FREQ / 10000u;
    for (volatile uint32_t i = 0; i < ms * iters_per_ms; i++);
}

void lcd_cmd(uint8_t cmd) {
    const bool _l = lcd_lock();
        LCD_RS_LOW();
        LCD_RW_LOW();
        lcd_write_byte(cmd);
        if (cmd <= 0x03) {
            lcd_delay_ms(2);  // Clear/home need extra time
        }
    lcd_unlock(_l);
}

void lcd_data(uint8_t data) {
    const bool _l = lcd_lock();
        LCD_RS_HIGH();
        LCD_RW_LOW();
        lcd_write_byte(data);
    lcd_unlock(_l);
}

void lcd_clear(void) {
    const bool _l = lcd_lock();
        lcd_cmd(0x01);
        lcd_delay_ms(2);
        lcd_shadow_invalidate();
    lcd_unlock(_l);
}

void lcd_set_cursor(uint8_t row, uint8_t col) {
    const bool _l = lcd_lock();
        // Nova Voyager 16x4 LCD - non-standard but continuous addressing:
        // DDRAM is WORD-addressed — each address holds 2 characters (see the note
        // above lcd_update_row). Each row therefore owns 8 addresses, not 16:
        //   Row 0: 0xC0-0xC7   Row 1: 0xD0-0xD7
        //   Row 2: 0xC8-0xCF   Row 3: 0xD8-0xDF
        // `col` here is a WORD index 0-7, so col N addresses characters 2N and
        // 2N+1. Passing col >= 8 does not clip — it runs into the next row's base
        // (row 1 col 8 == 0xD8 == row 3 col 0), which is exactly how the CalcRPM
        // screen ended up printing onto row 3.
        //
        // The old comment here read "0xC0-0xCF (16 chars)" per row, i.e. one
        // character per address. That contradicted the word-addressing note below
        // and is what made the row-1/row-3 overlap look like a quirk rather than
        // arithmetic.
        // Auto-increment runs through the row's 8 words (16 characters) and
        // then continues into whichever row follows in DDRAM — a string longer
        // than the remaining width spills onto another physical line.
        static const uint8_t row_bases[4] = {0xC0, 0xD0, 0xC8, 0xD8};
        /* REVIEW FIX: mask to 0-7, not 0-15. A word index of 8 or more is not
         * "off the end of the row" — it is the NEXT row's base, so it silently
         * writes onto a different physical line. That is the bug just fixed in
         * menu.c's CalcRPM screen; clamping here makes the whole class
         * unreachable instead of relying on every caller to know the addressing.
         * CLAMP, do not mask: `col & 0x07` maps 8 -> 0, so an out-of-range column
         * would silently overwrite the START of the row — still wrong, just wrong
         * somewhere new, and it removes the loud symptom (text appearing on
         * another physical line) that made the CalcRPM bug findable in the first
         * place. Clamping puts it at the row's last word, where it is visibly
         * truncated rather than misplaced. */
        const uint8_t word = (col > 7) ? 7 : col;
        uint8_t addr = row_bases[row & 3] + word;
        lcd_cmd(addr);
    lcd_unlock(_l);
}

void lcd_print(const char* str) {
    const bool _l = lcd_lock();
        while (*str) {
            lcd_data(*str++);
        }
    lcd_unlock(_l);
}

void lcd_print_at(uint8_t row, uint8_t col, const char* str) {
    const bool _l = lcd_lock();
        lcd_set_cursor(row, col);
        lcd_print(str);
    lcd_unlock(_l);
}

/*===========================================================================*/
/* Dirty-Row Update (only rewrite changed characters)                        */
/*===========================================================================*/

static char shadow[LCD_ROWS][LCD_COLS];
static bool shadow_valid = false;

/* Read-only view of the text shadow, for the console LCD dump.
 *
 * CAVEAT worth knowing before trusting the output: this shadow tracks only
 * what went through the dirty-row path. lcd_print_at() bypasses it, so any
 * full-screen flow that paints directly (and is supposed to call
 * lcd_shadow_invalidate() on the way out) can leave these rows stale. A dump
 * is evidence of what the row-painting path last wrote, not a photograph. */
bool lcd_shadow_snapshot(char out[LCD_ROWS][LCD_COLS]) {
    /* Copy all four rows atomically rather than handing out a live pointer.
     * The dirty-row updater memcpy's into these rows from the display task, so
     * a caller that printed straight from the array could be preempted
     * mid-dump and emit a row mixing pre- and post-update content, with
     * nothing to show it had happened. That is a poor property for what is now
     * the field build's answer to "what is the panel showing" — a silently
     * torn row misleads exactly the diagnosis it exists to support.
     * 64 bytes under a critical section is a few microseconds. */
    bool valid;
    taskENTER_CRITICAL();
    valid = shadow_valid;
    if (valid) {
        memcpy(out, shadow, sizeof(shadow));
    }
    taskEXIT_CRITICAL();
    return valid;
}

// ST7920 DDRAM is word-addressed: each address holds 2 bytes (one 16×16 char
// or two 8×16 HCGROM chars). lcd_set_cursor(row, word) positions by WORD index
// (0-7), not byte index. We track dirty at word granularity (2-byte pairs),
// writing only changed word spans.
// Typical: RPM changes 4 digits in words 0-1 → 1 cursor + 4 data = 5 writes
// instead of 17 for a full row (cursor + 16 data).

void lcd_update_row(uint8_t row, const char* buf) {
    const bool _l = lcd_lock();
        if (row >= LCD_ROWS) do { lcd_unlock(_l); return; } while (0);

        if (!shadow_valid) {
            lcd_set_cursor(row, 0);
            for (uint8_t i = 0; i < LCD_COLS; i++) lcd_data(buf[i]);
            memcpy(shadow[row], buf, LCD_COLS);
            do { lcd_unlock(_l); return; } while (0);
        }

        // Find first and last dirty WORD (2-byte pair)
        int first_word = -1, last_word = -1;
        for (int w = 0; w < LCD_COLS / 2; w++) {
            int b = w * 2;
            if (buf[b] != shadow[row][b] || buf[b + 1] != shadow[row][b + 1]) {
                if (first_word < 0) first_word = w;
                last_word = w;
            }
        }
        if (first_word < 0) do { lcd_unlock(_l); return; } while (0);

        lcd_set_cursor(row, first_word);
        for (int w = first_word; w <= last_word; w++) {
            int b = w * 2;
            lcd_data(buf[b]);
            lcd_data(buf[b + 1]);
            shadow[row][b] = buf[b];
            shadow[row][b + 1] = buf[b + 1];
        }
    lcd_unlock(_l);
}

void lcd_update_row_2byte(uint8_t row, const uint8_t* buf) {
    const bool _l = lcd_lock();
        if (row >= LCD_ROWS) do { lcd_unlock(_l); return; } while (0);

        if (!shadow_valid) {
            lcd_set_cursor(row, 0);
            for (uint8_t i = 0; i < LCD_COLS; i++) lcd_data(buf[i]);
            memcpy(shadow[row], buf, LCD_COLS);
            do { lcd_unlock(_l); return; } while (0);
        }

        int first_word = -1, last_word = -1;
        for (int w = 0; w < LCD_COLS / 2; w++) {
            int b = w * 2;
            if (buf[b] != (uint8_t)shadow[row][b] || buf[b + 1] != (uint8_t)shadow[row][b + 1]) {
                if (first_word < 0) first_word = w;
                last_word = w;
            }
        }
        if (first_word < 0) do { lcd_unlock(_l); return; } while (0);

        lcd_set_cursor(row, first_word);
        for (int w = first_word; w <= last_word; w++) {
            int b = w * 2;
            lcd_data(buf[b]);
            lcd_data(buf[b + 1]);
            shadow[row][b] = buf[b];
            shadow[row][b + 1] = buf[b + 1];
        }
    lcd_unlock(_l);
}

void lcd_shadow_invalidate(void) {
    shadow_valid = false;
}

void lcd_shadow_commit(void) {
    const bool _l = lcd_lock();
        shadow_valid = true;
    lcd_unlock(_l);
}

/*===========================================================================*/
/* Graphics Capability Test Functions                                        */
/*===========================================================================*/

/**
 * @brief Create custom character in CGRAM
 * @param location Custom char location (0-7)
 * @param data 8 bytes of pixel data (5 bits used per byte)
 */
// lcd_create_char removed — no-op on ST7920 (CGRAM is 16x16, not HD44780 5x8)
// ST7920 CGRAM is written directly in display_init() using the 16x16 protocol

/**
 * @brief Test ST7920 graphics mode capability
 * Attempts to enable graphics mode and draw test pattern
 */
void lcd_test_graphics_mode(void) {
    uart_puts("Testing ST7920 graphics mode...\r\n");

    // Save current state
    lcd_clear();

    // Try ST7920 extended instruction set
    lcd_cmd(0x30);  // Basic instruction set (8-bit interface)
    lcd_delay_ms(1);

    lcd_cmd(0x34);  // Extended instruction set enable
    lcd_delay_ms(1);

    lcd_cmd(0x36);  // Graphics display ON
    lcd_delay_ms(1);

    // Try to write pixel data at position (0,0)
    lcd_cmd(0x80);  // Set Y address (vertical, 0-31)
    lcd_cmd(0x80);  // Set X address (horizontal, 0-15 for 128 pixels/8)

    // Write test pattern (2 bytes = 16 pixels horizontal)
    lcd_data(0xFF);  // All pixels ON (first 8 pixels)
    lcd_data(0xFF);  // All pixels ON (next 8 pixels)

    lcd_delay_ms(10);  // Let display update

    // Try more positions
    lcd_cmd(0x81);  // Y=1
    lcd_cmd(0x80);  // X=0
    lcd_data(0xAA);  // Alternating pattern 10101010
    lcd_data(0x55);  // Alternating pattern 01010101

    // FIXED: Use FreeRTOS delay instead of busy-wait (prevents watchdog reset)
    delay_ms(500);  // Show for 0.5 seconds (was 2s busy-wait that caused watchdog)

    // Return to text mode — must clear G-bit first (sticky)
    lcd_cmd(0x34);  // Extended mode with G=0
    lcd_delay_ms(1);
    lcd_cmd(0x30);  // Basic instruction set
    lcd_delay_ms(1);

    lcd_clear();
    lcd_print_at(0, 0, "Graphics test OK");
    lcd_print_at(1, 0, "Did you see");
    lcd_print_at(2, 0, "pixel pattern?");

    uart_puts("Graphics mode test complete\r\n");
    uart_puts("If you saw pixels/pattern -> ST7920 graphics works!\r\n");
    uart_puts("If display went blank -> Graphics mode not supported\r\n");
}

/**
 * @brief Test display capabilities and report findings
 */
void lcd_test_capabilities(void) {
    uart_puts("\r\n=== LCD CAPABILITY TEST ===\r\n\r\n");

    // Test: ST7920 graphics mode
    uart_puts("Test: ST7920 Graphics Mode\r\n");
    lcd_test_graphics_mode();

    uart_puts("\r\n=== TEST COMPLETE ===\r\n");
    uart_puts("Results:\r\n");
    uart_puts("- CGRAM: Check if heart icon appeared\r\n");
    uart_puts("- Graphics: Check if pixels appeared before text\r\n");
}

void lcd_init(bool show_splash) {
    uart_puts("LCD: GPIO...\r\n");

    // Enable GPIO clocks
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;

    // PA0-PA7: 8-bit data bus (push-pull 50MHz)
    GPIOA->CRL = 0x33333333;

    // PB0=RS, PB1=RW, PB2=E (push-pull 50MHz)
    GPIOB->CRL &= ~0x00000FFF;
    GPIOB->CRL |= 0x00000333;

    // Initialize control signals
    LCD_RS_LOW();
    LCD_RW_LOW();
    LCD_E_LOW();
    GPIOA->ODR = 0;

    uart_puts("LCD: delay 50ms...\r\n");
    lcd_delay_ms(50);  // Power-up delay

    uart_puts("LCD: cmd 0x38...\r\n");
    // HD44780 initialization sequence
    lcd_cmd(0x38);  // 8-bit, 2-line, 5x8
    lcd_delay_ms(5);
    lcd_cmd(0x38);
    lcd_delay_ms(1);
    lcd_cmd(0x38);

    // Display OFF immediately — hide XOR artifacts during GRAM wipe on warm boot
    lcd_cmd(0x08);

    // Clear GRAM — previous session may have left graphics data that XORs with text.
    lcd_cmd(0x34);  // Extended instruction set
    lcd_delay_ms(1);
    lcd_cmd(0x36);  // Graphics ON
    lcd_delay_ms(1);
    for (uint8_t y = 0; y < 32; y++) {
        lcd_cmd(0x80 | y);
        lcd_cmd(0x80);      // X=0 (upper half)
        for (uint8_t x = 0; x < 16; x++) lcd_data(0x00);
        lcd_cmd(0x80 | y);
        lcd_cmd(0x88);      // X=8 (lower half)
        for (uint8_t x = 0; x < 16; x++) lcd_data(0x00);
    }
    lcd_cmd(0x34);  // G=0 (clear sticky graphics bit)
    lcd_delay_ms(1);

    uart_puts("LCD: cmd 0x0C, 0x06...\r\n");
    lcd_cmd(0x30);  // Back to basic mode
    lcd_cmd(0x0C);  // Display on, cursor off
    lcd_cmd(0x06);  // Entry mode: increment

    uart_puts("LCD: clear...\r\n");
    lcd_clear();

    // Conditional splash screen (only on cold boot for fast soft boot).
    // Renders the 128×64 1-bit boot bitmap in graphics mode, then drops back
    // to text mode for the rest of boot / the UI. Timing budget matches the
    // old 300 ms text splash so the IWDG margin is unchanged.
    if (show_splash) {
        uart_puts("LCD: splash...\r\n");
        lcd_graphics_mode(true);
        lcd_graphics_blit_1bit(nv_splash_128x64);
        IWDG->KR = 0xAAAA;  // Refresh — graphics-mode init + blit takes ~30 ms
        lcd_delay_ms(600);   // A touch longer than text splash — bitmap deserves it
        IWDG->KR = 0xAAAA;
        lcd_graphics_mode(false);   // Back to DDRAM text mode for the UI
#ifdef BUILD_READONLY
        // Read-only demo tag stays as text — the boot bitmap doesn't
        // differentiate demo vs full firmware, so print a small note under it.
        lcd_clear();
        lcd_print_at(0, 2, "NOVA VOYAGER");
        lcd_print_at(1, 1, "READ-ONLY DEMO");
        lcd_print_at(2, 0, "EEPROM untouched");
        lcd_delay_ms(400);
        IWDG->KR = 0xAAAA;
#endif
    }

    uart_puts("LCD: done\r\n");
}
