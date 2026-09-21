/**
 * footswitch_hid - 三路无源脚踏开关 -> USB HID 键盘 (STM32F103C8T6)
 *
 * 接线: 三个开关一端 -> PB9/PB8/PB7 (P4 排针 4/5/6), 公共端 -> GND (P4-2)。
 *       内部上拉, 按下为低电平。踩下 = 发送 HID 键码 1/2/3, 松开 = 释放。
 *
 * 本文件负责: 72MHz 时钟、GPIO、1ms SysTick、积分消抖、8 字节 boot keyboard
 * 报告组装、LED 状态指示。USB 协议栈在 app/ 中 (ST 官方 USB FS 设备库)。
 */

#include <stdint.h>

#include "config.h"
#include "cmd.h"
#include "cfg.h"
#include "usb_app.h"

/* ===== 临界区 (与 CMSIS __disable_irq()/__enable_irq() 等价的 CPS 指令) ===== */
#define __disable_irq()  __asm__ __volatile__ ("cpsid i" ::: "memory")
#define __enable_irq()   __asm__ __volatile__ ("cpsie i" ::: "memory")

/* ===== Cortex-M3 SysTick ===== */
#define SYSTICK_CTRL    (*(volatile uint32_t *)0xE000E010)
#define SYSTICK_LOAD    (*(volatile uint32_t *)0xE000E014)
#define SYSTICK_VAL     (*(volatile uint32_t *)0xE000E018)

/* ===== RCC / FLASH ===== */
#define RCC_CR          (*(volatile uint32_t *)0x40021000)
#define RCC_CFGR        (*(volatile uint32_t *)0x40021004)
#define RCC_APB2ENR     (*(volatile uint32_t *)0x40021018)
#define FLASH_ACR       (*(volatile uint32_t *)0x40022000)

#define RCC_CR_HSEON    (1u << 16)
#define RCC_CR_HSERDY   (1u << 17)
#define RCC_CR_PLLON    (1u << 24)
#define RCC_CR_PLLRDY   (1u << 25)

/* PLLSRC=HSE, PLLMUL=x9, APB1=/2; bit22 (USBPRE) 保持 0 => USB = PLL/1.5 = 48MHz */
#define RCC_CFGR_PLLSRC_HSE (1u << 16)
#define RCC_CFGR_PLLMUL9    (0x7u << 18)
#define RCC_CFGR_PPRE1_DIV2 (0x4u << 8)
#define RCC_CFGR_SW_PLL     (0x2u)

/* ===== GPIO ===== */
#define RCC_APB2ENR_IOPBEN (1u << 3)
#define RCC_APB2ENR_IOPCEN (1u << 4)

#define GPIOB_CRL       (*(volatile uint32_t *)0x40010C00)
#define GPIOB_CRH       (*(volatile uint32_t *)0x40010C04)
#define GPIOB_IDR       (*(volatile uint32_t *)0x40010C08)
#define GPIOB_ODR       (*(volatile uint32_t *)0x40010C0C)
#define GPIOC_CRH       (*(volatile uint32_t *)0x40011004)
#define GPIOC_ODR       (*(volatile uint32_t *)0x4001100C)

#define GPIO_OUT_PP_2MHZ    0x2u    /* CNF=00 MODE=10 */
#define GPIO_IN_PUPD        0x8u    /* CNF=10 MODE=00 (方向由 ODR 决定) */

/* ===== SWD 可观测变量 ===== */
volatile uint32_t heartbeat;              /* 主循环每拍 +1 */
volatile uint32_t systick_ms;             /* 开机以来的毫秒数 */
volatile uint8_t  last_report[HID_REPORT_LEN];  /* 最近一次发出的报告 */
volatile uint32_t key_event_count;        /* 消抖后接受的开关变化次数 */
volatile uint8_t  usb_configured_mirror;  /* usb_app_is_configured() 镜像 */

static uint32_t key_flash_until_ms;       /* LED 按键闪亮截止时间 */

/* ===== 开关状态 ===== */
typedef struct
{
    uint8_t pin;        /* GPIOB 引脚号 */
    uint8_t stable;     /* 消抖后的状态: 1 = 按下 */
    uint8_t counter;    /* 积分计数 */
} sw_state_t;

static sw_state_t sw[3];

