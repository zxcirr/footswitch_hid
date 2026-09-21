# footswitch_hid — 三路脚踏开关 → USB HID 键盘

STM32F103C8T6(TLS 核心板 / Blue Pill 兼容)固件:三个无源脚踏开关接三个 GPIO,
踩下即通过**板载 USB 口**模拟键盘按键,默认输出 `1` / `2` / `3`。

USB 协议栈使用 **ST 官方 STM32_USB-FS-Device_Lib V4.0.0**(原样放在 `lib/st_usbfs/`),
只改写了应用层(`app/`)与自研源码(`src/`)。`lib/` 内为上游代码,请勿改动。

---

## 1. 接线

| 开关 | 开发板引脚 | P4 排针脚位 | HID 键 |
|---|---|---|---|
| SW1 | PB9 | P4-4 | `1` (0x1E) |
| SW2 | PB8 | P4-5 | `2` (0x1F) |
| SW3 | PB7 | P4-6 | `3` (0x20) |
| 公共端 | GND | P4-2 | — |

```
P4 排针:  1=3V3   2=GND(三开关公共端)   3=PB6
          4=PB9(SW1)   5=PB8(SW2)   6=PB7(SW3)
```

- 开关一端接上表引脚,另一端接 **P4-2 (GND)**。
- 使用**内部上拉**,按下 = 引脚拉低;无需外部电阻。
- 线缆较长或环境干扰大时,建议每个开关就近加 10kΩ 上拉到 3V3 + 100nF 到 GND。

**板载 USB**:micro-USB 口 = PA11(D−)/ PA12(D+),D+ 板上已有 4.7k 上拉到 5V
(不可软件断开)。固件启动时会把 PA11/PA12 拉低约 100ms 制造 SE0 断开脉冲,
保证主机在每次复位后都能重新枚举(否则硬上拉会让主机以为设备一直在)。

## 2. 编译与烧录

```bash
cd /home/z/STM32/footswitch_hid
make                 # 生成 build/footswitch_hid.{elf,hex,bin}
make flash           # pyocd flash -t stm32f103c8 -e sector build/footswitch_hid.bin
make test            # 主机侧描述符校验(python3 host_test/test_descriptors.py)
make clean
```

烧录走 SWD(DAPLink),与 USB 口互不影响,可同时连接。

## 3. 上机验证

1. **插上开发板自己的 micro-USB** 到电脑(虚拟机需把该 USB 设备直通给 VM):
   ```bash
   lsusb | grep 1209            # 期望: ID 1209:0001
   dmesg | tail -20             # 期望: "new full-speed USB device" + "input: ... Keyboard"
   ```
2. **抓按键事件**(evtest 未安装时用自带脚本):
   ```bash
   python3 host_test/evdev_read.py --find
   python3 host_test/evdev_read.py --duration 10            # 踩开关,实时打印 KEY_1/2/3
   python3 host_test/evdev_read.py --duration 5 --expect-none   # 静置 5s 应无任何按键
   python3 host_test/evdev_read.py --duration 10 --expect-pairs 10   # 快速点 10 次
   ```
3. **权限**:`/dev/input/eventN` 默认是 `root:input 660`,普通用户读不了,脚本会报 Permission denied。
   ```bash
   sudo usermod -aG input $USER      # 一次性;之后重新登录生效
   # 临时(不改组,立即可用):
   sudo chmod 666 /dev/input/eventN
   ```
   一键端到端验收(SWD 注入踩键 + evdev 收键,自动跑 S1–S5):
   ```bash
   python3 host_test/qa_e2e.py
   # 若还没加 input 组, 本次可直接: sg input -c 'python3 host_test/qa_e2e.py'
   ```
   `dmesg` 受限时脚本会自动改用 sysfs 的 USB 描述符作为枚举证据。
4. 也可以在任意文本框里直接踩开关,应输入 1 / 2 / 3。

## 4. 状态指示(LED = PC13,低电平点亮)

| 现象 | 含义 |
|---|---|
| 500ms 慢闪 | USB **未**枚举成功(未插 USB / 主机未识别) |
| 常灭 + 按键时闪 100ms | USB 已枚举,键盘可用 |

## 5. 改键 / 改引脚

全部集中在 `config.h`:

```c
#define SW1_GPIO_PIN   GPIO_PIN_9    /* PB9 */
#define KEY1_HID_CODE  0x1E          /* '1'; 0x1F='2', 0x20='3', 0x04='a' ... */
#define DEBOUNCE_MS    5             /* 消抖积分阈值 */
```

键码见 HID Usage Tables (Keyboard/Keypad page 0x07)。若同时要按 4 个以上按键,
6KRO boot 报告上限为 6 键。

## 6. 目录结构

