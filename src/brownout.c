/**
 * @file brownout.c
 * @brief PVD brown-out interlock — see brownout.h for why.
 */

#include "brownout.h"
#include "stm32f1xx_hal.h"

/* Threshold selection (PWR_CR PLS[2:0]): 000=2.2 V ... 111=2.9 V.
 *
 * 2.5 V is chosen deliberately, and the trade-off runs both ways:
 *
 *  - Too HIGH (2.8-2.9 V) sits only ~0.4 V under the 3.3 V rail, close enough
 *    to normal ripple and motor-start load transients to risk a FALSE trip.
 *    A false trip here drops PD4 and stops the spindle mid-cut, which is its
 *    own hazard — the cure would be worse than the disease.
 *  - Too LOW approaches the GD32's own power-on reset (~1.8 V), by which point
 *    software has already been unreliable for a while and there is no warning
 *    left to give.
 *
 * 2.5 V leaves ~0.8 V of margin below nominal and ~0.7 V above POR: a rail at
 * 2.5 V is failing, not merely loaded. */
#define BROWNOUT_PLS_LEVEL  3u        /* 011 = 2.5 V */

/* The PVD is wired to EXTI line 16 on this family — it is not a GPIO line. */
#define PVD_EXTI_LINE       (1u << 16)

/* Global rather than file-static so include/safety.h can consult it the same
 * way it consults g_clock_fault — safety.h is header-inline and compiled by the
 * native test suite, which has no src/, so it must not depend on a function
 * living in a .c file. */
volatile bool g_brownout_latched = false;

void brownout_init(void) {
    /* PD4 FIRST. REVIEW FIX (HIGH): this function is armed second in main(),
     * but PD4's clock and push-pull config were not set until
     * motor_task_init() — reached from main() only AFTER settings_init(),
     * lcd_init() with its splash, and the boot messages, roughly a second
     * later. Until then PVD_IRQHandler's GPIOD->BSRR write targeted a
     * peripheral whose clock might be off, driving a pin still configured as a
     * floating input: the cutoff would have done nothing. That is exactly the
     * window brownout.h says this exists to cover.
     *
     * Configuring it here also means the enable line is DEFINED and LOW from
     * the earliest moment, rather than floating through boot.
     * motor_task_init() sets the same bits again later, harmlessly. */
    RCC->APB2ENR |= RCC_APB2ENR_IOPDEN;
    GPIOD->CRL &= ~(0xFu << 16);
    GPIOD->CRL |=  (0x3u << 16);          /* PD4: output push-pull, 50 MHz */
    GPIOD->BSRR = (1u << (4 + 16));       /* and LOW = motor disabled */

    /* PWR lives on APB1 and its clock is off out of reset. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;

    /* Set the level with PVDE clear, then enable — changing the threshold
     * while the detector is live can glitch its output. */
    uint32_t cr = PWR->CR;
    cr &= ~(PWR_CR_PLS | PWR_CR_PVDE);
    cr |= (BROWNOUT_PLS_LEVEL << PWR_CR_PLS_Pos);
    PWR->CR = cr;
    PWR->CR = cr | PWR_CR_PVDE;

    /* Falling edge only: VDD dropping THROUGH the threshold is the event.
     * The rising edge (supply recovering) is deliberately not wired — the latch
     * is meant to survive until a reset. */
    EXTI->PR   = PVD_EXTI_LINE;      /* discard anything already pending */
    EXTI->RTSR &= ~PVD_EXTI_LINE;
    EXTI->FTSR |=  PVD_EXTI_LINE;
    EXTI->IMR  |=  PVD_EXTI_LINE;

    /* Priority: numerically below (i.e. more urgent than) the E-Stop and guard
     * EXTIs at 6, because this one only drops a line and returns, and the whole
     * point is to beat a decaying rail. Still numerically above
     * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5) so it calls no FreeRTOS
     * API — and it does not: it touches two registers and a bool. */
    NVIC_SetPriority(PVD_IRQn, 6);
    NVIC_EnableIRQ(PVD_IRQn);
}

bool brownout_latched(void) {
    return g_brownout_latched;
}

/**
 * @brief VDD has fallen through the threshold.
 *
 * Deliberately minimal. NO uart_puts(), NO FreeRTOS calls: this can run at any
 * moment, including from inside another ISR's tail, and the console path takes
 * a mutex. Drop the motor enable line — the identical hardware cutoff the guard
 * and E-Stop ISRs perform — set the latch, and get out. Reporting is the
 * application's job once it notices brownout_latched().
 */
void PVD_IRQHandler(void) {
    EXTI->PR = PVD_EXTI_LINE;             /* clear before acting */
    GPIOD->BSRR = (1u << (4 + 16));       /* BR4: PD4 LOW = motor disabled */
    g_brownout_latched = true;
}
