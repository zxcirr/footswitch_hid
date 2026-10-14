# pedal_panel — Windows 上位机开发文档

> 目标读者：在 **Windows** 上开发这个 Tkinter 上位机的你。本文档 = 设计 + 开发指南 + 测试计划。
> 只承诺 **Windows 可运行**（Linux/macOS 顺带兼容，不单独保证）。

---

## 1. 定位与功能范围

一个**踏板配置器**（不是按键监视器）：读写三个踏板的单键/宏/时序，持久化到 Flash。

| 能做 | 说明 |
|---|---|
| 每踏板单键绑定 | 任意 HID 键 `0x04–0x65`、修饰键 `0xE0–0xE7`、`0x00` 禁用 |
| 每踏板宏序列 | 1–6 步；每步 = 修饰键组合（Ctrl/Shift/Alt/GUI 多选）+ 普通键；也支持「仅修饰键」步（`02:00` = 只按 Shift） |
| 每踏板宏时序 | 按下 `press` / 步间 `gap`，1–9999 ms，默认 30/30 |
| 持久化 | `SAVE` 写 Flash（重启保留）；`RESET` 恢复默认 1/2/3 |
| 状态 | 固件版本 `fw`、三个绑定与模式（`M` = 宏）、dirty（有未保存修改）|
| 键位捕获 | 点「捕获按键」后按物理键，从 Tk 键事件翻译成 HID 码填入（窗口内捕获，零额外依赖）|

**产品限制（必须在 UI 里体现）**：设备**不上报按键事件**——Feature 通道是「命令→应答」式，键盘报告走 EP1 直通系统。所以：

- 不能做「实时显示哪个踏板被踩」（除非另做键盘钩子，不做）。
- 配完后验证按键 = 用户在任意文本框踩踏板看输入。
- Windows 上没有 evdev，`host_test/evdev_read.py`、`qa_e2e.py` 与上位机无关。

---

## 2. 运行环境（Windows）

```text
Python 3.9+   （python.org 安装包自带 tkinter；Microsoft Store 版 Python 也可以）
hidapi        （Windows 必需，没有 /dev/hidraw 回退）
```

两种装法（任选）：

```bash
# A. uv（推荐，与 host_test 一致）
cd footswitch_hid/pedal_panel
uv run python pedal_panel.py

# B. pip
cd footswitch_hid/pedal_panel
pip install hidapi
python pedal_panel/pedal_panel.py
```

设备：VID `0x1209` / PID `0x0001`，通信走**接口 1 厂商 HID**（usage_page `0xFF00`）的 48 字节 Feature 通道。打开逻辑、应答严格匹配全部复用同目录 `pedalctl.py`，不要重写。

---

## 3. 目录结构

```text
footswitch_hid/
├── host_test/             # 纯测试目录（描述符 / C 单测 / 语义单测 / qa_e2e）
│   └── ...                # 从 ../pedal_panel 导入 pedalctl，测试与发布共用一份代码
└── pedal_panel/           # 本目录 = 上位机，完全自包含：整体拷贝此目录即可到 Windows 运行
    ├── pedal_panel.py     # Tkinter 图形界面（--self-test 纯逻辑自测 48 项）
    ├── pedalctl.py        # 协议核心 + api_* 数据层 + CLI 改键工具（本文档第 4 节）
    ├── win_remap.py       # Windows 诊断/改键 CLI 前端（转发 pedalctl）
    ├── pyproject.toml     # uv 项目 + hidapi(win32) 依赖
    └── README.md          # 本文档
```

设计决定：**单文件** `pedal_panel.py`。逻辑（键映射、命令生成、collect/fill）做成模块级纯函数，`--self-test` 可在无显示环境（包括当前容器）回归。

---

## 4. 已就绪的地基：`pedalctl.py` 数据层（M1 已完成 ✅）

以下函数已在同目录 `pedalctl.py` 实现并用伪设备实测通过（返回数据/元组，**不打印**）：