static void sw_init(void)
{
    sw[0].pin = SW1_GPIO_PIN;
    sw[1].pin = SW2_GPIO_PIN;
    sw[2].pin = SW3_GPIO_PIN;

    for (uint8_t i = 0; i < 3u; i++)
    {
        sw[i].stable = 0u;
        sw[i].counter = 0u;
    }
}

/* 读取引脚并折算为 "按下" 逻辑值 */
static uint8_t sw_pressed(uint8_t pin)
{
    uint8_t bit = (uint8_t)((GPIOB_IDR >> pin) & 1u);
#if SW_ACTIVE_LOW
    return (bit == 0u) ? 1u : 0u;
#else
    return bit;
#endif
}

/* ===== 时钟: HSE 8MHz -> PLL x9 -> 72MHz ===== */
static void clock_init(void)
{
    uint32_t timeout;

    RCC_CR |= RCC_CR_HSEON;
    timeout = HSE_STARTUP_TIMEOUT;
    while (((RCC_CR & RCC_CR_HSERDY) == 0u) && (--timeout != 0u))
    {
    }

    RCC_CFGR = RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLMUL9 | RCC_CFGR_PPRE1_DIV2;

    RCC_CR |= RCC_CR_PLLON;
    timeout = PLL_STARTUP_TIMEOUT;
    while (((RCC_CR & RCC_CR_PLLRDY) == 0u) && (--timeout != 0u))
    {
    }

    /* 切到 72MHz 前必须设置 Flash 等待周期, 否则取指失败死机 */
    FLASH_ACR = (1u << 4) | 0x2u;   /* PRFTBE + LATENCY=2 */

    RCC_CFGR |= RCC_CFGR_SW_PLL;
    timeout = PLL_STARTUP_TIMEOUT;
    while (((RCC_CFGR & 0x3u) != RCC_CFGR_SW_PLL) && (--timeout != 0u))
    {
    }
}

/* ===== GPIO: PC13 LED 输出; PB7/8/9 上拉输入 ===== */
static void gpio_init(void)
{
    RCC_APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN;

    GPIOC_CRH &= ~(0xFu << 20);                       /* PC13 */
    GPIOC_CRH |= ((uint32_t)GPIO_OUT_PP_2MHZ << 20);
    GPIOC_ODR |= (1u << 13);                          /* LED 灭 (高) */

    GPIOB_CRL &= ~(0xFu << 28);                       /* PB7 */
    GPIOB_CRL |= ((uint32_t)GPIO_IN_PUPD << 28);
    GPIOB_CRH &= ~(0xFFu << 0);                       /* PB8, PB9 */
    GPIOB_CRH |= ((uint32_t)GPIO_IN_PUPD << 0) | ((uint32_t)GPIO_IN_PUPD << 4);
    GPIOB_ODR |= (1u << 7) | (1u << 8) | (1u << 9);   /* 选择上拉 */
}

static void systick_init(uint32_t ticks)
{
    SYSTICK_LOAD = ticks - 1u;
    SYSTICK_VAL = 0u;
    SYSTICK_CTRL = (1u << 2) | (1u << 1) | (1u << 0); /* 处理器时钟 + 中断 + 使能 */
}

void SysTick_Handler(void)
{
    systick_ms++;
}

/* ===== 报告比较/保存 (避免 volatile 指针转换) ===== */
static uint8_t report_changed(const uint8_t *report)
{
    for (uint8_t i = 0; i < HID_REPORT_LEN; i++)
    {
        if (report[i] != last_report[i])
        {
            return 1u;
        }
    }
    return 0u;
}

static void report_store(const uint8_t *report)
{
    for (uint8_t i = 0; i < HID_REPORT_LEN; i++)
    {
        last_report[i] = report[i];
    }
}

