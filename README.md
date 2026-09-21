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

## 5.5 在线改键(HID Feature 报告命令通道)

不重新编译也能改键:主机通过**一个 HID Feature 报告**(48 字节)读写运行时键位表,
内容是 ASCII 命令行。该通道与 boot 键盘**共用同一个 USB 接口**,无 report ID、
不增加端点。报告描述符在键盘集合之后追加了一个厂商集合(Usage Page `0xFF00`、
Usage `0x01`、Feature 8 bits × 48),因此描述符从 63 字节变为 **82 字节**,
HID 描述符里的 `wDescriptorLength = 0x52`;键盘的 8 字节 Input 报告与
1 字节 LED Output 报告均不变。

| 方向 | `bmRequestType` | `bRequest` | `wValue` | `wIndex` | `wLength` |
|---|---|---|---|---|---|
| Get_Report | `0xA1` | `0x01` | `0x0300` | 0 | 48 |
| Set_Report | `0x21` | `0x09` | `0x0300` | 0 | 48 |

`wValue` 高字节 3 = Feature,低字节 0 = report ID。请求为 ASCII、NUL 填充,
命令字大小写不敏感,允许开头多一个 `0x00`、结尾带 `\n`;响应为 48 字节 ASCII、
以 `\n` 结尾。

**缓冲与长度约定(49 = report ID + 48)**:主机侧 hidapi/hidraw 的 Feature 缓冲首字节
是 report ID(本设备为 `0x00`),所以工具实际收发按 **49 字节**处理:首字节 `0x00`
+ 48 字节负载。负载必须 NUL 填充,命令文本最多 **47 个字符**——设备端会把收到的
长度截断为 48 字节,文本过长会丢尾部。`GET_REPORT(Feature)` 返回完整的 48 字节
报告(文本后 NUL 填充),主机侧剥掉首字节 report ID 后即得到 ASCII 文本。

| 命令 | 响应 |
|---|---|
| `SET <1-3> <0xNN>` | `OK SET <n>=0xNN\n` |
| `GET` | `OK MAP 0xNN 0xNN 0xNN\n` |
| `SAVE` | 先 `OK saving\n`,Flash 写完后为 `OK saved\n`(失败 `ERR flash\n`) |
| `RESET` | `OK RESET\n`(恢复默认 1/2/3,需再 `SAVE` 才持久化) |
| `INFO` | `OK INFO fw=1.0 keys=0xNN,0xNN,0xNN dirty=<0\|1>\n` |
| 其它 | `ERR unknown\n` / `ERR syntax\n` / `ERR pedal\n` / `ERR range\n` |

`SET` 的键码规则:

| 取值 | 含义 |
|---|---|
| `0x00` | 禁用,该踏板不输出任何按键 |
| `0x04..0x65` | 普通键,进入键盘报告的 6 键数组 |
| `0xE0..0xE7` | 修饰键,置位报告 byte 0 |
| 其余 | `ERR range`(含 `0x68..0x73`;描述符声明的 Usage Max 是 `0x65`,这类键发不出去) |

即 `cmd.h` 的完整合法集合为 `0x00 | 0x04..0x65 | 0xE0..0xE7`;`0x68..0x73`(F13–F24)
虽属合法 HID 用法但会被**显式拒绝**(发不出去),其余值返回 `ERR range`。

**持久化**:配置存在 **Flash 最后一页 `0x0800FC00`**,共 16 字节
(magic / version / keymap / checksum)。启动时校验并加载,非法则回落默认值。
`SAVE` 才写 Flash,`RESET` 只改 RAM;`make flash -e sector` **保留**该页,
`make flash -e chip`(或 `pyocd flash -e chip`)**会擦掉**它,配置随之回到默认。

> 报告描述符变了,所以**烧录新固件后需要重新插拔一次 USB**(重新枚举);
> 之后在线改键**不需要**再重新枚举。
> BIOS/UEFI 走 boot 协议时只认键盘 Input 报告,Feature 报告在 boot 协议下未定义,
> 不影响开机打字。

**主机工具 `host_test/pedalctl.py`**(hidapi 优先,缺库时回退 `/dev/hidraw` 原始 ioctl):