| 函数 | 说明 | 返回（实测） |
|---|---|---|
| `load_hidapi()` | 导入 hidapi，无则 None | module 或 None |
| `open_device(hid_module)` | 枚举 1209:0001，只认 usage_page 0xFF00 的厂商接口（含诊断输出） | `(device, None)` / `(None, 2)` |
| `api_info(dev)` | fw / keys / dirty | `{'fw': '0.0.2', 'keys': ['M','0x1F','0x20'], 'dirty': True}` |
| `api_map(dev)` | 三个绑定（`0xNN` 或 `M`） | `['M', '0x1F', '0x20']` |
| `api_steps(dev, pedal)` | 某踏板宏步骤 | `[(2, 0), (0, 29), (0, 6)]`，`[]` = 无，`None` = 应答不符 |
| `api_timing(dev, pedal[, p, g])` | 查询 / 设置时序 | `(True, (15, 25), 'OK MTIM 1 15 25')` |
| `api_set(dev, pedal, code)` | 单键绑定 | `(True, 'OK SET 1=0x04')` |
| `api_mac(dev, pedal, steps[, p, g])` | 宏（可带时序） | `(True, 'OK MAC 1 1D', '1D')` |
| `api_save(dev)` | 写 Flash，轮询 `OK saved`（最多 ~2 s） | `(True, 'OK saved')` |
| `api_reset(dev)` | 恢复默认 | `(True, 'OK RESET')` |

可复用的表与构造器（都在 pedalctl.py）：

```python
pedalctl.QUIET = True              # 静默 CLI 原始字节转贮，GUI 自绘状态
pedalctl.REPORT_MAX_TEXT           # 47（命令最长字符数，48B 报告）
pedalctl.MACRO_MAX_STEPS           # 6
pedalctl.MACRO_TIME_MAX            # 9999
pedalctl.MODIFIER_NAMES            # "shift":0x02, "ctrl":0x01, "alt":0x04, "gui":0x08, "rshift":0x20 ...
pedalctl.KEY_NAMES                 # "a":0x04 .. "z":0x1D, "1":0x1E .. "0":0x27, "f1".."f12",
                                   # "space":0x2C, "enter":0x28, "esc":0x29, "tab":0x2B, "backspace":0x2A
pedalctl.KEY_BY_CODE / MOD_BITS    # 反向表 / [(0x01,"Ctrl"),(0x02,"Shift"),(0x04,"Alt"),(0x08,"GUI"),...]
pedalctl.parse_step_token("shift+z")   # -> (0x02, 0x1D)
pedalctl.encode_step(0x02, 0x1D)       # -> "02:1D"
pedalctl.decode_step_token("02:1D")    # -> "Shift+z"
pedalctl.build_mac_command(1, steps)   # -> ("MAC 1 02:00,1D,06", "02:00,1D,06")，超长抛 ValueError
```

---

## 5. UI 设计

### 5.1 线框图

```text
┌──────────────────────────────────────────────────────────────────────────┐
│ ● 已连接 FootSwitch (fw 0.0.2)      ● 未保存/unsaved      [重新连接]      │  顶栏：状态灯+文本 / dirty 徽标 / 重连
├──────────────────────────────────────────────────────────────────────────┤
│ ┌─ 踏板 1 / Pedal 1 ───────────────────────────────────────────────────┐ │
│ │ 模式: (•)单键  ( )宏                                                  │ │
│ │ 键位: [z (0x1D)          ▾]  [捕获按键]        ← 单键模式             │ │
│ │ 宏步骤: [1. Shift        ]  [添加][编辑][删除][上移][下移]  ← 宏模式  │ │
│ │         [2. z (0x1D)     ]                                            │ │
│ │         [3. c (0x06)     ]                                            │ │
│ │ 宏时序(ms): 按下[30]  间隔[30]          [应用到设备]                  │ │
│ └──────────────────────────────────────────────────────────────────────┘ │
│ ┌─ 踏板 2 ──（同结构）─────────────────────────────────────────────────┐ │
│ ┌─ 踏板 3 ──（同结构）─────────────────────────────────────────────────┐ │
├──────────────────────────────────────────────────────────────────────────┤
│ [读取设备配置]  [全部应用]  [保存到 Flash]  [恢复默认]                    │
├──────────────────────────────────────────────────────────────────────────┤
│ 日志（每步操作的应答原文 / 错误）                                        │
└──────────────────────────────────────────────────────────────────────────┘
```

