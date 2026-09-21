/**
 * STM32F103C8T6 启动文件 (startup.s)
 *
 * 这是芯片上电后执行的第一段代码。
 * 执行流程:
 *   1. Cortex-M3 从 Flash 起始地址 (0x08000000) 读取初始 SP 值
 *   2. 从 0x08000004 读取 Reset_Handler 地址并跳转
 *   3. Reset_Handler 初始化 .data 和 .bss 段
 *   4. 跳转到 main()
 */

    .syntax unified
    .cpu cortex-m3
    .thumb

/* ===== 中断向量表 ===== */
    .section .isr_vector, "a", %progbits
    .type g_pfnVectors, %object
    .size g_pfnVectors, .-g_pfnVectors

g_pfnVectors:
    /* Cortex-M3 核心异常 */
    .word _estack              /* 0x00: 初始栈指针 (由链接脚本计算) */
    .word Reset_Handler        /* 0x04: 复位处理函数 */
    .word NMI_Handler          /* 0x08: NMI */
    .word HardFault_Handler    /* 0x0C: 硬件错误 */
    .word MemManage_Handler    /* 0x10: 内存管理错误 */
    .word BusFault_Handler     /* 0x14: 总线错误 */
    .word UsageFault_Handler   /* 0x18: 用法错误 */
    .word 0                    /* 0x1C: 保留 */
    .word 0                    /* 0x20: 保留 */
    .word 0                    /* 0x24: 保留 */
    .word 0                    /* 0x28: 保留 */
    .word SVC_Handler          /* 0x2C: SVCall */
    .word DebugMon_Handler     /* 0x30: 调试监视器 */
    .word 0                    /* 0x34: 保留 */
    .word PendSV_Handler       /* 0x38: PendSV */
    .word SysTick_Handler      /* 0x3C: SysTick 定时器 */

    /* STM32F103 外部中断 (IRQ 0-67) */
    .word WWDG_IRQHandler            /* IRQ 0 */
    .word PVD_IRQHandler             /* IRQ 1 */
    .word TAMPER_IRQHandler          /* IRQ 2 */
    .word RTC_IRQHandler             /* IRQ 3 */
    .word FLASH_IRQHandler           /* IRQ 4 */
    .word RCC_IRQHandler             /* IRQ 5 */
    .word EXTI0_IRQHandler           /* IRQ 6 */
    .word EXTI1_IRQHandler           /* IRQ 7 */
    .word EXTI2_IRQHandler           /* IRQ 8 */
    .word EXTI3_IRQHandler           /* IRQ 9 */
    .word EXTI4_IRQHandler           /* IRQ 10 */
    .word DMA1_Channel1_IRQHandler   /* IRQ 11 */
    .word DMA1_Channel2_IRQHandler   /* IRQ 12 */
    .word DMA1_Channel3_IRQHandler   /* IRQ 13 */
    .word DMA1_Channel4_IRQHandler   /* IRQ 14 */
    .word DMA1_Channel5_IRQHandler   /* IRQ 15 */
    .word DMA1_Channel6_IRQHandler   /* IRQ 16 */
    .word DMA1_Channel7_IRQHandler   /* IRQ 17 */
    .word ADC1_2_IRQHandler          /* IRQ 18 */
    .word USB_HP_CAN1_TX_IRQHandler  /* IRQ 19 */
    .word USB_LP_CAN1_RX0_IRQHandler /* IRQ 20 */
    .word CAN1_RX1_IRQHandler        /* IRQ 21 */
    .word CAN1_SCE_IRQHandler        /* IRQ 22 */
    .word EXTI9_5_IRQHandler         /* IRQ 23 */
    .word TIM1_BRK_IRQHandler        /* IRQ 24 */
    .word TIM1_UP_IRQHandler         /* IRQ 25 */
    .word TIM1_TRG_COM_IRQHandler    /* IRQ 26 */
    .word TIM1_CC_IRQHandler         /* IRQ 27 */
    .word TIM2_IRQHandler            /* IRQ 28 */
    .word TIM3_IRQHandler            /* IRQ 29 */
    .word TIM4_IRQHandler            /* IRQ 30 */
    .word I2C1_EV_IRQHandler         /* IRQ 31 */
    .word I2C1_ER_IRQHandler         /* IRQ 32 */
    .word I2C2_EV_IRQHandler         /* IRQ 33 */
    .word I2C2_ER_IRQHandler         /* IRQ 34 */
    .word SPI1_IRQHandler            /* IRQ 35 */
    .word SPI2_IRQHandler            /* IRQ 36 */
    .word USART1_IRQHandler          /* IRQ 37 */
    .word USART2_IRQHandler          /* IRQ 38 */
    .word USART3_IRQHandler          /* IRQ 39 */
    .word EXTI15_10_IRQHandler       /* IRQ 40 */
    .word RTCAlarm_IRQHandler        /* IRQ 41 */
    .word USBWakeUp_IRQHandler       /* IRQ 42 */

