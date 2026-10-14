# footswitch_hid

> 三路无源脚踏开关 → USB HID 键盘固件(STM32F103C8T6),默认输出 `1` / `2` / `3`,支持主机在线改键、宏序列(踩住踏板循环按键)与掉电保存(当前版本 v0.0.2)。

## 技术栈

- **语言**:C11(裸机,无 RTOS)
- **目标芯片**:STM32F103C8T6(Cortex-M3,64K Flash / 20K SRAM)
- **USB**:ST 官方 STM32_USB-FS-Device_Lib V4.0.0(原样引入)+ HID boot 键盘
- **构建**:GNU Make + Arm GNU Toolchain(`arm-none-eabi-gcc`)
- **烧录/调试**:pyOCD(SWD / DAPLink)
- **主机侧工具**:Python 3(描述符校验、按键抓取、在线改键)

## 项目结构

```text
footswitch_hid/
├── src/          自研源码:程序入口、产品配置、启动与链接
├── app/          USB 应用层:描述符、请求处理、命令解析、Flash 配置、板级适配
├── lib/          第三方代码(原样引入,请勿修改)
│   ├── st_usbfs/   ST USB FS 设备库 V4.0.0
│   └── cmsis/      CMSIS 内核与 STM32F10x 头文件
├── host_test/    主机侧测试(描述符 / C 单测 / 语义单测 / 端到端验收)
├── pedal_panel/  上位机(自包含,可整体拷去 Windows):Tkinter GUI + pedalctl.py CLI + win_remap.py
├── Makefile      构建 / 烧录 / 测试 / 调试入口
└── .gitignore
```

### 目录说明

| 目录/文件 | 说明 |
| --- | --- |
| `src/` | 固件主体。`main.c` 是**程序入口**(时钟、GPIO 采样、消抖、报告组装(单键+宏合并)、LED);`config.h` 集中引脚/键码/时序等产品配置;`startup.s` 向量表;`linker.ld` 内存布局 |
| `app/` | USB 应用层,对外入口为 `usb_app_init()` / `usb_app_send_report()` / `is_configured()` |
| `app/usb_desc.c` | 设备/配置/HID/报告描述符(键盘集合 + 厂商 Feature 集合) |
| `app/usb_prop.c` | 标准请求与 HID 类请求处理,含 Feature 报告命令通道 |
| `app/cmd.c` | ASCII 命令解析 + 每踏板运行时配置(单键绑定 / 宏步骤 / 宏时序),不依赖硬件,可主机单测 |
| `app/macro.c` | 宏播放引擎:踩住踏板按步骤循环按键,松开立即释放;纯逻辑,可主机单测 |
| `app/cfg_flash.c` | 配置页(Flash 最后一页,64 字节 v2 镜像)读写与上电加载,自动迁移 v1 旧镜像 |
| `lib/` | 第三方代码,**不要修改**:`st_usbfs` 为 USB 设备栈,`cmsis` 为内核头文件 |
| `host_test/` | 主机侧测试:描述符校验、cmd/macro C 单测、pedalctl 语义单测、端到端验收(qa_e2e) |
| `pedal_panel/` | 上位机目录,自包含可整体拷贝:图形界面 `pedal_panel.py`(单键/宏/时序/捕获按键/保存 Flash) + CLI 工具 `pedalctl.py` / `win_remap.py`;`--self-test` 纯逻辑自测 |
| `Makefile` | `make` / `make flash` / `make test` / `make debug` / `make clean` |

## 整体框架