### 5.2 类设计（单文件内）

```python
class PedalFrame(ttk.LabelFrame)   # 一个踏板：模式 radio / 单键 combobox+捕获 /
                                  # 宏 Listbox+5 按钮 / 时序 Spinbox / [应用]
    def fill(model)        # 设备配置 -> 控件
    def collect() -> model # 控件 -> 设备配置
    def start_capture()    # 请求 App 进入捕获态
    def apply_captured(kind, code, mask)   # 捕获结果落库（追加宏步 / 填单键）

class StepDialog(tk.Toplevel)   # 添加/编辑一步：4 个修饰键勾选 + 按键 combobox + 捕获
    -> result = (mask, key) 或 None

class App(tk.Tk)
    connect()/read_config()/apply_all()/save()/reset()   # 全部走 run_async
    begin_capture(frame)/_on_capture_key(event)          # bind_all <KeyPress>
    run_async(fn, on_done)   # 线程 + after(0)
    set_status()/log()/_led(color)
```

### 5.3 控件细节

| 区域 | 控件 | 说明 |
|---|---|---|
| 模式 | 2 × `ttk.Radiobutton`（`StringVar`）| `single` / `macro`；切换时禁用/启用对应控件（`state="disabled"`）|
| 单键 | `ttk.Combobox(readonly)` | 选项见下表，`SINGLE_CHOICES = [(label, code)]` |
| 单键捕获 | `ttk.Button` | 见第 6 节 |
| 宏步骤 | `tk.Listbox(height=4)` + 5 按钮 | 显示 `1. Shift` / `2. z (0x1D)`（`step_text()`）；上移/下移交换 `steps` 列表 |
| 时序 | 2 × `ttk.Spinbox(from_=1, to=9999, width=6)` | `StringVar`，`collect()` 时 `max(1, min(9999, int))`，非法回落 30 |
| 全局按钮 | 4 × `ttk.Button` | read / apply all / save / reset（reset 前 `messagebox.askyesno` 确认）|

**单键 combobox 选项**（`SINGLE_CHOICES`，按 code 排序）：

```text
禁用 Disabled (0x00)
a..z (0x04..0x1D)
1..9, 0 (0x1E..0x27)
F1..F12 (0x3A..0x45)
- = [ ] \ ; ' ` , . /   （标点，0x2D..0x38）
space / enter / esc / tab / backspace
修饰键: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI (0xE0..0xE7)
```

**宏步骤按键 combobox**（`KEY_STEP_ITEMS`）= 上面全部 + 第一项「（仅修饰键 / modifier-only）0x00」。

### 5.4 数据流

```text
[读取]  App._read_worker:  api_info → api_map → ×3(api_steps + api_timing)
        _read_done:   PedalFrame.fill(model)
[应用]  PedalFrame.collect() → model
        单键: api_set(pedal, code)
        宏:   api_mac(pedal, steps, press, gap)      # 一条命令带时序
              若 "MAC n body p g" 超过 47 字符 → 拆 api_mac(不带时序) + api_timing
        全部成功 → 重新 api_info 刷新 dirty 徽标
