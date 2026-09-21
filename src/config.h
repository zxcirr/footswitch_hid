/**
 * footswitch_hid - 板级配置
 *
 * 硬件接线 (Blue Pill STM32F103C8T6):
 *   3 个脚踏开关一端分别接 P4 排针 4/5/6 脚 (PB9/PB8/PB7),
 *   另一端公共端接 P4 排针第 2 脚 (GND)。
 *   按下 = 引脚接地 = 低电平 (ACTIVE_LOW), 使用内部上拉, 无需外部电阻。
 *
 *   P4:  [1] 3V3   [2] GND(公共端)   [3] PB6
 *        [4] PB9=SW1   [5] PB8=SW2   [6] PB7=SW3
 *
 * 逻辑:
 *   每 1ms 采样三个开关, 积分消抖 (DEBOUNCE_MS), 稳定后按 SW1..SW3 顺序
 *   填入 8 字节 HID boot keyboard 报告, 内容变化时通过 usb_app 发出。
 *   KEY_MODE_HOLD=1: 按住发键, 松开发空报告 (hold-while-pressed)。
 */

#ifndef FOOTSWITCH_HID_CONFIG_H
#define FOOTSWITCH_HID_CONFIG_H

/* ============================================================
 * GPIO 引脚号 (STM32F1 风格命名, 供引脚宏展开为数字使用)
 * ============================================================ */
#define GPIO_PIN_7              7
#define GPIO_PIN_8              8
#define GPIO_PIN_9              9

/* ============================================================
 * 开关接线: SW1/SW2/SW3 -> PB9/PB8/PB7 (P4 排针 4/5/6 脚)
 * 公共端 = P4 第 2 脚 GND; ACTIVE_LOW; 内部上拉
 * ============================================================ */
#define SW1_GPIO_PIN            GPIO_PIN_9      /* SW1 = PB9, P4-4 */
#define SW2_GPIO_PIN            GPIO_PIN_8      /* SW2 = PB8, P4-5 */
#define SW3_GPIO_PIN            GPIO_PIN_7      /* SW3 = PB7, P4-6 */

#define SW_ACTIVE_LOW           1               /* 按下 = 低电平 */
#define SW_USE_INTERNAL_PULLUP  1               /* 使能内部上拉 */

/* 板上 LED: PC13, 低电平点亮 */
#define LED_GPIO_PIN            13              /* PC13 */
#define LED_ACTIVE_LOW          1

/* ============================================================
 * HID boot keyboard 键码 (HID Usage Tables, Keyboard/Keypad page 0x07)
 * ============================================================ */
#define KEY1_HID_CODE           0x1E            /* '1' */
#define KEY2_HID_CODE           0x1F            /* '2' */
#define KEY3_HID_CODE           0x20            /* '3' */

/* HID 报告长度: modifiers + reserved + 6 keys */
#define HID_REPORT_LEN          8

/* ============================================================
 * 按键模式
 *   KEY_MODE_HOLD = 1: 按住期间持续发键, 松开发空报告 (默认)
 *   KEY_MODE_TAP  = 1: 每次按下只发一次单击 (备选, 当前不启用)
 * ============================================================ */
#define KEY_MODE_HOLD           1
#define KEY_MODE_TAP            0               /* 备选模式, 勿与 HOLD 同时使能 */

/* ============================================================
 * 时序参数 (单位: ms, 基于 1ms SysTick)
 * ============================================================ */
#define DEBOUNCE_MS             5               /* 消抖积分阈值 */
#define LED_BLINK_MS            500             /* 未配置 USB: 慢闪周期 */
#define LED_KEY_FLASH_MS        100             /* 按键按下时 LED 点亮时长 */

/* 时钟起振超时 (循环计数, 防止 HSE/PLL 故障时死等) */
#define HSE_STARTUP_TIMEOUT     1000000UL
#define PLL_STARTUP_TIMEOUT     1000000UL

#endif /* FOOTSWITCH_HID_CONFIG_H */