- **分层**:`src/`(产品逻辑)→ `app/`(USB 应用层)→ `lib/`(USB 设备栈 + CMSIS);`lib/` 不感知产品逻辑,可整体替换。
- **USB 形态**:以 HID 设备枚举,**两个 USB 接口**(每个接口各自只含一个顶层 Application 集合,均无 report ID):**接口 0 = boot 键盘**(class 3/sub 1/proto 1,自有 63 字节报告描述符,EP1 中断 IN 8B);**接口 1 = 厂商 HID**(class 3/sub 0/proto 0,自有 21 字节报告描述符,usage page 0xFF00 的 48 字节 Feature 通道——Feature 数据项带自己的 Usage 0x01,因为集合的 Usage 会被 Collection 项消耗,Windows 拒绝无 Usage 的数据主项;EP2 中断 IN 8B 永不发送、仅回 NAK)。Windows HidUsb 按**接口**解析 HID:每个接口各自单集合,因此无需 report ID(>1 顶层集合无 report ID、或数据主项缺 Usage,都会被 0xC00000B9 拒绝);厂商集合因此成为独立 HID 设备对象,hidapi/hidraw 可正常打开(无需 `\KBD` 之类 hack);设备版本(bcdDevice)为 **0.05**(改描述符必须同时 bump 此版本,否则 Windows 复用按 REV_xxxx 缓存的旧解析结果)。
- **主循环**:`main.c` 每 1ms 采样 PB9/PB8/PB7(内部上拉、按下为低电平),积分消抖后把按键填入 8 字节键盘报告,仅在内容变化时发送。
- **改键数据流**:主机 `Set_Report(Feature)` → `app/usb_prop.c` 收帧 → `app/cmd.c` 解析并更新 RAM 配置 → 键盘报告立即使用新键码/新宏;`SAVE` 才由主循环写入 Flash。
- **宏播放**:每个踏板可绑定 1..6 步序列(如 `Shift→Z→C`);踩住期间 `app/macro.c` 循环「按住当前步 press 毫秒 → 全部松开 gap 毫秒 → 下一步」(默认各 30ms,可在线配置),**松开踏板立即释放全部按键**(不卡键);宏踏板与单键踏板可同时踩,贡献合并进同一 8 字节报告(修饰键相 OR、键入 6 键槽)。
- **持久化**:上电时 `app/cfg_flash.c` 校验并加载配置页(64 字节 v2 镜像,含单键/宏步骤/时序;v0.0.1 的 16 字节旧镜像自动迁移、保留单键绑定),非法则回落默认值,因此改键可跨重启保留。
- **状态指示**:PC13 慢闪 = USB 未枚举;常灭、按键时短闪 = 键盘可用。

## 快速开始

### 环境要求

- Arm GNU Toolchain(`arm-none-eabi-gcc` 等,需在 `PATH` 中)
- GNU Make
- pyOCD(烧录/调试;本工程示例路径 `../venv/bin/pyocd`)
- Python 3(主机侧工具与测试)
- 硬件:STM32F103C8T6 板、DAPLink(SWD)、三个无源脚踏开关

### 安装

```bash
cd footswitch_hid
make
```

### 配置

固件配置集中在 `src/config.h`,无需环境变量;下表为主要可改项。

| 变量 | 说明 | 默认值 | 是否必填 |
| --- | --- | --- | --- |
| `SW1_GPIO_PIN` / `SW2_GPIO_PIN` / `SW3_GPIO_PIN` | 三个开关的 GPIO | PB9 / PB8 / PB7 | 是(改引脚时) |
| `KEY1_HID_CODE` / `KEY2_HID_CODE` / `KEY3_HID_CODE` | 默认 HID 键码 | `0x1E` / `0x1F` / `0x20`(即 `1` / `2` / `3`) | 否 |
| `DEBOUNCE_MS` | 消抖积分阈值(ms) | `5` | 否 |
| `KEY_MODE_HOLD` / `KEY_MODE_TAP` | 按住发键 / 每次按下只发一次 | `1` / `0` | 否 |

宏相关默认值集中在 `app/cmd.h`:`CMD_MACRO_PRESS_DEFAULT` / `CMD_MACRO_GAP_DEFAULT`(每步按下 / 步间间隔,默认 30 ms)、`CMD_MACRO_MAX_STEPS`(每踏板最大步数 6);运行时可用 `pedalctl.py tim <踏板> <press> <gap>` 在线调整(1..9999 ms),无需重编译。

接线:三个开关一端分别接 PB9 / PB8 / PB7,另一端接 GND(P4 排针第 2 脚);使用内部上拉,无需外部电阻。

### 运行

```bash
make flash     # pyocd flash -t stm32f103c8 -e sector build/footswitch_hid.bin
```

烧录后把开发板 micro-USB 接到主机,在任意文本框踩开关应输入 `1` / `2` / `3`。

### 测试

```bash
make test                                      # 主机侧全部单测: 描述符 + cmd + macro + pedalctl 语义
python3 host_test/evdev_read.py --find          # 定位输入设备节点
python3 host_test/evdev_read.py --duration 10   # 踩开关,实时打印按键
python3 host_test/qa_e2e.py                     # 端到端验收(S6 含宏循环,需要真机)
# 单独运行某一组单测 / run one suite only:
gcc -std=c11 -Wall -Wextra -Wshadow -Iapp host_test/test_cmd.c app/cmd.c -o /tmp/t && /tmp/t
gcc -std=c11 -Wall -Wextra -Wshadow -Iapp host_test/test_macro.c app/macro.c -o /tmp/t && /tmp/t
python3 host_test/test_pedalctl_semantics.py
python3 host_test/test_descriptors.py
python3 pedal_panel/pedal_panel.py --self-test   # 上位机纯逻辑自测(48 项,无需设备/显示)
```