/* ===== 复位处理函数 ===== */
    .section .text.Reset_Handler
    .weak Reset_Handler
    .type Reset_Handler, %function

Reset_Handler:
    /*
     * 步骤 1: 初始化 .data 段
     * 将 Flash 中的初始值复制到 SRAM
     */
    ldr r0, =_sdata      /* r0 = .data 段起始地址 (SRAM) */
    ldr r1, =_edata      /* r1 = .data 段结束地址 (SRAM) */
    ldr r2, =_sidata      /* r2 = .data 段初始值地址 (Flash) */
    movs r3, #0
    b .Ldata_loop_cond

.Ldata_loop:
    ldr r4, [r2, r3]     /* 从 Flash 读取 */
    str r4, [r0, r3]     /* 写入 SRAM */
    adds r3, r3, #4

.Ldata_loop_cond:
    adds r4, r0, r3      /* r4 = 当前目标地址 */
    cmp r4, r1           /* 是否到达结束地址? */
    bcc .Ldata_loop      /* 如果没有，继续复制 */

    /*
     * 步骤 2: 初始化 .bss 段
     * 将未初始化数据清零
     */
    ldr r0, =_sbss       /* r0 = .bss 段起始地址 */
    ldr r1, =_ebss       /* r1 = .bss 段结束地址 */
    movs r2, #0
    b .Lbss_loop_cond

.Lbss_loop:
    str r2, [r0]         /* 写入 0 */
    adds r0, r0, #4

.Lbss_loop_cond:
    cmp r0, r1
    bcc .Lbss_loop

    /*
     * 步骤 3: 跳转到 main()
     */
    bl main

    /*
     * 如果 main() 返回，进入死循环
     */
.Lhalt:
    b .Lhalt

    .size Reset_Handler, .-Reset_Handler

