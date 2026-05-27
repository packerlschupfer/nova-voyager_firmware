/**
 * @file init.c
 * @brief System initialization implementation
 */

#include "init.h"
#include "config.h"
#include "shared.h"
#include "stm32f1xx_hal.h"
#include <string.h>

/* External dependencies */
extern shared_state_t g_state;

/*===========================================================================*/
/* Clock Configuration - from 8MHz HSE (USE_120MHZ in config.h)             */
/*===========================================================================*/

// Set when the external crystal failed to start and we fell back to HSI.
//
// Deliberately a standalone global rather than a g_state field: clock_init()
// runs before the shared-state init memsets g_state, so a flag stored there
// would be zeroed again before anything could read it.
volatile bool g_clock_fault = false;

// Bounded wait on an RCC->CR bit reaching a wanted state.
//
// The bootloader falls back to HSI and jumps to the app on a dead crystal
// specifically so the board still comes up; an unbounded wait here would just
// move the hang into the application and waste that. See safety.h — a clock we
// cannot trust means we refuse to turn the spindle rather than run degraded.
//
// ALL THREE waits in clock_init must be bounded, not just HSERDY. Found on
// hardware 2026-08-29 with env:hse_fail_test: bounding only HSERDY still hung
// the board forever in the PLLRDY spin, because a crystal that stops after
// HSERDY has appeared leaves the PLL unable to lock. One bounded wait out of
// three is not a fix, it just moves the hang.
static bool rcc_wait_cr(uint32_t bit, bool want_set) {
    volatile uint32_t timeout = HSE_STARTUP_TIMEOUT_LOOPS;
    while (((RCC->CR & (1u << bit)) != 0u) != want_set) {
        if (--timeout == 0u) {
            return false;
        }
    }
    return true;
}

// Bounded wait for SYSCLK to actually be the PLL (SWS == 0b10).
static bool rcc_wait_sws_pll(void) {
    volatile uint32_t timeout = HSE_STARTUP_TIMEOUT_LOOPS;
    while ((RCC->CFGR & 0x0Cu) != 0x08u) {
        if (--timeout == 0u) {
            return false;
        }
    }
    return true;
}

// Give up on the external clock: stop the PLL, stay on HSI at a KNOWN bus
// configuration, latch the fault.
//
// The prescalers are normalised rather than inherited, and that matters more
// than it looks. This path's entire purpose is to report the fault, and
// uart_init() picks its 8 MHz divisor (0x341) for USART1 on APB2. If APB2 were
// divided down relative to SYSCLK the message would come out at the wrong baud
// — garbled at exactly the moment it is needed.
//
// It happens to work today only because nova-voyager_bootloader never sets
// PPRE2, so APB2 tracks HCLK on every path it has. That is an implicit
// dependency on another repo's register leftovers, discovered 2026-08-29 while
// correcting a wrong attribution. Setting them here makes the fault console
// deterministic whatever we inherit.
//
// Mask 0x3FF3 = SW (1:0) | HPRE (7:4) | PPRE1 (10:8) | PPRE2 (13:11), i.e.
// SYSCLK = HSI with every bus prescaler at /1. PLL config bits are left alone;
// the PLL is off.
static void clock_fail_to_hsi(void) {
    RCC->CR &= ~(1u << 24);          // PLLON = 0
    RCC->CFGR &= ~0x3FF3u;           // SW = HSI, HPRE/PPRE1/PPRE2 = /1
    g_clock_fault = true;
}

