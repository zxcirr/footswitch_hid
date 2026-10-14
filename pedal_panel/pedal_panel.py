#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pedal_panel.py — FootSwitch 踏板上位机 / desktop configurator (Tkinter).

配置 / configures (VID 0x1209 / PID 0x0001, USB HID Feature 通道):
  * 每踏板单键绑定: 任意 HID 键 (a-z/0-9/F1-F12/标点), 修饰键 (0xE0-0xE7),
    禁用 (0x00)
  * 每踏板宏序列: 1-6 步, 每步 = 修饰键组合 + 普通键 (或仅修饰键);
    踩住循环 (按下 press ms -> 松开 gap ms -> 下一步), 松开立即释放
  * 每踏板宏时序: press / gap, 1..9999 ms
  * 持久化 (写 Flash), 恢复默认, 读取当前配置, 固件版本 + 未保存提示
  * 键位捕获: 点 [捕获按键] 后按物理键, 从 Tk 键事件翻译 HID 码 (窗口内捕获)

设备通信复用同目录 pedalctl.py 的 api_* 数据层与打开逻辑
(hidapi / hidraw 回退)。本目录完全自包含, 可整体拷贝到 Windows 使用。
设计文档: pedal_panel/README.md。

用法 / usage:
    python pedal_panel.py              # 启动界面 / launch the UI
    python pedal_panel.py --self-test  # 纯逻辑自测 / pure-logic self test
                                       # (无需 tkinter / 显示 / 设备)
