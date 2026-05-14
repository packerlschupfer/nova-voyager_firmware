/*
 * Nova Voyager Custom Firmware Linker Script
 * GD32F303RCT6 - 256KB Flash, 48KB SRAM, Cortex-M4, 120MHz (USE_120MHZ=1)
 *
 * Note: GD32 is largely STM32-compatible, uses STM32F1xx HAL
 * Note: GD32F303RCT6 has 48KB SRAM (not 64KB - that's the RET6 variant)
 *
 * Memory Layout:
 *   0x08000000 - 0x08002FFF: Bootloader (12KB) - DO NOT OVERWRITE
 *   0x08003000 - 0x0803FFFF: Application (244KB available)
 *   0x20000000 - 0x2000BFFF: SRAM (48KB)
 */

MEMORY
{
    /* Skip bootloader region - app starts at 0x08003000 */
    FLASH (rx)  : ORIGIN = 0x08003000, LENGTH = 244K
    /* GD32F303RCT6 has 48KB SRAM (RET6 variant has 64KB) */
    RAM (rwx)   : ORIGIN = 0x20000000, LENGTH = 48K
}

/* Entry point */
ENTRY(Reset_Handler)

/* Stack at end of 48KB RAM */
_estack = 0x2000C000;  /* End of 48KB RAM */

/* Minimum heap and stack sizes */
_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;

SECTIONS
{
    /* Vector table must be at start of flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    /* Code */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)

        KEEP(*(.init))
        KEEP(*(.fini))

        . = ALIGN(4);
        _etext = .;
    } >FLASH

    /* Read-only data */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    /* ARM exception handling */
    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } >FLASH

    .ARM :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } >FLASH

    /* Used by startup to initialize data */
    _sidata = LOADADDR(.data);

    /* Initialized data (copied from flash to RAM) */
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT> FLASH

    /* Uninitialized data (zeroed) */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } >RAM

    /* User heap */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } >RAM

    /* Remove debug info from release builds */
    /DISCARD/ :
    {
        libc.a(*)
        libm.a(*)
        libgcc.a(*)
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
