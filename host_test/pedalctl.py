#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pedalctl.py - host-side CLI for the footswitch keyboard HID Feature-report channel.

Device / 设备
    VID 0x1209, PID 0x0001.  The vendor Feature report (usage page 0xFF00,
    usage 0x01) lives in the SAME USB interface as the boot keyboard and there
    are no report IDs.

Protocol / 协议
    48-byte ASCII commands -> 48-byte ASCII responses (NUL padded):
        SET <1-3> <0xNN>   ->  "OK SET n=0xNN"
        GET                ->  "OK MAP 0xNN 0xNN 0xNN"
        SAVE               ->  "OK saving", later "OK saved" (poll)
        RESET              ->  "OK RESET"
        INFO               ->  "OK INFO ..."
        errors start with "ERR".
    HID Get/Set_Report(Feature): wValue=0x0300, wIndex=interface 0, wLength=48.
    hidapi buffers include a leading report-ID byte, so 49-byte buffers are used
    and a leading 0x00 is stripped from responses.

Opening strategy / 打开策略
    1) If the OPTIONAL `import hid` (cython-hidapi / PyPI `hid`) succeeds:
       enumerate(0x1209, 0x0001), pick usage_page == 0xFF00 (and usage == 0x01
       when present), open by path.  If the vendor collection is missing, every
       enumerated entry is dumped and the script exits 2.
    2) Otherwise fall back to a raw Linux hidraw ioctl (HIDIOCSFEATURE /
       HIDIOCGFEATURE with the report-ID-0 buffer), finding nodes via
       /sys/class/hidraw/*/device/uevent (HID_ID=...00001209:00000001...).

macOS caveat / macOS 注意
    Because the vendor collection shares the boot-keyboard interface, macOS's
    hidapi opens the device exclusively by default and seizing a keyboard device
    needs root.  Workarounds:
      1) use the PyPI `hidapi` binding with hid_darwin_set_open_exclusive(0);
      2) run this script as root;
      3) ask for the separate-interface firmware variant.

Python 3 stdlib only, except the OPTIONAL `import hid`.
"""

from __future__ import annotations

import argparse
import ctypes
import errno
import fcntl
import glob
import os
import re
import sys
import time

VENDOR_ID = 0x1209
PRODUCT_ID = 0x0001
VENDOR_USAGE_PAGE = 0xFF00
VENDOR_USAGE = 0x01
REPORT_SIZE = 48                 # 48-byte ASCII payload
BUF_SIZE = REPORT_SIZE + 1       # +1 leading report-ID byte (0x00) for hidapi/hidraw
DEFAULT_RETRIES = 8
DEFAULT_DELAY = 0.25
TARGET_HID_ID = "%08X:%08X" % (VENDOR_ID, PRODUCT_ID)   # "00001209:00000001"

# --------------------------------------------------------------------------- #
# Linux hidraw ioctls, computed with _IOC-equivalent arithmetic.
#   _IOC(dir, type, nr, size) = dir<<30 | size<<16 | type<<8 | nr
#   HIDIOCSFEATURE(49) = 0xC0314806
#   HIDIOCGFEATURE(49) = 0xC0314807
# --------------------------------------------------------------------------- #
_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = 8
_IOC_SIZESHIFT = 16
_IOC_DIRSHIFT = 30
_IOC_WRITE = 1
_IOC_READ = 2


def _ioc(direction: int, type_char: str, nr: int, size: int) -> int:
    return ((direction << _IOC_DIRSHIFT)
            | (ord(type_char) << _IOC_TYPESHIFT)
            | (nr << _IOC_NRSHIFT)
            | (size << _IOC_SIZESHIFT))


HIDIOCSFEATURE = _ioc(_IOC_WRITE | _IOC_READ, "H", 0x06, BUF_SIZE)
HIDIOCGFEATURE = _ioc(_IOC_WRITE | _IOC_READ, "H", 0x07, BUF_SIZE)

HINT_PERMISSION = """\
权限不足 / permission denied. 可尝试 / try:
  * 加入 input 组 / add user to input group:  sudo usermod -aG input $USER
    然后重新登录 (logout/login) 使组权限生效。
  * 或添加 udev 规则 / or add a udev rule: /etc/udev/rules.d/99-footswitch.rules
      SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", MODE="0660", GROUP="input", TAG+="uaccess"
    然后执行: sudo udevadm control --reload-rules && sudo udevadm trigger
  * 或临时以 root 运行 / or run as root:  sudo python3 pedalctl.py ..."""

HINT_NOT_FOUND = """\
请检查 / please check:
  * USB 线插在“本机”而不是虚拟机 / the USB device is attached to THIS machine, not a VM
    (宿主机的 USB 设备在 VM 里看不到 / a host USB device is invisible inside a VM)
  * 设备已上电, VID:PID 应为 1209:0001 / device powered, VID:PID must be 1209:0001
  * Linux 下可用 --list 查看 hidraw 节点/权限 / use --list to inspect hidraw nodes & permissions
  * 若 --list 能看到节点但打不开, 多半是权限问题 (见下方提示) / if the node exists but cannot
    be opened it is most likely a permission problem (see the hint below)"""

EPILOG = """\
示例 / examples:
  pedalctl.py --probe                检查设备是否在线 / check the device is reachable
  pedalctl.py --list                 列出 HID/hidraw 候选 / list HID/hidraw candidates
  pedalctl.py set 1 0x2C             踏板1 -> HID 键码 0x2C / pedal 1 -> HID code 0x2C
  pedalctl.py get                    读取当前映射 / read current mapping
  pedalctl.py save                   写入 Flash / persist to flash
  pedalctl.py reset                  恢复默认 / restore defaults
  pedalctl.py info                   设备信息 / device info

协议 / protocol:
  48 字节 ASCII Feature report 命令 (wValue=0x0300, wIndex=0, wLength=48);
  hidapi/hidraw 缓冲带首字节 report ID, 因此实际使用 49 字节。
  响应为 48 字节 ASCII, NUL 填充。save/get 会自动轮询 (约 8 次 x 250ms)。

退出码 / exit codes: 0=OK, 1=设备返回 ERR, 2=设备/传输失败 (device/transport failure)

macOS 注意 / macOS caveat:
  厂商 (0xFF00) Feature 集合与 boot keyboard 共用同一个 USB 接口, macOS 上 hidapi
  默认以独占方式打开设备, 抢占键盘设备需要 root 权限 / because the vendor collection
  shares the boot-keyboard interface, macOS hidapi opens exclusively by default and
  seizing a keyboard device needs root. 三种方案 / three workarounds:
    1) 安装 PyPI `hidapi` 绑定并先调用 hid_darwin_set_open_exclusive(0) 再打开;
    2) 以 root 运行 / run as root:            sudo python3 pedalctl.py ...
    3) 使用厂商集合独立接口的固件版本 / use the separate-interface firmware variant.
"""

MAP_RE = re.compile(
    r"^OK\s+MAP\s+(0x[0-9A-Fa-f]{2})\s+(0x[0-9A-Fa-f]{2})\s+(0x[0-9A-Fa-f]{2})\s*$"
)


# --------------------------------------------------------------------------- #
# Small helpers
# --------------------------------------------------------------------------- #
def _reconfigure_streams() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def _err(message: str) -> str:
    return "[ERR] " + message


def _ok(message: str) -> str:
    return "[OK] " + message


def _warn(message: str) -> str:
    return "[!] " + message


def _hex_or_none(value) -> str:
    if value is None:
        return "None"
    try:
        return "0x%04X" % int(value)
    except (TypeError, ValueError):
        return str(value)


def format_entry(entry: dict) -> str:
    """One hidapi enumerate() entry as a debug-friendly line."""
    path = entry.get("path")
    if isinstance(path, (bytes, bytearray)):
        path = bytes(path).decode("utf-8", "replace")
    return ("path=%s usage_page=%s usage=%s interface_number=%s"
            % (path, _hex_or_none(entry.get("usage_page")),
               _hex_or_none(entry.get("usage")), entry.get("interface_number")))


def _looks_like_permission_error(exc: BaseException) -> bool:
    if isinstance(exc, PermissionError):
        return True
    if isinstance(exc, OSError) and getattr(exc, "errno", None) in (errno.EACCES, errno.EPERM):
        return True
    text = str(exc).lower()
    return any(token in text for token in ("permission", "access denied", "eacces", "not permitted"))


def normalize_response(data) -> bytes:
    """Strip the leading report-ID byte (0x00) from a 49-byte feature buffer."""
    if data is None:
        return b""
    if isinstance(data, str):
        data = data.encode("utf-8", "replace")
    raw = bytes(data)
    if len(raw) > REPORT_SIZE and raw[0] == 0x00:
        raw = raw[1:]
    return raw


def decode_text(data) -> str:
    raw = normalize_response(data)
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace").strip()


def classify(text: str) -> int:
    """Map a device response to an exit code: OK->0, ERR->1, anything->2."""
    stripped = text.strip()
    if stripped.startswith("OK"):
        return 0
    if stripped.startswith("ERR"):
        return 1
    return 2


def show(raw: bytes, text: str) -> None:
    print("响应 / response: %s" % (text if text else "(空 / empty)"))
    print("raw: %r" % (raw,))


# --------------------------------------------------------------------------- #
# Backends
# --------------------------------------------------------------------------- #
class HidapiDevice:
    """cython-hidapi / PyPI `hid` backend."""

    backend = "hidapi"

    def __init__(self, hid_module, path) -> None:
        self.path = path
        self._module = hid_module
        self._device = hid_module.device()
        self._device.open_path(path)

    def send_feature(self, payload: bytes) -> None:
        buf = bytes([0x00]) + payload
        try:
            self._device.send_feature_report(buf)
        except TypeError:
            # Older cython-hidapi releases only accept a list of ints.
            self._device.send_feature_report(list(buf))

    def get_feature(self):
        return self._device.get_feature_report(0x00, BUF_SIZE)

    def close(self) -> None:
        try:
            self._device.close()
        except Exception:
            pass


class HidrawDevice:
    """Linux raw hidraw ioctl backend (used when hidapi is unavailable)."""

    backend = "hidraw"

    def __init__(self, path: str) -> None:
        self.path = path
        flags = os.O_RDWR | getattr(os, "O_CLOEXEC", 0)
        self._fd = os.open(path, flags)

    def send_feature(self, payload: bytes) -> None:
        buf = ctypes.create_string_buffer(bytes([0x00]) + payload, BUF_SIZE)
        fcntl.ioctl(self._fd, HIDIOCSFEATURE, buf)

    def get_feature(self) -> bytes:
        buf = ctypes.create_string_buffer(bytes(BUF_SIZE), BUF_SIZE)  # first byte = report ID 0
        fcntl.ioctl(self._fd, HIDIOCGFEATURE, buf)
        return buf.raw

    def close(self) -> None:
        try:
            os.close(self._fd)
        except OSError:
            pass


# --------------------------------------------------------------------------- #
# Device discovery / opening
# --------------------------------------------------------------------------- #
def load_hidapi():
    """Optional dependency: return the `hid` module or None (never raises)."""
    try:
        import hid  # type: ignore
        return hid
    except Exception:
        return None


def scan_hidraw():
    """Yield (node, uevent_path, HID_ID, HID_NAME) for every /sys/class/hidraw node."""
    rows = []
    for dev in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        node = os.path.join("/dev", os.path.basename(dev))
        hid_id = ""
        hid_name = ""
        source = ""
        for candidate in (os.path.join(dev, "device", "uevent"), os.path.join(dev, "uevent")):
            try:
                with open(candidate, "r", encoding="utf-8", errors="replace") as handle:
                    text = handle.read()
            except OSError:
                continue
            if not source:
                source = candidate
            for line in text.splitlines():
                if line.startswith("HID_ID=") and not hid_id:
                    hid_id = line.partition("=")[2].strip()
                    source = candidate
                elif line.startswith("HID_NAME=") and not hid_name:
                    hid_name = line.partition("=")[2].strip()
        rows.append((node, source, hid_id, hid_name))
    return rows


def find_hidraw_nodes():
    """(node, uevent_path) for hidraw nodes whose HID_ID matches VID:PID."""
    return [(node, source) for node, source, hid_id, _ in scan_hidraw()
            if TARGET_HID_ID in hid_id.upper()]


def cmd_list(hid_module) -> int:
    print("== hidapi 设备 / hidapi devices ==")
    if hid_module is None:
        print("  hidapi 未安装 / not installed - 仅显示 hidraw 节点 / showing hidraw nodes only")
    else:
        try:
            entries = list(hid_module.enumerate() or [])
        except Exception as exc:  # never crash while debugging
            entries = []
            print("  enumerate() 调用失败 / call failed: %s" % exc)
        if not entries:
            print("  (无设备 / no devices)")
        for entry in entries:
            print("  - " + format_entry(entry))

    print("== hidraw 节点 / hidraw nodes (/sys/class/hidraw) ==")
    rows = scan_hidraw()
    if not rows:
        print("  (无节点 / no nodes)")
    for node, source, hid_id, hid_name in rows:
        match = "  <== MATCH VID:PID 1209:0001" if TARGET_HID_ID in hid_id.upper() else ""
        print("  - %s  HID_ID=%s  HID_NAME=%s%s"
              % (node, hid_id or "?", hid_name or "?", match))
        print("      sysfs: %s" % source)
    return 0


def _open_via_hidapi(hid_module):
    # macOS: allow a non-exclusive open if the binding exposes it (no-op elsewhere).
    setter = getattr(hid_module, "hid_darwin_set_open_exclusive", None)
    if callable(setter):
        try:
            setter(0)
        except Exception:
            pass

    try:
        entries = list(hid_module.enumerate(VENDOR_ID, PRODUCT_ID) or [])
    except Exception as exc:
        print(_err("hidapi enumerate(0x%04X, 0x%04X) 失败 / failed: %s"
                   % (VENDOR_ID, PRODUCT_ID, exc)))
        print(HINT_NOT_FOUND)
        return None, 2

    chosen = None
    for entry in entries:
        usage_page = entry.get("usage_page")
        usage = entry.get("usage")
        if usage_page == VENDOR_USAGE_PAGE and usage in (None, VENDOR_USAGE):
            chosen = entry
            break

    if chosen is None:
        print(_err("未找到厂商 Feature 集合 / vendor collection not found "
                   "(usage_page=0x%04X, usage=0x%02X)." % (VENDOR_USAGE_PAGE, VENDOR_USAGE)))
        if entries:
            print("已枚举目录 / enumerated entries:")
            for entry in entries:
                print("  - " + format_entry(entry))
        else:
            print("未枚举到 VID 0x%04X / PID 0x%04X 设备 / no device with that VID/PID enumerated."
                  % (VENDOR_ID, PRODUCT_ID))
        print(HINT_NOT_FOUND)
        return None, 2

    path = chosen.get("path")
    try:
        device = HidapiDevice(hid_module, path)
    except Exception as exc:
        print(_err("打开设备失败 / failed to open device: %s" % exc))
        if _looks_like_permission_error(exc):
            print(HINT_PERMISSION)
        return None, 2

    shown = path.decode("utf-8", "replace") if isinstance(path, (bytes, bytearray)) else path
    print(_ok("通过 hidapi 打开设备 / opened via hidapi: %s" % shown))
    return device, None


def _open_via_hidraw():
    nodes = find_hidraw_nodes()
    if not nodes:
        print(_err("未找到 footswitch 设备 / device not found (VID 0x%04X / PID 0x%04X)."
                   % (VENDOR_ID, PRODUCT_ID)))
        print(HINT_NOT_FOUND)
        return None, 2

    node, source = nodes[0]
    try:
        device = HidrawDevice(node)
    except OSError as exc:
        print(_err("打开 %s 失败 / failed to open %s: %s" % (node, node, exc)))
        if _looks_like_permission_error(exc):
            print(HINT_PERMISSION)
        return None, 2

    print(_ok("通过 hidraw 后备通道打开设备 / opened via hidraw fallback: %s (sysfs: %s)"
              % (node, source)))
    return device, None


def open_device(hid_module):
    """Return (device, None) on success or (None, exit_code) after diagnostics."""
    if hid_module is not None:
        return _open_via_hidapi(hid_module)
    return _open_via_hidraw()


# --------------------------------------------------------------------------- #
# Protocol transactions
# --------------------------------------------------------------------------- #
def _current_buffer_text(device) -> str:
    """Best-effort read of the feature buffer *before* sending a new command."""
    try:
        return decode_text(device.get_feature())
    except Exception:
        return ""


def read_response(device, retries: int = DEFAULT_RETRIES, delay: float = DEFAULT_DELAY,
                  placeholder: str = ""):
    """Poll get_feature_report until a non-empty/non-placeholder response arrives."""
    last_raw = b""
    last_text = ""
    for attempt in range(max(1, retries)):
        if attempt:
            time.sleep(max(0.0, delay))
        last_raw = normalize_response(device.get_feature())
        last_text = decode_text(last_raw)
        if last_text and last_text != placeholder:
            return last_raw, last_text
    return last_raw, last_text


def send_command(device, command: str, retries: int, delay: float, placeholder: str = ""):
    payload = command.encode("ascii", "replace")[:REPORT_SIZE].ljust(REPORT_SIZE, b"\x00")
    device.send_feature(payload)
    return read_response(device, retries=retries, delay=delay, placeholder=placeholder)


def command_simple(device, command: str, retries: int, delay: float):
    placeholder = _current_buffer_text(device)
    raw, text = send_command(device, command, retries, delay, placeholder)
    show(raw, text)
    return classify(text), raw, text


def command_get(device, retries: int, delay: float) -> int:
    code, raw, text = command_simple(device, "GET", retries, delay)
    match = MAP_RE.match(text)
    if match:
        for index, value in enumerate(match.groups(), 1):
            print("  踏板 %d / pedal %d -> %s" % (index, index, value))
    return code


def command_save(device, retries: int, delay: float) -> int:
    placeholder = _current_buffer_text(device)
    raw, text = send_command(device, "SAVE", retries, delay, placeholder)
    show(raw, text)
    code = classify(text)
    if code != 0:
        return code
    if "saved" in text.lower():
        return 0

    # "OK saving" received: the device writes the config later; poll for "OK saved".
    print(_warn("已受理, 等待写入完成 / accepted, waiting for flash write to finish ..."))
    for attempt in range(max(1, retries)):
        time.sleep(max(0.0, delay))
        raw = normalize_response(device.get_feature())
        text = decode_text(raw)
        if text.startswith("ERR"):
            show(raw, text)
            return 1
        if "saved" in text.lower():
            show(raw, text)
            return 0
    print(_warn("未收到 `OK saved` 确认 (设备可能仍在写入) / no `OK saved` confirmation received"))
    return 1


def command_probe(device, retries: int, delay: float) -> int:
    placeholder = _current_buffer_text(device)
    raw, text = send_command(device, "INFO", min(retries, 4), delay, placeholder)
    if text:
        show(raw, text)
        code = classify(text)
        if code == 0:
            print(_ok("设备在线 / device is reachable"))
        return code
    print(_err("设备已打开但 INFO 无响应 / device opened but did not answer the INFO command"))
    return 2


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def parse_set_params(parser: argparse.ArgumentParser, params):
    if len(params) != 2:
        parser.error("set 需要 2 个参数: <踏板 1-3> <值 0xNN> / set takes: <pedal 1-3> <0xNN>")
    try:
        pedal = int(params[0], 0)
    except ValueError:
        parser.error("踏板编号无效 / invalid pedal index: %r" % (params[0],))
    try:
        value = int(params[1], 0)
    except ValueError:
        parser.error("键码值无效 / invalid value: %r" % (params[1],))
    if pedal not in (1, 2, 3):
        parser.error("踏板编号必须是 1/2/3 / pedal index must be 1, 2 or 3")
    if not 0 <= value <= 0xFF:
        parser.error("键码值必须在 0x00-0xFF / value must be within 0x00-0xFF")
    return pedal, value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="pedalctl.py",
        description="footswitch 键盘 HID Feature 命令通道工具 / "
                    "host CLI for the footswitch keyboard HID Feature-report channel "
                    "(VID 0x1209 / PID 0x0001)",
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--probe", action="store_true",
                        help="打开设备并发送 INFO 检查在线 / open the device and send INFO")
    parser.add_argument("--list", dest="list_devices", action="store_true",
                        help="列出 HID/hidraw 候选 (调试用) / list HID/hidraw candidates")
    parser.add_argument("--retries", type=int, default=DEFAULT_RETRIES, metavar="N",
                        help="轮询次数 / poll retries (default: %(default)s)")
    parser.add_argument("--delay", type=float, default=DEFAULT_DELAY, metavar="SEC",
                        help="轮询间隔秒 / poll interval in seconds (default: %(default)s)")
    parser.add_argument("command", nargs="?", choices=("set", "get", "save", "reset", "info"),
                        metavar="{set,get,save,reset,info}",
                        help="要执行的命令 / command to run")
    parser.add_argument("params", nargs="*", metavar="ARGS",
                        help="set 的参数 / set parameters: <1-3> <0xNN>")
    return parser


def main(argv=None) -> int:
    _reconfigure_streams()
    parser = build_parser()
    args = parser.parse_args(argv)
    args.retries = max(1, args.retries)

    hid_module = load_hidapi()

    if args.list_devices:
        return cmd_list(hid_module)

    if args.command is None and not args.probe:
        parser.print_help()
        return 2

    pedal = value = None
    if args.command == "set":
        pedal, value = parse_set_params(parser, args.params)
    elif args.params:
        parser.error("命令 %s 不接受参数 / command %s takes no arguments"
                     % (args.command or "--probe", args.command or "--probe"))

    device, code = open_device(hid_module)
    if device is None:
        return code

    try:
        if args.probe:
            return command_probe(device, args.retries, args.delay)
        if args.command == "set":
            command_text = "SET %d 0x%02X" % (pedal, value)
            code, _raw, _text = command_simple(device, command_text, args.retries, args.delay)
            return code
        if args.command == "get":
            return command_get(device, args.retries, args.delay)
        if args.command == "save":
            return command_save(device, args.retries, args.delay)
        if args.command == "reset":
            code, _raw, _text = command_simple(device, "RESET", args.retries, args.delay)
            return code
        if args.command == "info":
            code, _raw, _text = command_simple(device, "INFO", args.retries, args.delay)
            return code
        parser.print_help()
        return 2
    except OSError as exc:
        print(_err("HID 传输错误 / HID transport error: %s" % exc))
        if _looks_like_permission_error(exc):
            print(HINT_PERMISSION)
        return 2
    except Exception as exc:  # hidapi raises plain Exception/IOError variants
        print(_err("通信失败 / communication failure: %s: %s" % (type(exc).__name__, exc)))
        return 2
    finally:
        device.close()


if __name__ == "__main__":
    sys.exit(main())