[保存]  api_save（轮询 OK saved，最多 ~2s）→ 重新 api_info
[恢复]  messagebox 确认 → api_reset → 重新 read_config
```

---

## 6. 键位捕获设计（窗口内）

点「捕获按键」→ App `bind_all("<KeyPress>", handler)` → 按任意键 → `unbind_all`。

### 6.1 keysym → HID 码

```python
MOD_KEYSYM_BITS = {"Control_L":0x01,"Control_R":0x10,"Shift_L":0x02,"Shift_R":0x20,
                   "Alt_L":0x04,"Alt_R":0x40,"Super_L":0x08,"Super_R":0x80}
MOD_BIT_TO_CODE = ((0x01,0xE0),(0x02,0xE1),(0x04,0xE2),(0x08,0xE3),
                   (0x10,0xE4),(0x20,0xE5),(0x40,0xE6),(0x80,0xE7))
MOD_NAMES = {0x01:"LCtrl",0x02:"LShift",0x04:"LAlt",0x08:"LGUI",
             0x10:"RCtrl",0x20:"RShift",0x40:"RAlt",0x80:"RGUI"}
KEYSYM_ALIASES = {"return":"enter","escape":"esc","backspace":"backspace",
                  "space":"space","kp_enter":"enter"}     # + kp_<digit> → <digit>
PUNCT_KEYCODES = {"comma":0x36,"period":0x37,"slash":0x38,"semicolon":0x33,
                  "apostrophe":0x34,"grave":0x35,"minus":0x2D,"equal":0x2E,
                  "bracketleft":0x2F,"bracketright":0x30,"backslash":0x31}

def keysym_to_keycode(keysym):
    """-> (kind, code): kind = "key" | "mod" | None"""
    if keysym in MOD_KEYSYM_BITS: ...        # ("mod", 0xE0+bit)
    name = _normalize_keysym(keysym)          # lower + 别名 + 去 kp_ 前缀
    if name in pedalctl.KEY_NAMES:   return "key", pedalctl.KEY_NAMES[name]
    if name in PUNCT_KEYCODES:       return "key", PUNCT_KEYCODES[name]
    return None, 0                    # 例: Pause/Break/Print 等不支持
```

### 6.2 event.state → 修饰键掩码

```python
STATE_BIT_TO_MASK = ((0x0001,0x02),   # Tk Shift
                     (0x0004,0x01),   # Tk Control
                     (0x0008,0x04),   # Tk Mod1 = Alt
                     (0x0040,0x08))   # Tk Mod4 = GUI/Windows 键
def state_to_mask(state): ...          # CapsLock(0x2)/NumLock(0x10)/Mod5 忽略
```

### 6.3 捕获落库规则

| 捕获到的 | 单键模式 | 宏模式 |
|---|---|---|
| 普通键（无修饰） | combobox 填该键 | 追加一步 `(0, code)` |
| 普通键 + 按住修饰键 | 只填普通键；日志提示「组合键请用宏」 | 追加一步 `(state_mask, code)` |
| 修饰键本身 | combobox 填对应 `0xE0+n` | 追加一步 `(bit, 0x00)` |
| `Escape` | 取消捕获 | 取消捕获 |
| 无法识别的 keysym | 状态栏提示，不改动 | 同 |

捕获期间状态栏显示「请按下任意键 (Esc 取消)…」。

---

## 7. 数据模型与命令生成

```python
model = {"mode": 0|1, "code": int, "steps": [(mask, key), ...],
         "press": int, "gap": int}

def model_to_commands(pedal, model):
    if model["mode"] == 0:
        return ["SET %d 0x%02X" % (pedal, model["code"])]
    _, body = pedalctl.build_mac_command(pedal, model["steps"])
    return ["MAC %d %s %d %d" % (pedal, body, model["press"], model["gap"])]
