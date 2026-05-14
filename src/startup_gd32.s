/**
 * @file startup_gd32.s
 * @brief Custom startup for GD32F303 with bootloader
 *
 * CRITICAL: The bootloader jumps to Reset_Handler without reinitializing SP.
 * We must manually set SP and VTOR before anything else.
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

/*===========================================================================*/
/* Vector Table - MUST be at 0x08003000                                       */
/*===========================================================================*/

    .section .isr_vector, "a", %progbits
    .global g_pfnVectors
    .type g_pfnVectors, %object

g_pfnVectors:
    .word _estack               /* 0x00: Initial Stack Pointer */
    .word Reset_Handler         /* 0x04: Reset Handler */
    .word NMI_Handler           /* 0x08: NMI Handler */
    .word HardFault_Handler     /* 0x0C: Hard Fault Handler */
    .word MemManage_Handler     /* 0x10: MPU Fault Handler */
    .word BusFault_Handler      /* 0x14: Bus Fault Handler */
    .word UsageFault_Handler    /* 0x18: Usage Fault Handler */
    .word 0                     /* 0x1C: Reserved */
    .word 0                     /* 0x20: Reserved */
    .word 0                     /* 0x24: Reserved */
    .word 0                     /* 0x28: Reserved */
    .word SVC_Handler           /* 0x2C: SVCall Handler */
    .word DebugMon_Handler      /* 0x30: Debug Monitor Handler */
    .word 0                     /* 0x34: Reserved */
    .word PendSV_Handler        /* 0x38: PendSV Handler */
    .word SysTick_Handler       /* 0x3C: SysTick Handler */
    /* External Interrupts */
    .word Default_Handler       /* 0x40: WWDG */
    .word Default_Handler       /* 0x44: PVD */
    .word Default_Handler       /* 0x48: TAMPER */
    .word Default_Handler       /* 0x4C: RTC */
    .word Default_Handler       /* 0x50: FLASH */
    .word Default_Handler       /* 0x54: RCC */
    .word EXTI0_IRQHandler      /* 0x58: EXTI0 (IRQ 6) - E-Stop PC0 */
    .word Default_Handler       /* 0x5C: EXTI1 */
    .word EXTI2_IRQHandler      /* 0x60: EXTI2 (IRQ 8) - Guard PC2 */
    .word EXTI3_IRQHandler      /* 0x64: EXTI3 (IRQ 9) - Pedal PC3 */
    .word EXTI4_IRQHandler      /* 0x68: EXTI4 (IRQ 10) - Menu PB4 */
    .word Default_Handler       /* 0x6C: DMA1_Channel1 */
    .word Default_Handler       /* 0x70: DMA1_Channel2 */
    .word Default_Handler       /* 0x74: DMA1_Channel3 */
    .word Default_Handler       /* 0x78: DMA1_Channel4 */
    .word Default_Handler       /* 0x7C: DMA1_Channel5 */
    .word Default_Handler       /* 0x80: DMA1_Channel6 */
    .word Default_Handler       /* 0x84: DMA1_Channel7 */
    .word Default_Handler       /* 0x88: ADC1_2 */
    .word Default_Handler       /* 0x8C: USB_HP_CAN_TX */
    .word Default_Handler       /* 0x90: USB_LP_CAN_RX0 */
    .word Default_Handler       /* 0x94: CAN_RX1 */
    .word Default_Handler       /* 0x98: CAN_SCE */
    .word Default_Handler       /* 0x9C: EXTI9_5 */
    .word Default_Handler       /* 0xA0: TIM1_BRK */
    .word Default_Handler       /* 0xA4: TIM1_UP */
    .word Default_Handler       /* 0xA8: TIM1_TRG_COM */
    .word Default_Handler       /* 0xAC: TIM1_CC */
    .word Default_Handler       /* 0xB0: TIM2 */
    .word Default_Handler       /* 0xB4: TIM3 */
    .word Default_Handler       /* 0xB8: TIM4 */
    .word Default_Handler       /* 0xBC: I2C1_EV */
    .word Default_Handler       /* 0xC0: I2C1_ER */
    .word Default_Handler       /* 0xC4: I2C2_EV */
    .word Default_Handler       /* 0xC8: I2C2_ER */
    .word Default_Handler       /* 0xCC: SPI1 */
    .word Default_Handler       /* 0xD0: SPI2 */
    .word USART1_IRQHandler     /* 0xD4: USART1 (IRQ 37) */
    .word Default_Handler       /* 0xD8: USART2 */
    .word Default_Handler       /* 0xDC: USART3 */
    .word EXTI15_10_IRQHandler  /* 0xE0: EXTI15_10 (IRQ 40) */
    .word Default_Handler       /* 0xE4: RTC_Alarm */
    .word Default_Handler       /* 0xE8: USBWakeUp */
    /* Remaining interrupts (43-67) */
    .rept 25
    .word Default_Handler
    .endr

    .size g_pfnVectors, .-g_pfnVectors