int main(void)
{
    uint32_t last_scan = 0u;
    uint8_t send_pending = 0u;

    clock_init();
    gpio_init();
    systick_init(72000u);      /* 72MHz / 72000 = 1ms */
    sw_init();
    cmd_init();                /* 默认 0x1E/0x1F/0x20 */
    (void)cfg_load();          /* 成功时经 cmd_set_keymap() 覆盖默认值 */
    usb_app_init();            /* 断开脉冲 + USB 协议栈启动 */

    for (;;)
    {
        /* --- SAVE 服务: 擦写 flash 会阻塞取指 ~20-40ms, 只能在主循环执行 --- */
        if (feature_save_pending != 0u)
        {
            uint8_t km_save[CMD_KEY_COUNT];
            uint8_t km_changed = 0u;

            feature_save_pending = 0u;

            /* Snapshot before the ~20-40 ms flash write: a SET landing while
            we are erasing must not be marked as saved. */
            __disable_irq();
            for (uint8_t i = 0u; i < CMD_KEY_COUNT; i++)
            {
                km_save[i] = cmd_keycode(i);
            }
            __enable_irq();

            if (cfg_save() == 0u)
            {
                __disable_irq();
                for (uint8_t i = 0u; i < CMD_KEY_COUNT; i++)
                {
                    if (cmd_keycode(i) != km_save[i])
                    {
                        km_changed = 1u;
                    }
                }
                __enable_irq();

                if (km_changed == 0u)
                {
                    cmd_save_done();       /* RAM still equals the flash image */
                }
                usb_feature_publish("OK saved\n");
            }
            else
            {
                usb_feature_publish("ERR flash\n");
            }
        }

        uint32_t now = systick_ms;
        uint8_t report[HID_REPORT_LEN];
        uint8_t idx = 2u;

        /* --- 每 1ms 采样一次 + 积分消抖 (DEBOUNCE_MS 个 tick 稳定才接受) --- */
        if (now != last_scan)
        {
            last_scan = now;

            for (uint8_t i = 0; i < 3u; i++)
            {
                uint8_t pressed = sw_pressed(sw[i].pin);

                if (pressed != sw[i].stable)
                {
                    sw[i].counter++;
                    if (sw[i].counter >= DEBOUNCE_MS)
                    {
                        sw[i].counter = 0u;
                        sw[i].stable = pressed;
                        key_event_count++;
                        if (pressed != 0u)
                        {
                            key_flash_until_ms = now + LED_KEY_FLASH_MS;
                        }
                    }
                }
                else
                {
                    sw[i].counter = 0u;
                }
            }
        }

        /* --- 快照运行时键表: SET 命令可能在 USB 中断里改写 --- */
        uint8_t km[CMD_KEY_COUNT];
        __disable_irq();
        for (uint8_t i = 0; i < CMD_KEY_COUNT; i++)
        {
            km[i] = cmd_keymap[i];
        }
        __enable_irq();

        /* --- 组装 8 字节 boot keyboard 报告 (键码来自运行时键表) --- */
        for (uint8_t i = 0; i < HID_REPORT_LEN; i++)
        {
            report[i] = 0u;
        }
        for (uint8_t i = 0; i < 3u; i++)
        {
            if (sw[i].stable != 0u)
            {
                uint8_t code = km[i];

                if (code == 0x00u)
                {
                    continue;      /* 禁用的踏板不发键 */
                }
                if ((code >= 0xE0u) && (code <= 0xE7u))
                {
                    report[0] |= (uint8_t)(1u << (code - 0xE0u));   /* 修饰键 -> byte 0 */
                }
                else if (idx < HID_REPORT_LEN)
                {
                    report[idx] = code;                            /* 普通键 -> 6 键数组 */
                    idx++;
                }
            }
        }

        /* --- 变化时发送; 若 EP1 忙则下一拍重试, 不丢状态 --- */
        if (report_changed(report) != 0u)
        {
            report_store(report);
            send_pending = 1u;
        }
        if ((send_pending != 0u) && (usb_app_send_report(report) != 0u))
        {
            send_pending = 0u;
        }

        /* --- LED: 未枚举 -> 500ms 慢闪; 已枚举 -> 按键时闪 100ms --- */
        usb_configured_mirror = usb_app_is_configured();
        if (usb_configured_mirror == 0u)
        {
            if ((now % LED_BLINK_MS) < (LED_BLINK_MS / 2u))
            {
                GPIOC_ODR &= ~(1u << LED_GPIO_PIN);   /* 亮 */
            }
            else
            {
                GPIOC_ODR |= (1u << LED_GPIO_PIN);    /* 灭 */
            }
        }
        else
        {
            if ((int32_t)(key_flash_until_ms - now) > 0)
            {
                GPIOC_ODR &= ~(1u << LED_GPIO_PIN);   /* 亮 */
            }
            else
            {
                GPIOC_ODR |= (1u << LED_GPIO_PIN);    /* 灭 */
            }
        }

        heartbeat++;
    }
}