```bash
python3 host_test/pedalctl.py set 1 0x2C   # 踏板1 -> 0x2C(需再 save 才持久化)
python3 host_test/pedalctl.py get          # 读当前映射
python3 host_test/pedalctl.py save         # 写入 Flash
python3 host_test/pedalctl.py reset        # 恢复默认
python3 host_test/pedalctl.py info         # 固件/键位/dirty 状态
python3 host_test/pedalctl.py --probe      # 设备是否在线
python3 host_test/pedalctl.py --list       # 列出 HID/hidraw 候选
```

必须打开**厂商集合**(usage page `0xFF00`),不要打开键盘集合:Windows 下键盘
TLC 被 `kbdhid` 独占,Linux 下每个接口对应一个 hidraw 节点;hidapi 缓冲带首字节
report ID,所以实际收发按 49 字节处理。`save` / `get` 会自动轮询等待结果。
成功判定是**严格语义匹配**:`set` 必须收到 `OK SET <n>=0xNN`(与请求一致)、
`reset` 必须收到 `OK RESET`、`get` 必须收到 `OK MAP 0xNN 0xNN 0xNN`、`info`
必须收到 `OK INFO`、`save` 必须收到 `OK saved`;读到旧命令的残留文本不会被当作
成功,而是返回 2 并提示用 `--probe` 复查设备。

Linux 读 `/dev/hidraw*` 需要权限:

```bash
sudo usermod -aG input $USER      # 一次性;之后重新登录生效
```

**macOS** 上厂商集合与键盘同接口,hidapi 默认独占打开,抢占键盘设备需要 root,
三种办法任选:用 PyPI `hidapi` 绑定并先调 `hid_darwin_set_open_exclusive(0)`、
以 root 运行、或改用厂商集合独立接口的固件变体。

## 6. 目录结构

```
footswitch_hid/
├── src/                自研源码
│   ├── main.c            应用主循环:时钟/GPIO/1ms SysTick/消抖/报告组装/LED/Feature 命令与配置加载
│   ├── config.h          引脚、键码、时序(改这里即可)
│   ├── startup.s         向量表(含 USB IRQ20/42 弱符号,来自 demo/)
│   └── linker.ld         64K Flash / 20K SRAM
├── app/                应用层(由 ST Custom_HID 例程改写为键盘)
│   ├── usb_desc.c/h      设备/配置/HID/报告描述符(82 字节:键盘 + 厂商 Feature 集合)
│   ├── usb_prop.c/h      SETUP 处理:标准请求 + HID 类请求(SET_IDLE / GET_REPORT / Feature 命令)
│   ├── cmd.c/h           命令解析 + 运行时键位表(可主机单测,见 §5.5)
│   ├── cfg.h/cfg_flash.c Flash 持久化:最后一页 0x0800FC00 的 16 字节配置
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
├── host_test/          主机侧测试与工具
│   ├── test_descriptors.py 描述符校验(make test,5 项)
│   ├── test_cmd.c          cmd 命令解析主机单测(见 §5.5)
│   ├── pedalctl.py         在线改键工具(见 §5.5)
│   ├── evdev_read.py       按键事件抓取
│   └── qa_e2e.py           端到端验收
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

- [x] 编译零警告;Flash 镜像 ≈ **8044 B**(text 7828 + data 216),RAM bss 2368 B
- [x] 主机侧描述符校验 5/5 通过(`make test`,含 48B vendor Feature 集合)
- [x] `cmd` 主机单测 ALL PASS(`gcc -std=c11 -Wall -Wextra -Wshadow -Iapp host_test/test_cmd.c app/cmd.c`,见 §5.5)
- [x] SWD 实测:72MHz、USBEN、EP0R=0x3210、DADDR=0x82、USB_Configured=1、NVIC IRQ20
- [x] 真机 USB 枚举成功(主机已分配地址:SWD 读到 DADDR=0x82 / USB_Configured=1)
- [x] SWD 注入模拟踩键:SW1/2/3 → 0x1E/0x1F/0x20,组合键同帧,消抖无重复
- [x] SWD 验证的 C1 固件侧证据:运行时键位表驱动键盘报告(含修饰键/禁用行为);延迟 `SAVE` 写入配置页(magic `0x31474643`、keymap `1E 1F 20`、checksum `0x015F`);复位后 `cfg_load()` 恢复正常
- [ ] 真实开关踩键(需接好三个开关)
- [ ] 主机侧 Feature `set`/`get`/`save`(`host_test/pedalctl.py`)与断电持久化 — **待验证**

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