读取 `/dev/input/event*` 与 `/dev/hidraw*` 需要权限:`sudo usermod -aG input $USER`(重新登录后生效)。

### 构建/部署

```bash
make clean && make    # 全量重建,产物在 build/
make debug            # 启动 pyocd gdbserver(GDB 调试)
```

## 常见用法

**1. 在线改键(无需重新编译)**

```bash
python3 pedal_panel/pedalctl.py set 1 0x04   # 踏板 1 → 'a'
python3 pedal_panel/pedalctl.py get          # 读当前映射
python3 pedal_panel/pedalctl.py save         # 写入 Flash,重启后仍生效
```

**2. 恢复默认键位**

```bash
python3 pedal_panel/pedalctl.py reset        # 恢复 1/2/3(仅 RAM)
python3 pedal_panel/pedalctl.py save         # 需要持久化时再执行
```

**3. 查看设备状态**

```bash
python3 pedal_panel/pedalctl.py --probe      # 设备是否在线
python3 pedal_panel/pedalctl.py info         # 固件版本 / 当前键位 / 是否有未保存修改
```

**4. 临时禁用某个踏板**:`python3 pedal_panel/pedalctl.py set 2 0x00`(键码 `0x00` 表示不输出任何按键)。

**5. 宏:踩住踏板循环按键(例如踩住 → `Shift → Z → C` 快速循环)**

```bash
python3 pedal_panel/pedalctl.py mac 1 shift z c    # 三个键依次独立快按,踩住期间循环
python3 pedal_panel/pedalctl.py mac 1 shift+z c    # 组合键步骤:Shift+Z 一起按,然后 C
python3 pedal_panel/pedalctl.py tim 1 15 15        # 每步按下 15ms、间隔 15ms(默认 30/30)
python3 pedal_panel/pedalctl.py mget 1             # 查看踏板 1 当前步骤
python3 pedal_panel/pedalctl.py tim 1              # 查看踏板 1 当前时序
python3 pedal_panel/pedalctl.py save               # 持久化,重启后仍是宏
```

- 步骤写法:按键名(`shift`/`ctrl`/`alt`/`gui`、`a`-`z`、`0`-`9`、`f1`-`f12`、`space`/`enter`/`esc`/`tab`),组合键用 `+`(如 `shift+z`),或原始十六进制(`02:1D` = Shift+Z 和弦,`1D` = 单键 `z`,`02:00` = 只按 Shift);每踏板最多 6 步。
- 时序含义:每步「按住 `press` 毫秒 → 全部松开 `gap` 毫秒 → 下一步」;松开踏板**立即**释放全部按键,不会卡键。
- `set <踏板> <键码>` 或 `reset` 即退出宏模式恢复单键(`set` 会保留已存步骤和时序,只是不再循环)。
- 三个踏板可各自配单键或宏,同时踩的组合也能表达(修饰键合并进报告 byte 0,普通键入 6 键槽)。

**6. Windows 上改键(uv)**

```bash
cd pedal_panel
uv run python win_remap.py            # 诊断:列出 hidapi 条目 + 探测设备 + 给出后续命令
uv run python win_remap.py set 1 0x04
uv run python win_remap.py get && uv run python win_remap.py save
```

> Windows/macOS 需要 `hidapi`(已按平台在 `pyproject.toml` 声明,`uv` 会自动安装);Linux 上可回退到 `/dev/hidraw`,无需安装。

**7. 图形上位机(Tkinter, Windows/Linux)**

```bash
cd pedal_panel
uv run python pedal_panel.py          # 或 pip install hidapi && python pedal_panel.py
python3 pedal_panel.py --self-test    # 纯逻辑自测(48 项; 无需设备/显示/tkinter)
```

界面:三个踏板各自 单键/宏 模式、键位下拉 + 「捕获按键」(点一下再按物理键自动填 HID 码)、宏步骤编辑(增删改/排序/组合步)、每踏板时序(press/gap ms)、底栏 读取/全部应用/保存到 Flash/恢复默认,顶栏显示固件版本与未保存(dirty)状态。设计/开发文档见 `pedal_panel/README.md`。

## 待补充

- 三个开关的实际接线照片与线序确认(当前仅依据 P4 排针定义)。
- 真实开关踩键验证(目前为 SWD 注入与真机 USB 枚举验证)。
- 第三方许可说明:`lib/st_usbfs`(ST 官方库)与 `lib/cmsis`(Apache-2.0)尚未随仓库附带许可文件。