/* ===== 默认中断处理函数 ===== */
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

    .weak WWDG_IRQHandler
    .thumb_set WWDG_IRQHandler, Default_Handler

    .weak PVD_IRQHandler
    .thumb_set PVD_IRQHandler, Default_Handler

    .weak TAMPER_IRQHandler
    .thumb_set TAMPER_IRQHandler, Default_Handler

    .weak RTC_IRQHandler
    .thumb_set RTC_IRQHandler, Default_Handler

    .weak FLASH_IRQHandler
    .thumb_set FLASH_IRQHandler, Default_Handler

    .weak RCC_IRQHandler
    .thumb_set RCC_IRQHandler, Default_Handler

    .weak EXTI0_IRQHandler
    .thumb_set EXTI0_IRQHandler, Default_Handler

    .weak EXTI1_IRQHandler
    .thumb_set EXTI1_IRQHandler, Default_Handler

    .weak EXTI2_IRQHandler
    .thumb_set EXTI2_IRQHandler, Default_Handler

    .weak EXTI3_IRQHandler
    .thumb_set EXTI3_IRQHandler, Default_Handler

    .weak EXTI4_IRQHandler
    .thumb_set EXTI4_IRQHandler, Default_Handler

    .weak DMA1_Channel1_IRQHandler
    .thumb_set DMA1_Channel1_IRQHandler, Default_Handler

    .weak DMA1_Channel2_IRQHandler
    .thumb_set DMA1_Channel2_IRQHandler, Default_Handler

    .weak DMA1_Channel3_IRQHandler
    .thumb_set DMA1_Channel3_IRQHandler, Default_Handler

    .weak DMA1_Channel4_IRQHandler
    .thumb_set DMA1_Channel4_IRQHandler, Default_Handler

    .weak DMA1_Channel5_IRQHandler
    .thumb_set DMA1_Channel5_IRQHandler, Default_Handler

    .weak DMA1_Channel6_IRQHandler
    .thumb_set DMA1_Channel6_IRQHandler, Default_Handler

    .weak DMA1_Channel7_IRQHandler
    .thumb_set DMA1_Channel7_IRQHandler, Default_Handler

    .weak ADC1_2_IRQHandler
    .thumb_set ADC1_2_IRQHandler, Default_Handler

    .weak USB_HP_CAN1_TX_IRQHandler
    .thumb_set USB_HP_CAN1_TX_IRQHandler, Default_Handler

    .weak USB_LP_CAN1_RX0_IRQHandler
    .thumb_set USB_LP_CAN1_RX0_IRQHandler, Default_Handler

    .weak CAN1_RX1_IRQHandler
    .thumb_set CAN1_RX1_IRQHandler, Default_Handler

    .weak CAN1_SCE_IRQHandler
    .thumb_set CAN1_SCE_IRQHandler, Default_Handler

    .weak EXTI9_5_IRQHandler
    .thumb_set EXTI9_5_IRQHandler, Default_Handler

    .weak TIM1_BRK_IRQHandler
    .thumb_set TIM1_BRK_IRQHandler, Default_Handler

    .weak TIM1_UP_IRQHandler
    .thumb_set TIM1_UP_IRQHandler, Default_Handler

    .weak TIM1_TRG_COM_IRQHandler
    .thumb_set TIM1_TRG_COM_IRQHandler, Default_Handler

    .weak TIM1_CC_IRQHandler
    .thumb_set TIM1_CC_IRQHandler, Default_Handler

    .weak TIM2_IRQHandler
    .thumb_set TIM2_IRQHandler, Default_Handler

    .weak TIM3_IRQHandler
    .thumb_set TIM3_IRQHandler, Default_Handler

    .weak TIM4_IRQHandler
    .thumb_set TIM4_IRQHandler, Default_Handler

    .weak I2C1_EV_IRQHandler
    .thumb_set I2C1_EV_IRQHandler, Default_Handler

    .weak I2C1_ER_IRQHandler
    .thumb_set I2C1_ER_IRQHandler, Default_Handler

    .weak I2C2_EV_IRQHandler
    .thumb_set I2C2_EV_IRQHandler, Default_Handler

    .weak I2C2_ER_IRQHandler
    .thumb_set I2C2_ER_IRQHandler, Default_Handler

    .weak SPI1_IRQHandler
    .thumb_set SPI1_IRQHandler, Default_Handler

    .weak SPI2_IRQHandler
    .thumb_set SPI2_IRQHandler, Default_Handler

    .weak USART1_IRQHandler
    .thumb_set USART1_IRQHandler, Default_Handler

    .weak USART2_IRQHandler
    .thumb_set USART2_IRQHandler, Default_Handler

    .weak USART3_IRQHandler
    .thumb_set USART3_IRQHandler, Default_Handler

    .weak EXTI15_10_IRQHandler
    .thumb_set EXTI15_10_IRQHandler, Default_Handler

    .weak RTCAlarm_IRQHandler
    .thumb_set RTCAlarm_IRQHandler, Default_Handler

    .weak USBWakeUp_IRQHandler
    .thumb_set USBWakeUp_IRQHandler, Default_Handler

/* ===== 默认中断处理函数 (死循环) ===== */
    .section .text.Default_Handler
    .weak Default_Handler
    .type Default_Handler, %function

Default_Handler:
    b Default_Handler    /* 死循环，便于调试器捕获 */

    .size Default_Handler, .-Default_Handler

/* ===== 栈顶地址 (由链接脚本计算) ===== */
    .section .stack
    .word _estack
