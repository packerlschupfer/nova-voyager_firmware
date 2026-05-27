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
    /* Skip bootloader region - app starts at 0x08003000.
     * AUDIT FIX (MEDIUM, ldscript.ld:17): reserve the last 2 KB flash page
     * (0x0803F800..0x0803FFFF) for the SETTINGS_FLASH_ADDR fallback store,
     * so `settings_save()` can never erase live application code even at
     * maximum image growth. FLASH length shrunk from 244 K to 242 K to
     * carve that page out; linker now errors at overflow instead of the
     * flash-write silently trashing running code. */
    FLASH (rx)  : ORIGIN = 0x08003000, LENGTH = 242K
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

    /* AUDIT FIX (MEDIUM, shared.h:380): reserved uninitialized RAM for the
     * cold-boot magic word. startup_gd32.s zeroes .bss before main() runs,
     * so the old FORCE_COLD_BOOT_MAGIC_ADDR (0x20002700) was erased before
     * detect_boot_type() could read it, AND it overlapped the g_state_mutex
     * kernel object which cmd_coldboot corrupted while the scheduler was
     * still running. This section is NOLOAD so nothing writes it during
     * init; the variable retains its value across a soft reset. */
    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        KEEP(*(.noinit))
        KEEP(*(.noinit*))
        . = ALIGN(4);
        _noinit_end = .;
    } >RAM

    /* REVIEW FIX (MEDIUM): the DFU hand-off word (include/dfu.h) sits at a
     * fixed address the bootloader agrees on, and this script neither reserved
     * nor checked it — unlike the bootloader's own script, which dfu.h cites as
     * ASSERT-guarded, and unlike the cold-boot magic, which got the .noinit
     * section above after exactly this class of bug.
     *
     * Adding the check found it was ALREADY overlapped on env:nova_voyager_games
     * (4 KB FreeRTOS heap for the dynamic game task, on top of the game state):
     * pressing DFU from a games build wrote the magic into live heap and the
     * bootloader saw whatever the allocator left there. Reserving a hole here
     * fixes that rather than merely reporting it — the heap and stack now start
     * above the word instead of on top of it. */
    .dfu_magic 0x20004FF0 (NOLOAD) :
    {
        KEEP(*(.dfu_magic))
        . = . + 4;
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
        _ram_alloc_end = .;
    } >RAM

    /* REVIEW FIX (MEDIUM): the DFU hand-off word (include/dfu.h) lives at a
     * fixed address that this script did not reserve or check, unlike the
     * bootloader's linker script — which dfu.h itself cites as ASSERT-guarded
     * — and unlike the cold-boot magic, which got the .noinit section above
     * after exactly this class of bug. It is safe today only by margin: RAM
     * allocation currently ends around 0x2000392C, some 5.8 KB below. Make the
     * margin a build-time guarantee instead of a fact someone has to re-measure
     * after every struct they add. */
    /* Remove debug info from release builds */
    /DISCARD/ :
    {
        libc.a(*)
        libm.a(*)
        libgcc.a(*)
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}

_dfu_magic = ADDR(.dfu_magic);

/* Static data must stay BELOW the reserved word; the heap and stack are placed
 * above it by the section order. */
ASSERT(_noinit_end <= _dfu_magic, "static RAM (.data/.bss/.noinit) reached the DFU hand-off word at 0x20004FF0 - see include/dfu.h")