```
footswitch_hid/
├── src/                自研源码
│   ├── main.c            应用主循环:时钟/GPIO/1ms SysTick/消抖/报告组装/LED
│   ├── config.h          引脚、键码、时序(改这里即可)
│   ├── startup.s         向量表(含 USB IRQ20/42 弱符号,来自 demo/)
│   └── linker.ld         64K Flash / 20K SRAM
├── app/                应用层(由 ST Custom_HID 例程改写为键盘)
│   ├── usb_desc.c/h      设备/配置/HID/报告描述符(63 字节 boot 键盘)
│   ├── usb_prop.c/h      SETUP 处理:标准请求 + HID 类请求(SET_IDLE/GET_REPORT...)
│   ├── usb_endp.c        EP1 IN 完成回调
│   ├── usb_istr.c/h      USB 中断分发 + IRQ20/42 入口
│   ├── usb_pwr.c/h       上电/挂起管理(已禁用 STOP 低功耗,见 §7)
│   ├── usb_conf.h        端点与缓冲区配置(EP0 + EP1 IN)
│   ├── usb_app.c/h       应用接口:usb_app_init / send_report / is_configured
│   ├── hw_config.c/h     本板适配:USB 时钟、NVIC、PA11/PA12、断开脉冲
│   └── platform_config.h CMSIS 头选择(STM32F10X_MD)
├── lib/                第三方代码(原样引入,勿改)
│   ├── st_usbfs/         ST 官方 USB FS 设备库 V4.0.0(inc/ + src/)
│   └── cmsis/            stm32f10x.h / core_cm3 / system_stm32f10x
├── host_test/          主机侧测试:test_descriptors.py / evdev_read.py / qa_e2e.py
├── Makefile            构建与烧录(make / make flash / make test)
├── README.md
└── .gitignore
```

## 7. 移植要点(排错参考)

- **USB 时钟**:HSE 8MHz → PLL×9 = 72MHz,`RCC_CFGR.USBPRE=0` → USB = PLL/1.5 = 48MHz。
- **挂起处理**:`app/usb_pwr.c` 中 `fSuspendEnabled = FALSE`。若为 TRUE,板上无主机时
  USB 空闲 3ms 即触发 SUSP,库函数 `Suspend()` 会让 MCU 进入 **STOP 模式**(表现为
  SWD 读外设全 0、心跳停止)。键盘需常驻响应,故禁用。
- **端点**:只有 EP0(控制,64B)+ EP1(中断 IN,8B,`bInterval=1ms`)。
- **报告只在变化时发送**;`SET_IDLE` 直接 ACK,`GET_REPORT` 返回当前报告。
- **无字符串描述符**(设备描述符 iManufacturer/iProduct/iSerialNumber = 0),枚举最简。
- VID:PID = `0x1209:0x0001`(pid.codes 开源 VID),可按需修改 `app/usb_desc.c`。

## 8. 已验证 / 待验证

- [x] 编译零警告;Flash 占用约 5.7KB / 20KB SRAM 约 2.2KB
- [x] 主机侧描述符校验 4/4 通过(`make test`)
- [x] SWD 实测:72MHz、USBEN、EP0R=0x3210、DADDR=0x80、NVIC IRQ20
- [x] SWD 注入模拟踩键:SW1/2/3 → 0x1E/0x1F/0x20,组合键同帧,消抖无重复
- [ ] 真机 USB 枚举 + 真实开关按键(需插上板载 USB 并接好开关)

---

## 9. 物理链路排查(插上 USB 但主机无反应时)

日志/`lsusb` 看不到设备时,用 SWD 读这几个寄存器即可区分"固件问题"和"线材/接线问题":

```bash
cd /home/z/STM32/footswitch_hid
../venv/bin/pyocd cmd -t cortex_m -c "halt" \
  -c "read32 0x40005C48"   # USB_FNR: bit15=RXDP(D+电平), bit10:0=帧号
  -c "read32 0x40005C44"   # USB_ISTR: RESET(0x400)/SUSP(0x800)/ESOF(0x100)
  -c "read32 0x40005C4C"   # USB_DADDR: 0x80 = 还没被主机分配地址
  -c "read32 0x40010808"   # GPIOA_IDR: bit11=PA11(D-), bit12=PA12(D+)
  -c exit
../venv/bin/pyocd reset -t cortex_m
```

| 现象 | 判读 | 处理 |
|---|---|---|
| 帧号恒为 0、无 RESET、DADDR=0x80 | **主机完全没在驱动总线** | 换一根**能传数据**的 USB 线;确认插的是**开发板自己的 micro-USB 口**;VMware 需把该 USB 设备直通给虚拟机 |
| D+ = 1 但 D− = 1(高) | 主机侧 15k 下拉不存在 → D−/D+ 没接到主机 | 典型"充电线"(只有电源、无数据线) |
| D+ = 0 | VBUS 没来(或 4.7k 上拉/焊接异常) | 换口/换线,检查 USB 座 |
| 出现 RESET、帧号跳动、DADDR 变化 | 主机在枚举 | 若仍失败,看 `dmesg` 具体错误(如 error -71 多为时序/上拉问题) |

> 注意:板上 USB **未接**时,本固件会一直处于"待命"状态(不进入 STOP,见 §7),因此 SWD 诊断随时可用。