"""

from __future__ import annotations

import contextlib
import io
import os
import sys
import threading

# 同目录导入 pedalctl (本目录自包含, 可从任意位置运行/整体拷贝到 Windows)
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import pedalctl

pedalctl.QUIET = True          # 静默 CLI 的原始字节转储 / silence CLI dumps

try:
    import tkinter as tk
    from tkinter import messagebox, ttk
except ImportError:            # headless self-test environment (no python3-tk)
    tk = None
    messagebox = None
    ttk = None

APP_TITLE = "FootSwitch 踏板配置 / pedal configurator"
DEFAULT_PRESS_MS = 30          # 与固件 CMD_MACRO_PRESS_DEFAULT 一致
DEFAULT_GAP_MS = 30

# 无 tkinter 环境仍可导入本模块跑 --self-test: UI 类挂到 object 占位基类上,
# 只有 main() 在有 tkinter 时才会实例化它们。
_UI_FRAME_BASE = ttk.LabelFrame if ttk is not None else object
_UI_TOPLEVEL_BASE = tk.Toplevel if tk is not None else object
_UI_APP_BASE = tk.Tk if tk is not None else object

# --------------------------------------------------------------------------- #
# Tk 键事件 -> HID 码 (纯逻辑; 设计与规则见 README.md 第 6 节, --self-test 覆盖)
# --------------------------------------------------------------------------- #

# Tk 修饰键 keysym -> 修饰键位图 bit (bit b = HID modifier 0xE0+b)
MOD_KEYSYM_BITS = {
    "Control_L": 0x01, "Control_R": 0x10,
    "Shift_L": 0x02, "Shift_R": 0x20,
    "Alt_L": 0x04, "Alt_R": 0x40,
    "Super_L": 0x08, "Super_R": 0x80,
    "Meta_L": 0x08, "Meta_R": 0x80,
}

# 修饰键 bit -> 单键模式下的修饰键码 (SET n 0xE0..0xE7)
MOD_BIT_TO_CODE = ((0x01, 0xE0), (0x02, 0xE1), (0x04, 0xE2), (0x08, 0xE3),
                   (0x10, 0xE4), (0x20, 0xE5), (0x40, 0xE6), (0x80, 0xE7))
MOD_CODE_TO_BIT = {code: bit for bit, code in MOD_BIT_TO_CODE}
MOD_NAMES = {0x01: "LCtrl", 0x02: "LShift", 0x04: "LAlt", 0x08: "LGUI",
             0x10: "RCtrl", 0x20: "RShift", 0x40: "RAlt", 0x80: "RGUI"}

# 宏步骤编辑对话框里勾选的主修饰键 (bit, 显示名)
MAIN_MODIFIERS = ((0x01, "Ctrl"), (0x02, "Shift"), (0x04, "Alt"), (0x08, "GUI"))

# Tk event.state bit -> HID 修饰键掩码 bit (Lock/NumLock/Mod5 忽略)
STATE_BIT_TO_MASK = ((0x0001, 0x02),    # Tk Shift
                     (0x0004, 0x01),    # Tk Control
                     (0x0008, 0x04),    # Tk Mod1 (Alt)
                     (0x0040, 0x08))    # Tk Mod4 (GUI / Windows 键)

# 标点 keysym -> HID usage (pedalctl.KEY_NAMES 只覆盖字母/数字/功能键)
PUNCT_KEYCODES = {"comma": 0x36, "period": 0x37, "slash": 0x38,
                  "semicolon": 0x33, "apostrophe": 0x34, "grave": 0x35,
                  "minus": 0x2D, "equal": 0x2E, "bracketleft": 0x2F,
                  "bracketright": 0x30, "backslash": 0x31}
PUNCT_LABELS = {0x36: ",", 0x37: ".", 0x38: "/", 0x33: ";", 0x34: "'",
                0x35: "`", 0x2D: "-", 0x2E: "=", 0x2F: "[", 0x30: "]",
                0x31: "\\"}

KEYSYM_ALIASES = {"return": "enter", "escape": "esc", "kp_enter": "enter"}


def _normalize_keysym(keysym: str) -> str:
    """'Return'->'enter', 'KP_3'->'3', 'F5'->'f5' (pedalctl.KEY_NAMES 用小写)."""
    ks = keysym.lower()
    if ks in KEYSYM_ALIASES:
        return KEYSYM_ALIASES[ks]
    if ks.startswith("kp_") and len(ks) == 4 and ks[3].isdigit():
        return ks[3]
    return ks


def keysym_to_keycode(keysym: str):
    """Tk keysym -> (kind, code); kind = "key" | "mod" | None (无法识别)."""
    if keysym in MOD_KEYSYM_BITS:
        bit = MOD_KEYSYM_BITS[keysym]
        for b, code in MOD_BIT_TO_CODE:
            if b == bit:
                return "mod", code
    name = _normalize_keysym(keysym)
    if name in pedalctl.KEY_NAMES:
        return "key", pedalctl.KEY_NAMES[name]
    if name in PUNCT_KEYCODES:
        return "key", PUNCT_KEYCODES[name]
    return None, 0


def state_to_mask(state: int) -> int:
    """Tk KeyEvent.state 修饰键位 -> HID modifier byte (Lock 类忽略)."""
    mask = 0
    for bit, mask_bit in STATE_BIT_TO_MASK:
        if state & bit:
            mask |= mask_bit
    return mask


def _named_key_labels():
    """code -> 最短键名 (丢弃 pedalctl.KEY_NAMES 里的 'return'/'escape' 别名)."""
    labels = {}
    for name, code in pedalctl.KEY_NAMES.items():
        if code not in labels or len(name) < len(labels[code]):
            labels[code] = name
    return labels


NAMED_KEY_LABELS = _named_key_labels()


def code_label(code: int) -> str:
    """HID 码 -> 短标签 ('z', ',', 'LShift'); 未知码 -> '0xNN'."""
    if code in NAMED_KEY_LABELS:
        return NAMED_KEY_LABELS[code]
    if code in PUNCT_LABELS:
        return PUNCT_LABELS[code]
    for bit, mod_code in MOD_BIT_TO_CODE:
        if mod_code == code:
            return MOD_NAMES[bit]
    return "0x%02X" % code


def mask_text(mask: int) -> str:
    return "+".join(MOD_NAMES[bit] for bit, _code in MOD_BIT_TO_CODE if mask & bit)


def step_text(mask: int, key: int) -> str:
    """宏步骤一行文本: 'LShift + z (0x1D)' / 'LShift' / '(空 / empty)'."""
    parts = [MOD_NAMES[bit] for bit, _code in MOD_BIT_TO_CODE if mask & bit]
    if key:
        parts.append("%s (0x%02X)" % (code_label(key), key))
    return " + ".join(parts) if parts else "(空 / empty)"


# ---- combobox 选项表 (label, code) ----------------------------------------- #

def _single_choices():
    items = [("禁用 Disabled", 0x00)]
    for code in sorted(NAMED_KEY_LABELS):
        items.append((NAMED_KEY_LABELS[code], code))
    for code in sorted(PUNCT_LABELS):
        items.append((PUNCT_LABELS[code], code))
    for bit, code in MOD_BIT_TO_CODE:
        items.append(("%s (0x%02X)" % (MOD_NAMES[bit], code), code))
    return items


SINGLE_CHOICES = _single_choices()
SINGLE_LABELS = [label for label, _code in SINGLE_CHOICES]
SINGLE_LABEL_TO_CODE = dict(SINGLE_CHOICES)
SINGLE_BY_CODE = {code: label for label, code in SINGLE_CHOICES}


def _key_step_items():
    items = [("（仅修饰键 / modifier-only）", 0x00)]
    for code in sorted(NAMED_KEY_LABELS):
        items.append((NAMED_KEY_LABELS[code], code))
    for code in sorted(PUNCT_LABELS):
        items.append((PUNCT_LABELS[code], code))
    return items


KEY_STEP_ITEMS = _key_step_items()
STEP_LABELS = [label for label, _code in KEY_STEP_ITEMS]
STEP_LABEL_TO_CODE = dict(KEY_STEP_ITEMS)
STEP_LABEL_BY_CODE = {code: label for label, code in KEY_STEP_ITEMS}


# ---- 数据模型 -> 线上命令 (自测用; 实际发送走 pedalctl.api_*) -------------- #

def model_to_commands(pedal: int, model: dict):
    """UI model -> 理想命令列表 (README.md 第 7 节; 超长时 build 会抛错)."""
    if model["mode"] == 0:
        return ["SET %d 0x%02X" % (pedal, model["code"])]
    _command, body = pedalctl.build_mac_command(pedal, model["steps"])
    return ["MAC %d %s %d %d" % (pedal, body, int(model["press"]), int(model["gap"]))]


def macro_apply_commands(index: int, model: dict):
    """宏应用命令; "MAC n body p g" 超过 47 字符时拆成 MAC + MTIM 两条."""
    body = ",".join(pedalctl.encode_step(m, k) for m, k in model["steps"])
    press, gap = int(model["press"]), int(model["gap"])
    combined = "MAC %d %s %d %d" % (index, body, press, gap)
    if len(combined) <= pedalctl.REPORT_MAX_TEXT:
        return [combined]
    return ["MAC %d %s" % (index, body), "MTIM %d %d %d" % (index, press, gap)]


# --------------------------------------------------------------------------- #
# UI (需要 tkinter; 以下代码在无显示环境不可用, 由 --self-test 守卫)
# --------------------------------------------------------------------------- #

class StepDialog(_UI_TOPLEVEL_BASE):
    """添加/编辑一个宏步骤: 修饰键勾选 + 按键选择 (+ 窗口内捕获).

    模态: 构造函数阻塞到窗口关闭, 结果在 .result ((mask, key) 或 None)。
    """

    def __init__(self, master, app, step=None):
        super().__init__(master)
        self.app = app
        self.title("宏步骤 / macro step")
        self.transient(master)
        self.resizable(False, False)
        self.result = None
        mask_init, key_init = step if step else (0, 0x1D)

        ttk.Label(self, text="修饰键 / modifiers:").grid(
            row=0, column=0, columnspan=4, sticky="w", padx=6, pady=(6, 0))
        self.mask_vars = {}
        for i, (bit, name) in enumerate(MAIN_MODIFIERS):
            var = tk.BooleanVar(value=bool(mask_init & bit))
            self.mask_vars[bit] = var
            ttk.Checkbutton(self, text=name, variable=var).grid(
                row=1, column=i, sticky="w", padx=6)

        ttk.Label(self, text="按键 / key:").grid(row=2, column=0, sticky="w", padx=6, pady=4)
        self.key_var = tk.StringVar(value=STEP_LABEL_BY_CODE.get(key_init, STEP_LABELS[0]))
        ttk.Combobox(self, textvariable=self.key_var, values=STEP_LABELS,
                     width=24, state="readonly").grid(row=2, column=1, columnspan=3, padx=4)

        ttk.Button(self, text="捕获 / capture", command=self._capture).grid(
            row=3, column=1, sticky="w", pady=2)
        ttk.Button(self, text="确定 / OK", command=self._ok).grid(
            row=4, column=2, sticky="e", padx=4, pady=4)
        ttk.Button(self, text="取消 / cancel", command=self._cancel).grid(
            row=4, column=3, sticky="w")

        self.grab_set()
        self.protocol("WM_DELETE_WINDOW", self._cancel)
        self.wait_window()

    # -- capture ----------------------------------------------------------- #

    def _capture(self):
        self.app.cancel_capture()
        self.bind("<KeyPress>", self._on_capture)
        self.app.set_status("请按下任意键 (Esc 取消) / press any key (Esc cancels) ...")

    def _on_capture(self, event):
        self.unbind("<KeyPress>")
        if event.keysym == "Escape":
            self.app.set_status("捕获已取消 / capture cancelled")
            return
        kind, code = keysym_to_keycode(event.keysym)
        if kind == "key":
            self.key_var.set(STEP_LABEL_BY_CODE.get(code, self.key_var.get()))
            self._set_mask(state_to_mask(event.state))
        elif kind == "mod":
            self._set_mask(state_to_mask(event.state))
        else:
            self.app.set_status("无法识别的键 / unrecognised key: %s" % event.keysym)
            return
        self.app.set_status("就绪 / ready")

    def _set_mask(self, mask):
        for bit, var in self.mask_vars.items():
            var.set(bool(mask & bit))

    # -- buttons ----------------------------------------------------------- #

    def _ok(self):
        mask = 0
        for bit, var in self.mask_vars.items():
            if var.get():
                mask |= bit
        code = STEP_LABEL_TO_CODE.get(self.key_var.get(), 0x00)
        if mask == 0 and code == 0:
            messagebox.showerror(
                "宏步骤 / macro step",
                "空步骤: 至少勾选一个修饰键或选择一个按键\n"
                "empty step: a modifier or a key is required",
                parent=self)
            return
        self.result = (mask, code)
        self.destroy()

    def _cancel(self):
        self.result = None
        self.destroy()


class PedalFrame(_UI_FRAME_BASE):
    """一个踏板的配置面板: 模式 / 单键 / 宏步骤 / 时序 / 应用."""

    def __init__(self, master, index, app):
        super().__init__(master, text="踏板 %d / Pedal %d" % (index, index), padding=6)
        self.index = index
        self.app = app
        self.steps = []            # [(mask, key), ...]
        self._build()

    # -- widgets ----------------------------------------------------------- #

    def _build(self):
        self.columnconfigure(1, weight=1)

        # r0: 模式 (radio 并排, 宏=踩住循环步骤; 详见 README)
        modes = ttk.Frame(self)
        self.mode_var = tk.StringVar(value="single")
        ttk.Radiobutton(modes, text="单键 single", value="single",
                        variable=self.mode_var,
                        command=self._mode_changed).pack(side="left")
        ttk.Radiobutton(modes, text="宏 macro", value="macro",
                        variable=self.mode_var,
                        command=self._mode_changed).pack(side="left", padx=(10, 0))
        modes.grid(row=0, column=0, columnspan=2, sticky="w")

        # r1: 单键: 下拉 + 捕获
        self.key_var = tk.StringVar(value=SINGLE_BY_CODE.get(0x1E, SINGLE_LABELS[0]))
        self.key_combo = ttk.Combobox(self, textvariable=self.key_var,
                                      values=SINGLE_LABELS, width=16, state="readonly")
        self.key_combo.grid(row=1, column=0, sticky="we", pady=2)
        ttk.Button(self, text="捕获", command=self.start_capture).grid(
            row=1, column=1, sticky="e")

        # r2-3: 宏步骤列表 + 编辑按钮(2行x3列网格, 省高度)
        self.step_list = tk.Listbox(self, height=5, width=22, exportselection=False)
        self.step_list.grid(row=2, column=0, rowspan=2, sticky="ns", pady=2)
        self.step_btns = ttk.Frame(self)
        for i, (text, cmd) in enumerate((("添加", self.add_step),
                                         ("编辑", self.edit_step),
                                         ("删除", self.del_step),
                                         ("上移", lambda: self.move_step(-1)),
                                         ("下移", lambda: self.move_step(1)))):
            ttk.Button(self.step_btns, text=text, command=cmd, width=5).grid(
                row=i // 3, column=i % 3, sticky="nsew", padx=1, pady=1)
        self.step_btns.grid(row=2, column=1, rowspan=2, sticky="n", padx=(4, 0), pady=2)

        # r4: 宏时序(一行) + 应用
        timing = ttk.Frame(self)
        self.press_var = tk.StringVar(value=str(DEFAULT_PRESS_MS))
        self.gap_var = tk.StringVar(value=str(DEFAULT_GAP_MS))
        ttk.Label(timing, text="按下").pack(side="left")
        ttk.Spinbox(timing, from_=1, to=pedalctl.MACRO_TIME_MAX, width=4,
                    textvariable=self.press_var).pack(side="left", padx=(2, 8))
        ttk.Label(timing, text="间隔").pack(side="left")
        ttk.Spinbox(timing, from_=1, to=pedalctl.MACRO_TIME_MAX, width=4,
                    textvariable=self.gap_var).pack(side="left", padx=(2, 8))
        ttk.Label(timing, text="ms").pack(side="left")
        timing.grid(row=4, column=0, sticky="w", pady=2)
        ttk.Button(self, text="应用 apply", command=self.apply).grid(
            row=4, column=1, sticky="e")

        self._mode_changed()

    def _mode_changed(self):
        macro = self.mode_var.get() == "macro"
        self.key_combo.configure(state="disabled" if macro else "readonly")
        self.step_list.configure(state="normal" if macro else "disabled")
        for child in self.step_btns.winfo_children():
            child.configure(state="normal" if macro else "disabled")

    # -- model <-> widgets -------------------------------------------------- #

    def fill(self, model):
        """设备配置 (api_* 读取结果) -> 控件."""
        self.mode_var.set("macro" if model.get("mode") == 1 else "single")
        self.key_var.set(SINGLE_BY_CODE.get(model.get("code", 0x1E), SINGLE_LABELS[0]))
        self.steps = list(model.get("steps") or [])
        self.press_var.set(str(model.get("press", DEFAULT_PRESS_MS)))
        self.gap_var.set(str(model.get("gap", DEFAULT_GAP_MS)))
        self._refresh_steps()
        self._mode_changed()

    def collect(self):
        """控件 -> 设备配置 model (README.md 第 7 节)."""
        if self.mode_var.get() == "macro":
            return {"mode": 1, "code": 0,
                    "steps": list(self.steps),
                    "press": self._timing(self.press_var, DEFAULT_PRESS_MS),
                    "gap": self._timing(self.gap_var, DEFAULT_GAP_MS)}
        return {"mode": 0,
                "code": SINGLE_LABEL_TO_CODE.get(self.key_var.get(), 0x00),
                "steps": [],
                "press": DEFAULT_PRESS_MS, "gap": DEFAULT_GAP_MS}

    @staticmethod
    def _timing(var, default):
        try:
            value = int(var.get())
        except ValueError:
            return default
        return max(1, min(pedalctl.MACRO_TIME_MAX, value))

    # -- macro step editing -------------------------------------------------- #

    def _refresh_steps(self):
        self.step_list.delete(0, "end")
        for i, (mask, key) in enumerate(self.steps, 1):
            self.step_list.insert("end", "%d. %s" % (i, step_text(mask, key)))
        if self.steps:
            self.step_list.selection_clear(0, "end")
            self.step_list.selection_set(len(self.steps) - 1)
            self.step_list.see(len(self.steps) - 1)

    def add_step(self):
        if len(self.steps) >= pedalctl.MACRO_MAX_STEPS:
            messagebox.showinfo(
                "宏步骤 / macro steps",
                "每踏板最多 %d 步 / at most %d steps per pedal"
                % (pedalctl.MACRO_MAX_STEPS, pedalctl.MACRO_MAX_STEPS))
            return
        dialog = StepDialog(self, self.app)
        if dialog.result is not None:
            self.steps.append(dialog.result)
            self._refresh_steps()

    def edit_step(self):
        sel = self.step_list.curselection()
        if not sel:
            return
        i = sel[0]
        dialog = StepDialog(self, self.app, step=self.steps[i])
        if dialog.result is not None:
            self.steps[i] = dialog.result
            self._refresh_steps()

    def del_step(self):
        sel = self.step_list.curselection()
        if not sel:
            return
        del self.steps[sel[0]]
        self._refresh_steps()

    def move_step(self, delta):
        sel = self.step_list.curselection()
        if not sel:
            return
        i = sel[0]
        j = i + delta
        if 0 <= j < len(self.steps):
            self.steps[i], self.steps[j] = self.steps[j], self.steps[i]
            self._refresh_steps()
            self.step_list.selection_set(j)

    # -- capture (App 捕获回调, 规则见 README.md 6.3) ------------------------ #

    def start_capture(self):
        self.app.begin_capture(self)

    def apply_captured(self, kind, code, mask):
        if self.mode_var.get() == "macro":
            if len(self.steps) >= pedalctl.MACRO_MAX_STEPS:
                self.app.set_status("宏最多 %d 步 / at most %d steps"
                                    % (pedalctl.MACRO_MAX_STEPS, pedalctl.MACRO_MAX_STEPS))
                return
            if kind == "mod":
                self.steps.append((MOD_CODE_TO_BIT[code], 0x00))
            else:
                self.steps.append((mask, code))
            self._refresh_steps()
        else:
            self.key_var.set(SINGLE_BY_CODE.get(code, self.key_var.get()))

    def apply(self):
        self.app.apply_pedals([self.index])


class App(_UI_APP_BASE):
    """主窗口: 连接状态 / 三个踏板面板 / 全局操作 / 日志.

    所有设备 I/O 在 worker 线程执行 (self._io_lock 串行化), 结果经
    after(0) 回到主线程更新控件 (README.md 第 8 节)。
    """

    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.minsize(940, 480)
        self.device = None
        self.hid = pedalctl.load_hidapi()
        self.capture_frame = None
        self._io_lock = threading.Lock()
        self._build()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(150, self.connect)

    # -- widgets ----------------------------------------------------------- #

    def _build(self):
        top = ttk.Frame(self, padding=(8, 6))
        top.pack(fill="x")
        self.led = tk.Canvas(top, width=14, height=14, highlightthickness=0)
        self.led.create_oval(2, 2, 12, 12, fill="#c0392b", outline="", tags="dot")
        self.led.pack(side="left", padx=(0, 6))
        ttk.Button(top, text="重新连接 / reconnect",
                   command=self.connect).pack(side="right")
        self.status_var = tk.StringVar(value="未连接 / not connected")
        ttk.Label(top, textvariable=self.status_var).pack(side="left")
        self.info_var = tk.StringVar(value="")
        ttk.Label(top, textvariable=self.info_var).pack(side="left", padx=(16, 0))
        self.dirty_var = tk.StringVar(value="")
        self.dirty_label = ttk.Label(top, textvariable=self.dirty_var,
                                     foreground="#c0392b")
        self.dirty_label.pack(side="left", padx=(16, 0))

        # 三踏板横排: 用宽度换高度
        body = ttk.Frame(self)
        body.pack(fill="x", padx=8)
        self.frames = []
        for i, index in enumerate((1, 2, 3)):
            frame = PedalFrame(body, index, self)
            frame.grid(row=0, column=i, sticky="nsew", padx=(0 if i == 0 else 4, 0))
            body.columnconfigure(i, weight=1)
            self.frames.append(frame)

        bar = ttk.Frame(self, padding=(8, 4))
        bar.pack(fill="x")
        for text, cmd in (("读取设备配置 / read", self.read_config),
                          ("全部应用 / apply all", self.apply_all),
                          ("保存到 Flash / save", self.save),
                          ("恢复默认 / reset", self.reset)):
            ttk.Button(bar, text=text, command=cmd).pack(side="left", padx=3)

        self.log_text = tk.Text(self, height=6, wrap="word", state="disabled")
        self.log_text.pack(fill="both", expand=True, padx=8, pady=(0, 8))

    # -- status / log -------------------------------------------------------- #

    def set_status(self, text):
        self.status_var.set(text)

    def log(self, text):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", text.rstrip() + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _led(self, color):
        self.led.itemconfigure("dot", fill=color)

    # -- async plumbing (README.md 第 8 节) --------------------------------- #

    def run_async(self, fn, on_done):
        def worker():
            try:
                result = fn()
            except Exception as exc:            # noqa: BLE001 - surfaced in UI
                self.after(0, lambda e=exc: self._async_error(e))
            else:
                self.after(0, lambda r=result: on_done(r))
        threading.Thread(target=worker, daemon=True).start()

    def _async_error(self, exc):
        self.log("[ERR] %s: %s" % (type(exc).__name__, exc))
        self.set_status("通信错误 / IO error: %s" % (exc,))

    # -- connection ----------------------------------------------------------- #

    def connect(self):
        if self.device is not None:
            try:
                self.device.close()
            except Exception:
                pass
            self.device = None
        self._led("#c0392b")
        self.set_status("正在连接设备 / connecting ...")
        self.run_async(self._connect_worker, self._connected)

    def _connect_worker(self):
        chatter = io.StringIO()
        with contextlib.redirect_stdout(chatter):
            with self._io_lock:
                device, _code = pedalctl.open_device(self.hid)
        return device, chatter.getvalue()

    def _connected(self, result):
        device, chatter = result
        for line in chatter.strip().splitlines():
            self.log("    " + line)
        if device is None:
            self.device = None
            self._led("#c0392b")
            self.set_status("未连接 / not connected")
            return
        self.device = device
        self._led("#27ae60")
        self.log("[OK] 设备已连接 / device connected")
        self.read_config()

    # -- read ------------------------------------------------------------------ #

    def read_config(self):
        if self.device is None:
            self.set_status("未连接 / not connected")
            return
        self.set_status("正在读取 / reading ...")
        self.run_async(self._read_worker, self._read_done)

    def _read_worker(self):
        with self._io_lock:
            info = pedalctl.api_info(self.device)
            keys = pedalctl.api_map(self.device)
            per = []
            for pedal in (1, 2, 3):
                steps = pedalctl.api_steps(self.device, pedal)
                ok, timing, _msg = pedalctl.api_timing(self.device, pedal)
                per.append({"steps": steps if steps is not None else [],
                            "timing": timing if ok else (DEFAULT_PRESS_MS, DEFAULT_GAP_MS)})
            return info, keys, per

    def _read_done(self, result):
        info, keys, per = result
        if info:
            self._info_done(info)
        if keys is None:
            self.log("[ERR] 读取映射失败 / read failed")
            self.set_status("读取失败 / read failed")
            return
        for i, key in enumerate(keys):
            timing = per[i]["timing"]
            if key.upper() == "M":
                model = {"mode": 1, "code": 0x1E, "steps": per[i]["steps"],
                         "press": timing[0], "gap": timing[1]}
            else:
                model = {"mode": 0, "code": int(key, 16), "steps": [],
                         "press": timing[0], "gap": timing[1]}
            self.frames[i].fill(model)
        self.log("[OK] 已读取 / read: %s" % " ".join(keys))
        self.set_status("已读取 / read")

    # -- apply ------------------------------------------------------------------ #

    def apply_all(self):
        self.apply_pedals([1, 2, 3])

    def apply_pedals(self, indices):
        if self.device is None:
            self.set_status("未连接 / not connected")
            return
        jobs = [(f.index, f.collect()) for f in self.frames if f.index in indices]
        self.set_status("正在应用 / applying ...")
        self.run_async(lambda: self._apply_worker(jobs), self._apply_done)

    def _apply_worker(self, jobs):
        with self._io_lock:
            out = []
            for index, model in jobs:
                if model["mode"] == 0:
                    ok, msg = pedalctl.api_set(self.device, index, model["code"])
                elif not model["steps"]:
                    ok, msg = False, "宏至少需要一步 / a macro needs at least one step"
                else:
                    ok, msg = self._apply_macro(index, model)
                out.append((index, ok, msg))
            return out

    def _apply_macro(self, index, model):
        """MAC (含时序); 47 字符装不下时拆成 MAC + MTIM (README.md 5.4)."""
        steps = model["steps"]
        press, gap = int(model["press"]), int(model["gap"])
        body = ",".join(pedalctl.encode_step(m, k) for m, k in steps)
        combined = "MAC %d %s %d %d" % (index, body, press, gap)
        if len(combined) <= pedalctl.REPORT_MAX_TEXT:
            ok, msg, _body = pedalctl.api_mac(self.device, index, steps, press, gap)
            return ok, msg
        ok, msg, _body = pedalctl.api_mac(self.device, index, steps)
        if not ok:
            return ok, msg
        ok2, _values, msg2 = pedalctl.api_timing(self.device, index, press, gap)
        return ok2, msg2

    def _apply_done(self, results):
        bad = 0
        for index, ok, msg in results:
            if ok:
                self.log("[OK] 踏板 %d / pedal %d: %s" % (index, index, msg))
            else:
                bad += 1
                self.log("[ERR] 踏板 %d / pedal %d: %s" % (index, index, msg))
        self.set_status("已应用 / applied" if bad == 0
                        else "应用有失败 / apply failures: %d" % bad)
        self.run_async(self._info_worker, self._info_done)

    # -- save / reset / info ------------------------------------------------------ #

    def save(self):
        if self.device is None:
            self.set_status("未连接 / not connected")
            return
        self.set_status("正在写入 Flash / saving ...")
        self.run_async(self._save_worker, self._save_done)

    def _save_worker(self):
        with self._io_lock:
            return pedalctl.api_save(self.device)

    def _save_done(self, result):
        ok, msg = result
        if ok:
            self.log("[OK] 已保存到 Flash / saved: %s" % msg)
            self.set_status("已保存 / saved")
            self.run_async(self._info_worker, self._info_done)
        else:
            self.log("[ERR] 保存失败 / save failed: %s" % msg)
            self.set_status("保存失败 / save failed: %s" % msg)

    def reset(self):
        if self.device is None:
            self.set_status("未连接 / not connected")
            return
        if not messagebox.askyesno(
                "恢复默认 / reset",
                "恢复全部默认 (键位 1/2/3, 无宏, 时序 30/30, 仅 RAM)?\n"
                "restore all defaults (keys 1/2/3, no macros, RAM only)?"):
            return
        self.run_async(self._reset_worker, self._reset_done)

    def _reset_worker(self):
        with self._io_lock:
            return pedalctl.api_reset(self.device)

    def _reset_done(self, result):
        ok, msg = result
        if ok:
            self.log("[OK] 已恢复默认 / reset: %s" % msg)
            self.read_config()
        else:
            self.log("[ERR] reset: %s" % msg)

    def _info_worker(self):
        with self._io_lock:
            return pedalctl.api_info(self.device)

    def _info_done(self, info):
        if not info:
            self.info_var.set("")
            self.dirty_var.set("")
            return
        self.info_var.set("固件 / firmware: %s" % info["fw"])
        if info["dirty"]:
            self.dirty_var.set("● 未保存 / unsaved")
            self.dirty_label.configure(foreground="#c0392b")
        else:
            self.dirty_var.set("已保存 / saved")
            self.dirty_label.configure(foreground="#7f8c8d")

    # -- capture (规则见 README.md 6.3) --------------------------------------------- #

    def begin_capture(self, frame):
        self.cancel_capture()
        self.capture_frame = frame
        self.set_status("请按下任意键 (Esc 取消) / press any key (Esc cancels) ...")
        self.bind_all("<KeyPress>", self._on_capture_key)

    def cancel_capture(self):
        if self.capture_frame is not None:
            self.capture_frame = None
            try:
                self.unbind_all("<KeyPress>")
            except Exception:
                pass

    def _on_capture_key(self, event):
        frame = self.capture_frame
        self.cancel_capture()
        if frame is None:
            return
        if event.keysym == "Escape":
            self.set_status("捕获已取消 / capture cancelled")
            return
        kind, code = keysym_to_keycode(event.keysym)
        if kind is None:
            self.set_status("无法识别的键 / unrecognised key: %s" % event.keysym)
            return
        mask = state_to_mask(event.state)
        if kind == "mod":
            frame.apply_captured("mod", code, 0)
            self.set_status("已捕获 / captured: %s" % code_label(code))
            return
        frame.apply_captured("key", code, mask)
        label = code_label(code)
        if frame.mode_var.get() != "macro" and mask:
            self.log("[!] 检测到修饰键 %s: 单键模式只写入 %s; 组合键请用宏模式 / "
                     "modifiers ignored in single mode; use a macro for chords"
                     % (mask_text(mask), label))
            self.set_status("已捕获 / captured: %s (修饰键已忽略 / modifiers ignored)" % label)
        elif mask:
            self.set_status("已捕获 / captured: %s + %s" % (mask_text(mask), label))
        else:
            self.set_status("已捕获 / captured: %s" % label)

    # -- close ------------------------------------------------------------------------ #

    def _on_close(self):
        self.cancel_capture()
        if self.device is not None:
            try:
                self.device.close()
            except Exception:
                pass
        self.destroy()


# --------------------------------------------------------------------------- #
# --self-test: 纯逻辑回归, 无需 tkinter / 显示 / 设备 (README.md 第 7 节)
# --------------------------------------------------------------------------- #

def _self_test() -> int:
    checks = 0
    failures = 0

    def check(name, got, want):
        nonlocal checks, failures
        checks += 1
        ok = got == want
        if not ok:
            failures += 1
        print("%s %-46s got=%r want=%r" % ("PASS" if ok else "FAIL", name, got, want))

    # keysym -> (kind, code)
    check("keysym 'z'", keysym_to_keycode("z"), ("key", 0x1D))
    check("keysym 'Z' (shift held)", keysym_to_keycode("Z"), ("key", 0x1D))
    check("keysym '1'", keysym_to_keycode("1"), ("key", 0x1E))
    check("keysym '0'", keysym_to_keycode("0"), ("key", 0x27))
    check("keysym 'F5'", keysym_to_keycode("F5"), ("key", 0x3E))
    check("keysym 'F12'", keysym_to_keycode("F12"), ("key", 0x45))
    check("keysym 'Return'", keysym_to_keycode("Return"), ("key", 0x28))
    check("keysym 'Escape'", keysym_to_keycode("Escape"), ("key", 0x29))
    check("keysym 'Tab'", keysym_to_keycode("Tab"), ("key", 0x2B))
    check("keysym 'BackSpace'", keysym_to_keycode("BackSpace"), ("key", 0x2A))
    check("keysym 'space'", keysym_to_keycode("space"), ("key", 0x2C))
    check("keysym 'KP_3'", keysym_to_keycode("KP_3"), ("key", 0x20))
    check("keysym 'KP_Enter'", keysym_to_keycode("KP_Enter"), ("key", 0x28))
    check("keysym 'comma'", keysym_to_keycode("comma"), ("key", 0x36))
    check("keysym 'minus'", keysym_to_keycode("minus"), ("key", 0x2D))
    check("keysym 'Shift_L'", keysym_to_keycode("Shift_L"), ("mod", 0xE1))
    check("keysym 'Control_R'", keysym_to_keycode("Control_R"), ("mod", 0xE4))
    check("keysym 'Alt_L'", keysym_to_keycode("Alt_L"), ("mod", 0xE2))
    check("keysym 'Super_L'", keysym_to_keycode("Super_L"), ("mod", 0xE3))
    check("keysym 'Pause' unsupported", keysym_to_keycode("Pause"), (None, 0))
    check("keysym 'Capital' unsupported", keysym_to_keycode("Capital"), (None, 0))

    # event.state -> modifier mask (Lock/NumLock ignored)
    check("state ctrl", state_to_mask(0x0004), 0x01)
    check("state shift", state_to_mask(0x0001), 0x02)
    check("state alt", state_to_mask(0x0008), 0x04)
    check("state gui", state_to_mask(0x0040), 0x08)
    check("state ctrl+shift", state_to_mask(0x0005), 0x03)
    check("state ctrl+alt+gui", state_to_mask(0x004C), 0x0D)
    check("state capslock ignored", state_to_mask(0x0002), 0x00)
    check("state numlock ignored", state_to_mask(0x0010), 0x00)

    # step / code labels
    check("step_text modifier-only", step_text(0x02, 0x00), "LShift")
    check("step_text plain key", step_text(0x00, 0x1D), "z (0x1D)")
    check("step_text chord", step_text(0x02, 0x1D), "LShift + z (0x1D)")
    check("step_text ctrl+shift+a", step_text(0x03, 0x04), "LCtrl + LShift + a (0x04)")
    check("step_text punct", step_text(0x00, 0x36), ", (0x36)")
    check("mask_text", mask_text(0x05), "LCtrl+LAlt")

    # model -> commands
    check("model single", model_to_commands(1, {"mode": 0, "code": 0x04, "steps": [],
                                                "press": 30, "gap": 30}), ["SET 1 0x04"])
    check("model macro", model_to_commands(
        1, {"mode": 1, "code": 0, "steps": [(0x02, 0), (0, 0x1D), (0, 0x06)],
            "press": 15, "gap": 25}), ["MAC 1 02:00,1D,06 15 25"])

    # length-aware split (47-char budget)
    check("macro combined fits", macro_apply_commands(
        2, {"mode": 1, "steps": [(0, 0x1D)], "press": 30, "gap": 30}),
        ["MAC 2 1D 30 30"])
    check("macro split when long", macro_apply_commands(
        2, {"mode": 1, "steps": [(0x02, 0x1D)] * 6, "press": 100, "gap": 100}),
        ["MAC 2 02:1D,02:1D,02:1D,02:1D,02:1D,02:1D", "MTIM 2 100 100"])
    check("macro 6 chords + short timing fits", len(macro_apply_commands(
        1, {"mode": 1, "steps": [(0x02, 0x1D)] * 6, "press": 15, "gap": 15})), 1)

    # choice tables round trip
    check("single: disable entry", SINGLE_BY_CODE.get(0x00), "禁用 Disabled")
    check("single: modifier entry", SINGLE_BY_CODE.get(0xE1), "LShift (0xE1)")
    check("single: round trip", all(SINGLE_BY_CODE[SINGLE_LABEL_TO_CODE[label]] == label
                                    for label in SINGLE_LABELS), True)
    check("single: unique codes", len(SINGLE_LABEL_TO_CODE), len(SINGLE_CHOICES))
    check("step: modifier-only first", STEP_LABEL_BY_CODE.get(0x00),
          "（仅修饰键 / modifier-only）")
    check("code_label modifier", code_label(0xE4), "RCtrl")
    check("code_label punct", code_label(0x30), "]")
    check("code_label unknown", code_label(0x55), "0x55")

    print("\n%d checks, %d failures" % (checks, failures))
    if failures == 0:
        print("ALL PASS")
        return 0
    print("FAILED %d" % failures)
    return 1


def _reconfigure_streams():
    """Windows 控制台中文输出 / utf-8 console output."""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def main(argv=None) -> int:
    _reconfigure_streams()
    args = list(sys.argv[1:] if argv is None else argv)
    if "--self-test" in args:
        return _self_test()
    if tk is None:
        sys.stderr.write(
            "tkinter 不可用 / tkinter unavailable.\n"
            "  Windows: 使用 python.org 的 Python 3.9+ (安装包自带 tkinter)\n"
            "  Debian/Ubuntu: sudo apt install python3-tk\n")
        return 2
    App().mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
