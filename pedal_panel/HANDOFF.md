# HANDOFF — Windows 开发交接归档（2026-10-14）

> 本文档归档 Linux 侧已完成的全部工作。`pedal_panel/` 目录**自包含**：整体拷贝到 Windows 即可开发与调试。
> 源仓库：`/home/z/STM32/footswitch_hid`（本地与 GitHub `zxcirr/footswitch_hid@master` 已同步）。

---

## 0. 你现在必须做的第一步

**重新拷贝 `pedal_panel/` 目录覆盖 `D:\download\pedal_panel`。**
你现在那份里 `pedalctl.py` 是旧版（55417 字节，未含 Windows 兼容修复），首跑必崩：

```text
ModuleNotFoundError: No module named 'fcntl'
```

拷贝后用第 3 节的哈希表核对，再跑自查命令：

```powershell
python -c "import pedalctl; print('fcntl =', pedalctl.fcntl)"   # 应打印 None, 不再报错
```

---

## 1. 项目全景状态

| 层 | 状态 | 验证方式与结果 |
|---|---|---|
| 固件 v0.0.2（STM32F103C8T6） | ✅ 完成 | `make` 编译 OK（text 10456B）；C 单测 cmd 140 / macro 44 全绿 |
| 宏功能（踩住循环按键） | ✅ 完成并已上真机 | 已用 DAPLink 烧录；SWD 注入实测：v1→v2 迁移、单键输出、宏三相位循环（Shift/z/c）全部验证 |
| USB 描述符 | ✅ 冻结 | 双接口（boot 键盘 + 厂商 Feature），无 report ID；描述符校验 8/8；**不要改动**（Windows HidUsb 兼容性） |
| Feature 协议 | ✅ 稳定 | `SET/GET/MAC/MGET/MTIM/SAVE/RESET/INFO`，错误分类 syntax/pedal/range；语义单测全绿 |
| CLI 工具 | ✅ 完成 | `pedalctl.py`（全部子命令 + 严格应答匹配）、`win_remap.py`（Windows 诊断前端） |
| GUI 上位机 | ✅ 代码完成 | `pedal_panel.py`：单键/宏/时序/捕获按键/保存 Flash/dirty 徽标/日志；`--self-test` 48 项全过；**真机交互待你在 Windows 验证**（第 4 节清单） |
| 板子当前 Flash 配置页 | ⚠️ 旧 v1 状态 | 存的是 `0`/`-`/`=` 三键 + dirty=1；GUI 首连会如实显示，点「保存到 Flash」一次即固化为 v2 |

## 2. 本次工作日志（提交记录）

| commit | 内容 |
|---|---|
| `dc7e8b0` / `d1c1f2a`（已在远程） | 宏功能全套：`app/macro.c` 引擎、`MAC/MGET/MTIM` 协议、Flash v2 镜像+v1 迁移、pedalctl CLI 宏子命令、QA S6 场景 |
| `2778a0a` | GUI 上位机 `pedal_panel.py` + `pedalctl.py` 新增 `api_*` 数据层（9 函数）+ `QUIET` 开关 + 48 项自测 + 20 项 api 语义用例 |
| `b266901` | 目录拆分：**`pedal_panel/` = 上位机（自包含）**，`host_test/` = 纯测试；`pedalctl.py` 迁入 pedal_panel（单一副本防漂移） |
| `a1569b5` | **Windows 兼容修复**：`fcntl` 条件导入 + hidraw 回退守卫（你的崩溃根因） |

---

## 3. Windows 部署与文件校验

### 3.1 环境要求

- Python 3.9+（**python.org 安装包**自带 tkinter；若 `import tkinter` 失败即装错版本）
- `uv`（或 `pip install hidapi`）；`uv sync` 的 hardlink warning 无害，可 `set UV_LINK_MODE=copy` 消掉
- 板子 micro-USB 插到 Windows 本机（DAPLink 可继续挂着，互不影响；Windows 免驱，HID 设备无需管理员）

### 3.2 文件清单（拷贝后逐一核对 sha256 前缀 / 字节数）