```

上面是「理想命令」，实际发送走 api_*（自带严格应答匹配）。长度回退规则见 5.4。

**`--self-test`（无显示可跑，必须实现）**：对 `keysym_to_keycode` / `state_to_mask` / `step_text` / `model_to_commands` / 长度回退 做 20+ 断言，风格对齐 `host_test/test_macro.c`（`PASS/FAIL` + `ALL PASS` + 退出码）。tkinter 导入失败时也必须能跑，因此 tkinter 导入要 try/except 守卫。

---

## 8. 线程模型

所有 Feature I/O 放 worker 线程，UI 更新 marshal 回主线程：

```python
def run_async(self, fn, on_done):
    def worker():
        try:    result = fn()
        except Exception as exc:
            self.after(0, lambda: self._async_error(exc))
        else:   self.after(0, lambda: on_done(result))
    threading.Thread(target=worker, daemon=True).start()
```

- `api_save` 最慢（轮询上限 ~2 s），放线程避免UI卡死。
- `open_device()` 会 print 诊断；`_connect_worker` 里用 `contextlib.redirect_stdout(io.StringIO())` 捕获，`_connected` 里 `self.log(...)` 显示（注意 redirect 是全局动作，连接期间主线程不要 print）。
- 线程里不要碰任何 tk 控件。

---

## 9. 实现里程碑

> 代码与自动化验证已全部完成（`--self-test` 48 项、api 语义 20 项、`make test` 全绿；
> Windows 侧连接/枚举/CLI 读写已于 2026-10-14 实机通过, MI_01 Status OK）。
> GUI 逐项交互按 §10.2 清单收尾。

| 里程碑 | 内容 | 完成判据 |
|---|---|---|
| M1 | pedalctl.py 数据层（api_* + QUIET）| 伪设备实测 |
| M2 | `pyproject.toml` + 窗口骨架 + 连接/状态/dirty/日志 + 读取 | 读回三踏板配置 |
| M3 | 单键：combobox + 应用（api_set）| 改键后踩踏板输出对应字符 |
| M4 | 宏：Listbox + StepDialog + api_mac（含 47 字符长度回退）| 三独立步/和弦步循环正常 |
| M5 | 捕获按键（§6.1–6.3）+ StepDialog 内捕获 | 按物理键自动填码 |
| M6 | save / reset / 时序 Spinbox / `--self-test` | 保存重启保留；reset 回 1/2/3 |

---

## 10. 测试计划

### 10.1 无显示环境（本容器 CI 风格）

```bash
python3 -m py_compile pedal_panel/pedal_panel.py
python3 pedal_panel/pedal_panel.py --self-test     # 纯逻辑，无需 tkinter/显示
python3 host_test/test_pedalctl_semantics.py       # 现有语义测试保持绿
```

### 10.2 Windows 真机清单（你来做）

```text
□ uv run python pedal_panel.py   启动无报错，状态灯变绿，显示 fw 0.0.2
□ 读取：三个踏板显示与 pedalctl.py get/mget 1/tim 1 输出一致
□ 单键：踏板1 改 "a" → 应用 → 文本框踩踏板出 "a"
□ 宏：踏板1 = shift,z,c（捕获或下拉各加一步）→ 应用 → 踩住循环 Shift→z→c，松开即停
□ 宏带修饰：shift+z 一步（按住 Shift 再按 z 捕获）→ 输出 Shift+z
□ 时序：press/gap 改 15/15 → 循环明显变快
□ 保存 → 拔插 USB → 配置仍在；恢复默认 → 1/2/3
□ dirty 徽标：改键后亮，保存后灭
□ 拔线 → 重新连接按钮恢复
□ 交换机/分屏/dpi 缩放下窗口不破版（moved：用 grid+padding，别写死像素）
```

### 10.3 Windows 部署速查与故障排查

前置：python.org 版 Python 3.9+（自带 tkinter）；把 `pedal_panel/` 整目录拷到 Windows 即可。

```powershell
cd D:\...\pedal_panel
uv sync                      # 或 pip install hidapi; hardlink warning 无害(UV_LINK_MODE=copy 可消)
uv run python pedal_panel.py
```

首连预期：绿灯、`fw 0.0.2`、键位 `0/-/=`（板载 v1 旧配置自动迁移所致）、右上 dirty 红点；点「保存到 Flash」固化为 v2。

| 症状 | 原因 | 解决 |
|---|---|---|
| `ModuleNotFoundError: fcntl` | 拷贝的 `pedalctl.py` 过旧 | 重拷 pedal_panel / `git pull` |
| `No module named tkinter` | 非 python.org 版 Python | 换官方安装包并勾选 tcl/tk |
| 只枚举到 `interface_number=0` / MI_01 Code 10 | 固件过旧（Feature 无独立 Usage）或混编产物（宣告 21 实回 19） | 烧 bcdDevice≥0.05 的干净构建；改描述符必 bump revision |
| 红灯 + “未找到厂商 HID 条目” | 板子 USB 未插 / 仅充电线 / 未枚举 | 设备管理器找 1209:0001；`uv run python win_remap.py` 诊断 |
| 保存超时 | 设备正忙擦写 / 两个上位机同开 | 稍候重试；保持单实例 |
| 重启后配置丢失 | 只“应用”未“保存” | 应用只写 RAM，必须保存到 Flash |
| `ERR syntax` / `ERR range` | 超出协议约束 | 见第 6/7 节与 `app/cmd.h` 契约 |

---

## 11. 坑与注意事项

1. **命令 47 字符上限**：6 个组合步 + 时序会超（`6 + 35 + 10 = 51`）。发送前按 5.4 拆成 `MAC` + `MTIM` 两条；`build_mac_command` 会先抛 ValueError，别把异常直接弹给用户，转成日志。
2. **SAVE 轮询 ~2 s**：必须线程；期间不要再发其他命令（设备主循环忙着擦 Flash，会拖慢应答）。
3. **hidapi 独占**：打开着 pedalctl 时 GUI 也能开（厂商接口同一设备对象由 hidapi 管理，两个进程各开各的 handle 一般可行），但**两个 GUI 同时开**会互相抢，启动时做单实例或容忍失败提示。
4. **中文路径/编码**：`pedal_panel.py` 顶部 `# -*- coding: utf-8 -*-`；Windows 控制台日志用 `sys.stdout.reconfigure(encoding="utf-8", errors="replace")`（仓库既有模式）。
5. **tkinter 无显示环境**：导入要 try/except 守卫，`--self-test` 不依赖 tkinter（本容器就没装 python3-tk，M2 之前的回归靠它）。
6. **Spinbox 空值/非法值**：清空时 `int()` 会炸，`collect()` 里 try/except 回落默认。
7. **多语言**：控件文案沿用仓库的「中文 / English」双语风格，别只写一种。