/*===========================================================================*/
/* Reset Handler                                                              */
/*===========================================================================*/

    .section .text.Reset_Handler
    .global Reset_Handler
    .type Reset_Handler, %function

Reset_Handler:
    /* FIRST: Set stack pointer manually (bootloader doesn't do this) */
    ldr sp, =_estack

    /* SECOND: Relocate vector table IMMEDIATELY */
    ldr r0, =0xE000ED08         /* SCB->VTOR address */
    ldr r1, =0x08003000         /* Our vector table address */
    str r1, [r0]

    /* Memory barriers */
    dsb
    isb

    /* Now safe to continue with normal startup */
    /* Copy .data from flash to RAM */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_sidata
    movs r3, #0
    b LoopCopyDataInit

CopyDataInit:
    ldr r4, [r2, r3]
    str r4, [r0, r3]
    adds r3, r3, #4

LoopCopyDataInit:
    adds r4, r0, r3
    cmp r4, r1
    bcc CopyDataInit

    /* Zero .bss */
    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
    b LoopFillZerobss

FillZerobss:
    str r2, [r0]
    adds r0, r0, #4

LoopFillZerobss:
    cmp r0, r1
    bcc FillZerobss

    /* Call static constructors */
    bl __libc_init_array

    /* Call main */
    bl main

    /* If main returns, loop forever */
Hang:
    b Hang

    .size Reset_Handler, .-Reset_Handler

/*===========================================================================*/
/* Default Exception Handlers (weak - can be overridden)                      */
/*===========================================================================*/

    .section .text.Default_Handler, "ax", %progbits
    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
    b Default_Handler
    .size Default_Handler, .-Default_Handler

    /* Weak aliases for exception handlers */
    .weak NMI_Handler
    .thumb_set NMI_Handler, Default_Handler

    .weak HardFault_Handler
    .thumb_set HardFault_Handler, Default_Handler

    .weak MemManage_Handler
    .thumb_set MemManage_Handler, Default_Handler

    .weak BusFault_Handler
    .thumb_set BusFault_Handler, Default_Handler

    .weak UsageFault_Handler
    .thumb_set UsageFault_Handler, Default_Handler

    .weak SVC_Handler
    .thumb_set SVC_Handler, Default_Handler

    .weak DebugMon_Handler
    .thumb_set DebugMon_Handler, Default_Handler

    .weak PendSV_Handler
    .thumb_set PendSV_Handler, Default_Handler

    .weak SysTick_Handler
    .thumb_set SysTick_Handler, Default_Handler

    .weak USART1_IRQHandler
    .thumb_set USART1_IRQHandler, Default_Handler

    .weak EXTI0_IRQHandler
    .thumb_set EXTI0_IRQHandler, Default_Handler

    .weak EXTI2_IRQHandler
    .thumb_set EXTI2_IRQHandler, Default_Handler

    .weak EXTI3_IRQHandler
    .thumb_set EXTI3_IRQHandler, Default_Handler

    .weak EXTI4_IRQHandler
    .thumb_set EXTI4_IRQHandler, Default_Handler

    .weak EXTI15_10_IRQHandler
    .thumb_set EXTI15_10_IRQHandler, Default_Handler
