/*
 * STM32F103C8T6 链接脚本
 * Flash: 64KB @ 0x08000000
 * SRAM:  20KB @ 0x20000000
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

SECTIONS
{
    /* ===== 中断向量表 - 必须在 Flash 最前面 ===== */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* ===== 代码段 ===== */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* ===== 已初始化数据段 ===== */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT> FLASH

    /* ===== 未初始化数据段 ===== */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    /* ===== 栈空间 ===== */
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        . = . + 2K;      /* 栈大小 2KB */
        _estack = .;      /* 栈顶地址 */
    } > SRAM
}