void clock_init(void) {
    // The bootloader hands over with HSE up, the PLL running and SYSCLK already
    // switched to the PLL. On F1/GD32 the PLLMUL field is read-only while PLLON
    // is set, so writing RCC->CFGR from that state silently drops the
    // multiplier: the core keeps whatever the bootloader picked while the code
    // below believes it configured the clock. With a 72 MHz bootloader and a
    // 120 MHz firmware that means running at 72 and thinking it is 120 — every
    // derived timing (baud, delays, PWM) off by 1.67x, with nothing to see.
    //
    // Park on HSI and stop the PLL first so the configuration below always
    // lands, whatever clock state we inherited. Note SYSCLK must leave the PLL
    // *before* PLLON is cleared, or we stop the clock we are running on.
    RCC->CR |= (1 << 0);                     // HSION
    while (!(RCC->CR & (1 << 1)));           // wait HSIRDY
    RCC->CFGR &= ~0x03u;                     // SW = HSI
    while ((RCC->CFGR & 0x0C) != 0x00);      // wait SWS = HSI
    RCC->CR &= ~(1 << 24);                   // PLLON = 0
    while (RCC->CR & (1 << 25));             // wait !PLLRDY

#if USE_120MHZ
    // GD32F303 at 120MHz
    // Set flash wait states (3 for 96-120MHz)
    FLASH->ACR = 0x33;  // 3 wait states + prefetch enable

    // Enable HSE (8MHz external crystal)
#if HSE_FAIL_TEST
    // Test-only build (env:hse_fail_test): force the crystal-dead path so the
    // whole fault response can be exercised on hardware without a dead crystal.
    //
    // This CLEARS HSEON rather than merely skipping the enable, because the
    // bootloader hands over with HSE already running — skipping our write would
    // leave HSERDY set from its configuration and the wait would succeed.
    //
    // And it then waits for HSERDY to actually drop. Hardware does not clear
    // that bit in the same cycle as the HSEON write, so polling immediately
    // reads the stale ready bit and sails through. That is exactly what
    // happened on the first attempt at this test: the fault never fired.
    RCC->CR &= ~(1u << 16);              // HSEOFF
    (void)rcc_wait_cr(17, false);        // let HSERDY actually fall
#else
    RCC->CR |= (1 << 16);  // HSEON
#endif
    if (!rcc_wait_cr(17, true)) {
        // Crystal dead. uart_init() reads SWS and picks the 8 MHz divisor, so
        // the console still comes up readable to report this, and the motor
        // stays refused via safety_can_start_motor(). Same normalising exit as
        // the PLL failures below — all three failures leave one known state.
        clock_fail_to_hsi();
        return;
    }

    // Configure PLL: HSE * 15 = 120MHz, APB1 = /4 (30MHz), APB2 = /1 (120MHz)
    // PLLSRC=HSE (bit 16), PLLMUL=15 (bits 21:18 = 1101), PPRE1=/4 (bits 10:8 = 101)
    //
    // AUDIT FIX (MEDIUM, init.c:136): this assigns CFGR wholesale, which also
    // writes ADCPRE (bits 15:14) as 00 = PCLK2/2. With APB2 undivided at
    // 120 MHz that clocks the ADC at 60 MHz, well over the part's 40 MHz
    // maximum, on all four 120 MHz builds — and the ADC is the quill depth
    // sensor and the die thermometer, so out-of-spec here means depth readings
    // of unknown accuracy. ADCPRE = 11 (PCLK2/8) gives 15 MHz.
    RCC->CFGR = (1 << 16) | (0xD << 18) | (5 << 8) | (3 << 14);

    // Enable PLL (bounded: a crystal that dies after HSERDY leaves this hanging)
    RCC->CR |= (1 << 24);  // PLLON
    if (!rcc_wait_cr(25, true)) {   // PLLRDY
        clock_fail_to_hsi();
        return;
    }

    // Switch to PLL as system clock (bounded for the same reason)
    RCC->CFGR |= 0x02;  // SW = PLL
    if (!rcc_wait_sws_pll()) {
        clock_fail_to_hsi();
        return;
    }
#else
    // STM32-compatible 72MHz
    // Set flash wait states (2 for 48-72MHz)
    FLASH->ACR = 0x32;  // 2 wait states + prefetch enable

    // Enable HSE (8MHz external crystal)
#if HSE_FAIL_TEST
    // Test-only build (env:hse_fail_test): force the crystal-dead path so the
    // whole fault response can be exercised on hardware without a dead crystal.
    //
    // This CLEARS HSEON rather than merely skipping the enable, because the
    // bootloader hands over with HSE already running — skipping our write would
    // leave HSERDY set from its configuration and the wait would succeed.
    //
    // And it then waits for HSERDY to actually drop. Hardware does not clear
    // that bit in the same cycle as the HSEON write, so polling immediately
    // reads the stale ready bit and sails through. That is exactly what
    // happened on the first attempt at this test: the fault never fired.
    RCC->CR &= ~(1u << 16);              // HSEOFF
    (void)rcc_wait_cr(17, false);        // let HSERDY actually fall
#else
    RCC->CR |= (1 << 16);  // HSEON
#endif
    if (!rcc_wait_cr(17, true)) {
        // Crystal dead. uart_init() reads SWS and picks the 8 MHz divisor, so
        // the console still comes up readable to report this, and the motor
        // stays refused via safety_can_start_motor(). Same normalising exit as
        // the PLL failures below — all three failures leave one known state.
        clock_fail_to_hsi();
        return;
    }

    // Configure PLL: HSE * 9 = 72MHz, APB1 = /2 (36MHz max)
    // PLLSRC=HSE (bit 16), PLLMUL=9 (bits 21:18 = 0111), PPRE1=/2 (bits 10:8 = 100)
    //
    // AUDIT FIX (MEDIUM, init.c:136): ADCPRE (bits 15:14) is written here too
    // rather than left at the reset default of PCLK2/2, which would be 36 MHz.
    // 10 = PCLK2/6 gives 12 MHz, inside the 14 MHz STM32F1 limit as well as
    // the GD32's 40 MHz.
    RCC->CFGR = (1 << 16) | (7 << 18) | (4 << 8) | (2 << 14);

    // Enable PLL (bounded: a crystal that dies after HSERDY leaves this hanging)
    RCC->CR |= (1 << 24);  // PLLON
    if (!rcc_wait_cr(25, true)) {   // PLLRDY
        clock_fail_to_hsi();
        return;
    }

    // Switch to PLL as system clock (bounded for the same reason)
    RCC->CFGR |= 0x02;  // SW = PLL
    if (!rcc_wait_sws_pll()) {
        clock_fail_to_hsi();
        return;
    }
#endif
}

