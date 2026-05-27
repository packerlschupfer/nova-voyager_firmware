/**
 * @file dfu.h
 * @brief The one authority for the bootloader DFU hand-off.
 *
 * Hand-off contract with nova-voyager_bootloader: it re-enters DFU when it
 * finds this value at this address after a reset (bootloader src/main.c:66-67,
 * address reserved by its linker as _dfu_magic and ASSERT-guarded against its
 * own .bss, so the value survives the bootloader zeroing BSS).
 *
 * WHY THIS HEADER EXISTS
 * ----------------------
 * The address/value pair used to be written inline in two places. Both were
 * wrong: 0xDEADBEEF at 0x20000000 matched neither the address nor the value,
 * so DFU never entered DFU mode — it just reset back into the application —
 * and 0x20000000 is the base of SRAM, so it scribbled four bytes over live
 * .data on the way out. The console command was corrected on 2026-08-29 and
 * verified on hardware. The front-panel System -> DFU action was NOT, and kept
 * writing the old pair until 2026-08-30, so the menu update path was dead
 * while the console one worked.
 *
 * One definition, one function, no second copy to miss. If you add another
 * entry point, call dfu_reboot_into_bootloader() — do not write the word.
 */

#ifndef DFU_H
#define DFU_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

/* The linker owns this address: ldscript.ld defines _dfu_magic and ASSERTs
 * that RAM allocation never reaches it, the same way the bootloader's script
 * guards its own copy. Taking the address from the symbol means the two can no
 * longer drift apart silently. */
extern uint32_t _dfu_magic;
#define BOOTLOADER_DFU_MAGIC_ADDR  ((volatile uint32_t*)&_dfu_magic)
#define BOOTLOADER_DFU_MAGIC_VALUE 0x424CU  /* "BL" */

/**
 * @brief Arm the bootloader hand-off and reset. Does not return.
 *
 * NOTE: DFU only exists on the 72 MHz bootloader — the 120 MHz build cannot
 * derive the 48 MHz USB needs. On a 120 MHz pairing this simply reboots. The
 * callers warn about that; the firmware cannot detect which bootloader is
 * installed, only its own clock.
 */
static inline void dfu_reboot_into_bootloader(void) {
    /* REVIEW FIX: the spin (there to let the console line drain before the
     * reset) used to run with interrupts and the scheduler live, between
     * writing the magic and resetting — a window in which any other task could
     * still run and, once RAM grows toward the word, clobber it. Drain first,
     * then arm and reset with nothing else able to execute. */
    for (volatile int i = 0; i < 100000; i++);
    __disable_irq();
    *BOOTLOADER_DFU_MAGIC_ADDR = BOOTLOADER_DFU_MAGIC_VALUE;
    __DSB();
    NVIC_SystemReset();
}

#endif /* DFU_H */