---

## 12. 设计决策（v1 已实现）

| # | 问题 | 实现选择 |
|---|---|---|
| 1 | UI 语言 | 中英双语（与仓库现有工具一致）|
| 2 | 单键模式捕获到 `Shift+z` | 忽略修饰键，日志提示「组合键请用宏模式」；宏模式则完整记录掩码 |
| 3 | JSON 配置导入/导出 | v1 不做 |
| 4 | 托盘 / 开机自启 / 多设备 | v1 不做 |

---

## 13. 项目现状

- `pedal_panel/`：自包含上位机目录（GUI `pedal_panel.py` + CLI `pedalctl.py` / `win_remap.py` + 本文档），整体拷贝即可在 Windows 运行
- `host_test/`：纯测试目录（描述符 8 项 / cmd 140 / macro 44 / pedalctl 语义含 api 层 / qa_e2e·evdev 真机工具）
- 固件：宏功能 + 描述符修复完成；板上固件与仓库构建逐字节一致（注释级改动不触发重烧），Windows 侧已实机验证：REV_0005、MI_00/MI_01 均 OK、`GET` 返回 `0x27 0x2D 0x2E`（板载 v1 迁移配置，dirty=1 属预期）
- 仓库：GitHub `zxcirr/footswitch_hid` 单 `master` 分支（默认），与本地同步