/*===========================================================================*/
/* UART Initialization                                                       */
/*===========================================================================*/

void uart_init(void) {
    // Same init as working code
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_IOPAEN;
    GPIOA->CRH &= ~(0xFF << 4);
    GPIOA->CRH |= (0xB << 4);    // PA9: AF push-pull 50MHz (TX)
    GPIOA->CRH |= (0x4 << 8);    // PA10: Floating input (RX)

    /* Clock-aware baud rate: USART1 is on APB2.
     * At PLL: APB2_FREQ from config.h. At HSI the PLL never locked, so the
     * core is on the 8 MHz internal oscillator and the divisor must come from
     * that instead — a fault path that still has to be able to say so on the
     * console. Both divisors are computed from CONSOLE_BAUD so the two can
     * never drift apart; the old code hardcoded the HSI case as 0x341, which
     * silently meant 9600 no matter what the PLL case said.
     *
     * Integer division error at 115200: 120 MHz -> 1041 (+0.06%),
     * 72 MHz -> 625 (exact), 8 MHz -> 69 (+0.64%). All well inside the ~2%
     * that 8N1 framing tolerates. */
    uint32_t sws = (RCC->CFGR >> 2) & 0x3;
    USART1->BRR = (sws == 0x02) ? (APB2_FREQ / CONSOLE_BAUD)
                                : (8000000u / CONSOLE_BAUD);

    // Enable TX, RX, RXNE interrupt, and USART
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    // Enable USART1 interrupt in NVIC (lower priority than FreeRTOS kernel)
    NVIC_SetPriority(USART1_IRQn, 6);
    NVIC_EnableIRQ(USART1_IRQn);
}

/*===========================================================================*/
/* Shared State Initialization                                               */
/*===========================================================================*/

void shared_init(void) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.state = APP_STATE_STARTUP;
    g_state.target_rpm = SPEED_DEFAULT_RPM;
    // 0 /* tap_mode removed */ removed;
}
