#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""win_remap.py - Windows 友好的改键助手 / Windows-friendly remapping helper.

很薄的前端 / thin front-end:协议、设备枚举与厂商接口选择全部复用同目录的
pedalctl.py,这里不重复实现任何协议逻辑 / the protocol, device enumeration
and vendor-interface selection are all reused from pedalctl.py in this same
directory - no protocol logic is duplicated here.

无参数 / no arguments:
    运行中文诊断向导 / run the Chinese-language diagnostic guide:
    环境信息 -> hidapi 状态 -> 枚举 0x1209:0x0001 条目 -> `--probe` 探测
    -> 给出可直接复制的后续命令。
    退出码 / exit codes: 0=设备应答, 2=设备未应答 / 0=device answered,
    2=device did not answer.

带参数 / with arguments:
    原样转发给 pedalctl.py / forwarded verbatim, so
        python win_remap.py set 1 0x04
    behaves exactly like
        python pedalctl.py set 1 0x04

用法 / usage:
    cd host_test
    uv run python win_remap.py              # 诊断向导 / diagnostic guide
    uv run python win_remap.py set 1 0x04   # 改键 / remap pedal 1 to 'a'
    uv run python win_remap.py get
    uv run python win_remap.py save         # 持久化 / persist to flash
    uv run python win_remap.py reset        # 恢复 1/2/3 / restore defaults
"""

from __future__ import annotations

import os
import platform
import sys

# --------------------------------------------------------------------------- #
# 复用同目录的 pedalctl.py / reuse pedalctl.py from this script's own directory.
# 这样从仓库根目录 (`python host_test/win_remap.py`) 或从 host_test/ 内部
# (`python win_remap.py`) 运行都能正确导入 / works when run from the repo root
# or from inside host_test/.
# --------------------------------------------------------------------------- #
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

try:
    import pedalctl
except Exception as exc:  # never crash: report and exit 2
    print("[ERR] 无法导入同目录的 pedalctl.py / cannot import pedalctl.py: %s: %s"
          % (type(exc).__name__, exc))
    print("      请确认 win_remap.py 与 pedalctl.py 在同一目录 / "
          "keep win_remap.py next to pedalctl.py")
    sys.exit(2)

REMEDY_MISSING_HIDAPI = """\
hidapi 未安装 / hidapi is not installed:
  * Windows 上必须安装 (没有 /dev/hidraw 后备通道) / REQUIRED on Windows (no /dev/hidraw fallback):
      cd host_test && uv run python win_remap.py     (uv 会自动安装 / uv installs it)
      py -m pip install hidapi                       (不用 uv 时 / without uv)
  * Linux 上可选: 未安装时会回退到 /dev/hidraw ioctl / optional on Linux (falls back to /dev/hidraw)
  * macOS 上同样需要 hidapi / also required on macOS"""

NEXT_COMMANDS = """\
     下一步命令 / next commands:
       uv run python win_remap.py set 1 0x04   # 踏板1 -> 键码 0x04 (即 'a')
       uv run python win_remap.py get          # 读取当前映射 / read current mapping
       uv run python win_remap.py save         # 写入 Flash 才会持久化 / persist to flash
       uv run python win_remap.py reset        # 恢复默认 1/2/3 (仅 RAM) / restore defaults (RAM)
     注意 / note: 改键后必须 save 才能跨重启保留; reset + save 恢复默认 1/2/3 /
     run save to persist; reset followed by save restores 1/2/3."""


def _reconfigure_streams() -> None:
    """UTF-8 console output on Windows / 避免中文在 Windows 控制台乱码或报错。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def _fmt_hex(value, width: int = 4) -> str:
    if value is None:
        return "?"
    try:
        return "0x%0*X" % (width, int(value))
    except (TypeError, ValueError):
        return str(value)