| 文件 | 字节 | sha256 |
|---|---:|---|
| `pedal_panel.py` | 38945 | `a1def33a…ce3ea4` |
| `pedalctl.py` | **55916**（旧崩溃版是 55417） | `bf4d3d43…c02fdd` |
| `win_remap.py` | 8546 | `903bb454…73c7c4` |
| `pyproject.toml` | 370 | `9ab9e5c7…2e8a00` |
| `README.md`（设计文档） | 19677 | `fbaab5bb…8d6a6d` |

```powershell
# Windows 全文件校验
Get-FileHash .\*.py, .\pyproject.toml, .\README.md -Algorithm SHA256 | Format-Table Hash, Path -AutoSize
```

### 3.3 启动

```powershell
cd D:\download\pedal_panel
uv sync
uv run python pedal_panel.py
```

---

## 4. 首次运行验证清单（真机）

```text
□ 启动无报错；日志区有连接诊断；插着踏板时状态灯 3s 内变绿, 显示 fw 0.0.2
□ 首连预期: 三个踏板显示 0 / - /=(v1 迁移的旧键位), 右上"● 未保存"为红色
□ 读取: 与 CLI 一致 → python pedalctl.py get / mget 1 / tim 1
□ 单键: 踏板1 选 'a' → 应用 → 文本框踩踏板出 a; 日志出现 OK SET 1=0x04
□ 捕获: 点「捕获按键」按 Z → 自动填 z; 按住 Shift 再按 → 日志提示"修饰键已忽略, 组合键请用宏"
□ 宏: 踏板1 添加 3 步(shift / z / c, 用捕获或下拉) → 应用 → 踩住循环 Shift→z→c, 松开立即停
□ 宏和弦: 步骤编辑里勾 Shift + 选 z → 一步输出 Shift+z
□ 时序: press/gap 改 15/15 → 循环明显变快; 改 200/200 → 明显变慢
□ 保存: 点保存到 Flash → 日志 OK saved → dirty 灭 → 拔插 USB → 配置仍在(v2 页)
□ 恢复默认: 确认后三踏板回 1/2/3, 宏清空
□ 拔线 → 红灯 → 重新连接恢复; 6 步和弦 + 长时序自动拆 MAC+MTIM 两条发送(日志可看到两条)
□ 窗口缩放/DPI 不破版
```

## 5. 故障排查表

| 症状 | 原因 | 解决 |
|---|---|---|
| `ModuleNotFoundError: fcntl` | 拷了旧版 pedalctl.py | 重拷; 用 3.2 表核对 |
| `No module named tkinter` | 非 python.org 版 Python | 装官方 Python, 勾选 tcl/tk |
| 红灯 + "未找到厂商 HID 条目" | 板子 USB 未插/仅供电线/未枚举 | 查设备管理器 1209:0001; 换线; `uv run python win_remap.py` 一键诊断 |
| `ERR range` / `ERR syntax` | 键码/步骤超出协议范围 | 对照 `README.md` 第 6/7 节约束 |
| 保存超时 | 板子忙擦写/应答拖慢 | 等 1-2s 重试; 确认没同时开两个上位机 |
| 改键后重启丢失 | 忘了保存 | 应用只写 RAM, 必须点「保存到 Flash」 |

## 6. 速查

- **协议硬约束**：命令 ≤47 字符；宏 1..6 步；时序 1..9999ms；键码 `0x04..0x65` + 修饰键 `0xE0..0xE7` + `0x00` 禁用；步骤线上格式 `MM:KK`/`KK`（`02:00`=仅 Shift，`02:1D`=Shift+Z）
- **api_* 数据层**（GUI/脚本直接调用，全部带严格应答匹配）：`api_info/api_map/api_steps/api_timing/api_set/api_mac/api_save/api_reset`
- **纯逻辑自测（无需设备/显示）**：`python pedal_panel.py --self-test`
- **CLI**：`python pedalctl.py set|get|save|reset|info|mac|mget|tim ...`；诊断入口 `win_remap.py`（无参=中文向导）
- **设计细节**：本目录 `README.md`（线框图、捕获规则 6.3、里程碑、坑清单）

## 7. 遗留与可选项

- GUI 真机交互（第 4 节清单）——你在 Windows 的首个任务
- 固件/文档既有待办：接线照片、第三方许可文件、`cfg_load_v1` pad 字节栈垃圾（无害）
- GUI 候选增强（未实现）：JSON 配置导入导出、多语言切换、系统级按键钩子