def _print_environment():
    """打印 OS/Python/hidapi 状态, 返回 hidapi 模块或 None。"""
    hid_module = pedalctl.load_hidapi()
    print("== 环境 / environment ==")
    print("  操作系统 / OS : %s %s" % (platform.system(), platform.release()))
    print("  Python       : %s" % sys.version.split()[0])
    if hid_module is not None:
        print("  hidapi       : 已安装 / available (`import hid` OK)")
    else:
        print("  hidapi       : 未安装 / missing (`import hid` failed)")
    return hid_module


def _list_entries(hid_module) -> bool:
    """枚举 0x1209:0x0001 的 hidapi 条目并标出厂商接口, 返回是否存在厂商条目。"""
    print("== hidapi 条目 / hidapi entries (VID 0x%04X / PID 0x%04X) =="
          % (pedalctl.VENDOR_ID, pedalctl.PRODUCT_ID))
    try:
        entries = list(hid_module.enumerate(pedalctl.VENDOR_ID, pedalctl.PRODUCT_ID) or [])
    except Exception as exc:
        print("  枚举失败 / enumerate() failed: %s: %s" % (type(exc).__name__, exc))
        return False

    if not entries:
        print("  (无条目 / no entries) - 设备未插入或驱动未就绪 / "
              "device unplugged or driver not ready")
        return False

    has_vendor = False
    for entry in entries:
        path = entry.get("path")
        if isinstance(path, (bytes, bytearray)):
            path = bytes(path).decode("utf-8", "replace")
        is_vendor = entry.get("usage_page") == pedalctl.VENDOR_USAGE_PAGE
        has_vendor = has_vendor or is_vendor
        print("  - usage_page=%s usage=%s interface_number=%s path=%s%s"
              % (_fmt_hex(entry.get("usage_page"), 4),
                 _fmt_hex(entry.get("usage"), 2),
                 entry.get("interface_number"),
                 path,
                 "   <= 厂商/改键接口" if is_vendor else ""))

    if not has_vendor:
        print("[!] 没有任何条目 usage_page=0xFF00 / no entry exposes usage_page=0xFF00:")
        print("    固件可能不是双接口版本(改键需要接口1的厂商集合) / the firmware may not be "
              "the two-interface build (remapping needs the vendor collection on interface 1)")
    return has_vendor


def _guided() -> int:
    """无参数向导 / no-argument guided flow. 返回 0 (设备应答) 或 2 (未应答)。"""
    hid_module = _print_environment()

    if hid_module is None:
        print()
        print(REMEDY_MISSING_HIDAPI)
        print()
        print("提示 / note: 未安装 hidapi 时仍会尝试 Linux /dev/hidraw 后备通道 / "
              "without hidapi the Linux /dev/hidraw fallback is still attempted below")
    else:
        print()
        _list_entries(hid_module)

    print()
    print("== 探测设备 / probe ==")
    code = pedalctl.main(["--probe"])

    if code == 0:
        print()
        print("[OK] 设备已应答, 可以开始改键 / device answered; ready to remap")
        print(NEXT_COMMANDS)
        return 0

    print()
    print("[ERR] 设备未应答 (probe 退出码 %d) / the device did not answer "
          "(probe exit code %d)" % (code, code))
    print("     排查 / checklist:")
    print("       * USB 线插在本机而不是虚拟机 / USB attached to THIS machine, not a VM")
    print("       * Windows 必须安装 hidapi (见上方提示) / hidapi is REQUIRED on Windows "
          "(see the hint above)")
    print("       * 设备应枚举出两个 HID 接口, VID:PID = 1209:0001 / two HID interfaces "
          "expected with VID:PID 1209:0001")
    print("       * Linux 下可用 python3 pedalctl.py --list 检查节点与权限 / on Linux use "
          "`--list` to inspect hidraw nodes & permissions")
    return 2


def main() -> int:
    _reconfigure_streams()
    if len(sys.argv) > 1:
        # 带参数: 原样转发 / with arguments: forward verbatim to pedalctl.py
        return pedalctl.main(sys.argv[1:])
    return _guided()


if __name__ == "__main__":
    sys.exit(main())
