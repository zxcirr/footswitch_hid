#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pedalctl.py - host-side CLI for the footswitch keyboard HID Feature-report channel.

Device / 设备
    VID 0x1209, PID 0x0001.  Two USB HID interfaces (QMK/VIA-style layout):
      * interface 0 = boot keyboard: own 63-byte report descriptor with a
        single top-level keyboard collection, EP1 IN (8 bytes, 1 ms);
      * interface 1 = vendor HID: own 19-byte report descriptor with a single
        top-level usage page 0xFF00 / usage 0x01 collection carrying the
        48-byte Feature report, EP2 IN (8 bytes, 1 ms, never transmits).
    Each interface is its own HID device object; the Feature channel lives on
    interface 1.  There are no report IDs anywhere (Windows HidUsb rejects a
    descriptor with more than one top-level collection and no report ID).

Protocol / 协议
    48-byte ASCII commands -> 48-byte ASCII responses (NUL padded):
        SET <1-3> <0xNN>   ->  "OK SET n=0xNN"
        GET                ->  "OK MAP 0xNN 0xNN 0xNN"
        SAVE               ->  "OK saving", later "OK saved" (poll)
        RESET              ->  "OK RESET"
        INFO               ->  "OK INFO ..."
        errors start with "ERR".
    A command is successful ONLY when the reply matches that exact expected
    text (case-insensitive).  Stale buffer text from an earlier command is
    never accepted: the tool exits 2 and asks the user to re-check the device
    with --probe.
    HID Get/Set_Report(Feature): wValue=0x0300, wIndex=interface 1, wLength=48.
    hidapi/hidraw buffers include a leading report-ID byte, so 49-byte buffers
    are used and a leading 0x00 is stripped from responses; the payload is
    NUL-padded and the command text must stay <= 47 characters (the device
    clamps the received length to 48).

Opening strategy / 打开策略
    1) If the OPTIONAL `import hid` (cython-hidapi / PyPI `hid`) succeeds:
       enumerate(0x1209, 0x0001); open ONLY the entry with usage_page == 0xFF00
       (usage == 0x01 when present) - that is interface 1.  The keyboard
       interface (usage page 0x01) is never opened.  If no 0xFF00 entry is
       present, every enumerated entry is dumped and the script exits 2.
    2) Otherwise fall back to a raw Linux hidraw ioctl (HIDIOCSFEATURE /
       HIDIOCGFEATURE with the report-ID-0 buffer).  The device exposes TWO
       /dev/hidraw* nodes with the same HID_ID (HID_ID=...00001209:00000001...),
       so the vendor interface is selected by reading each candidate's sysfs
       report descriptor (/sys/class/hidraw/hidrawN/device/report_descriptor)
       and keeping the node that contains Usage Page 0xFF00.  If that sysfs
       file is unreadable, each candidate is probed with an INFO Feature
       request (the vendor interface answers "OK INFO", the keyboard interface
       STALLs the request) and the winning probe reply is drained before the
       handle is returned.

macOS caveat / macOS 注意
    The vendor Feature report now lives on its own USB interface and top-level
    collection, so hidapi opens only the vendor HID device - it no longer has
    to seize the keyboard, and the old exclusive-open/root workaround is not
    needed for this firmware.  (On older single-interface firmware the vendor
    Feature report shared the keyboard collection and seizing it needed root:
    1) PyPI `hidapi` binding + hid_darwin_set_open_exclusive(0); 2) run as
    root; 3) use this two-interface firmware.)

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
PROBE_RETRIES = 4                # hidraw vendor-interface probe (sysfs unavailable)
PROBE_DELAY = 0.05
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
  48 字节 ASCII Feature report 命令 (wValue=0x0300, wIndex=1, wLength=48);
  hidapi/hidraw 缓冲带首字节 report ID, 因此实际使用 49 字节; 负载需 NUL 填充,
  命令文本最多 47 个字符 (设备把收到的长度截断到 48)。
  响应为 48 字节 ASCII, NUL 填充。save/get 会自动轮询 (约 8 次 x 250ms)。
  成功判定是严格的: 应答必须与请求语义匹配 (如 SET 必须回 "OK SET n=0xNN"),
  收到旧命令的残留文本不会被当作成功 (退出码 2)。

设备 / device:
  两个 USB HID 接口: 接口0 = boot 键盘 (63 字节报告描述符, 无 0xFF00);
  接口1 = 厂商 HID (19 字节报告描述符, usage page 0xFF00/usage 0x01,
  48 字节 Feature 报告)。Linux 下会出现两个 HID_ID 相同的 /dev/hidraw* 节点,
  本工具通过 sysfs report_descriptor 是否含 0xFF00 选择厂商节点 (读不到时
  用 INFO Feature 请求探测); --list 会标出哪个节点是厂商接口。

退出码 / exit codes: 0=应答与请求匹配, 1=设备返回 ERR, 2=应答缺失/不符(stale)或传输失败
  (0=reply matches the request, 1=device returned ERR, 2=no/stale reply or transport
  failure; 请用 --probe 复查设备 / re-check the device with --probe)

macOS 注意 / macOS caveat:
  厂商 (0xFF00) Feature 报告现在位于独立的 USB 接口与独立的顶层集合, macOS 上
  hidapi 只打开该厂商 HID 设备, 不再抢占键盘, 因此无需旧的独占打开/root 方案 /
  the vendor (0xFF00) Feature report now lives on its own USB interface and
  top-level collection, so macOS hidapi opens only the vendor HID device and no
  longer seizes the keyboard; the old exclusive-open/root workarounds are not
  needed. 仅旧版单接口固件需要 / only older single-interface firmware needs them:
    1) 安装 PyPI `hidapi` 绑定并先调用 hid_darwin_set_open_exclusive(0) 再打开;
    2) 以 root 运行 / run as root:            sudo python3 pedalctl.py ...
    3) 使用本双接口固件 / use this two-interface firmware.
"""

ERR_RE = re.compile(r"^ERR\b", re.IGNORECASE)
RESET_OK_RE = re.compile(r"^OK\s+RESET\b", re.IGNORECASE)
INFO_OK_RE = re.compile(r"^OK\s+INFO\b", re.IGNORECASE)
SAVED_RE = re.compile(r"^OK\s+saved\b", re.IGNORECASE)
MAP_RE = re.compile(
    r"^OK\s+MAP\s+(0x[0-9A-Fa-f]{2})\s+(0x[0-9A-Fa-f]{2})\s+(0x[0-9A-Fa-f]{2})\b",
    re.IGNORECASE,
)


def set_ok_re(pedal: int, value: int):
    """Reply that uniquely confirms `SET <pedal> 0xNN` was applied.

    The pedal index and the canonical upper-case 2-digit code are both pinned
    to what was requested, so a stale "OK SET ..." from a previous command can
    never be mistaken for success.
    """
    return re.compile(r"^OK\s+SET\s+%d=0x%02X\b" % (pedal, value), re.IGNORECASE)


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


def unexpected_reply(text: str, expected: str) -> int:
    """Exit 2 when the reply is not the exact expected text (stale/empty/other).

    Never report success for text that only *looks* like an old OK reply.
    """
    print(_err("应答不符合预期 / unexpected reply: %s"
               % (text if text else "(空 / empty)")))
    print(_warn("期望 / expected: %s" % expected))
    print(_warn("命令可能被丢弃或设备已掉线, 请用 `--probe` 重新确认设备状态 / the command may have "
                "been dropped or the device went away; re-check the device with `--probe`"))
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


def _descriptor_has_usage_page(blob: bytes, page: int) -> bool:
    """Walk HID short items and return True if a Usage Page global == page."""
    off = 0
    while off < len(blob):
        prefix = blob[off]
        bsize = prefix & 0x03
        btype = (prefix >> 2) & 0x03
        btag = (prefix >> 4) & 0x0F
        nbytes = 4 if bsize == 3 else bsize
        if off + 1 + nbytes > len(blob):
            return False
        if btype == 1 and btag == 0x0:          # Global Usage Page
            value = 0
            for i in range(nbytes):
                value |= blob[off + 1 + i] << (8 * i)
            if value == page:
                return True
        off += 1 + nbytes
    return False


def _read_report_descriptor(node: str):
    """Read the HID report descriptor sysfs exposes for a hidraw node.

    The device has two interfaces with the same VID:PID, hence two /dev/hidraw*
    nodes whose HID_ID matches; report_descriptor (next to the HID device's
    uevent, reachable through the node's `device/` symlink) is what tells the
    vendor interface (Usage Page 0xFF00) from the keyboard interface.
    Returns the bytes, or None when unavailable (permissions, old kernel).
    """
    name = os.path.basename(node)
    seen = set()
    for base in (os.path.join("/sys/class/hidraw", name, "device"),
                 os.path.join("/sys/class/hidraw", name)):
        try:
            real = os.path.realpath(base)
        except OSError:
            continue
        if real in seen:
            continue
        seen.add(real)
        try:
            with open(os.path.join(real, "report_descriptor"), "rb") as handle:
                return handle.read()
        except OSError:
            continue
    return None


def classify_hidraw_nodes(nodes):
    """Split matching hidraw nodes into (vendor, other, unknown).

    vendor  : sysfs descriptor read OK and contains Usage Page 0xFF00
    other   : sysfs descriptor read OK and does NOT contain it (keyboard)
    unknown : descriptor unavailable - the caller must probe
    """
    vendor, other, unknown = [], [], []
    for node, source in nodes:
        blob = _read_report_descriptor(node)
        if blob is None:
            unknown.append((node, source))
        elif _descriptor_has_usage_page(blob, VENDOR_USAGE_PAGE):
            vendor.append((node, source))
        else:
            other.append((node, source))
    return vendor, other, unknown


def _node_role_label(node: str) -> str:
    blob = _read_report_descriptor(node)
    if blob is None:
        return "? (report_descriptor unreadable; probe to identify)"
    if _descriptor_has_usage_page(blob, VENDOR_USAGE_PAGE):
        return "VENDOR/feature-capable (usage page 0xFF00) - use this one"
    return "keyboard (no 0xFF00) - Feature channel lives on the other node"


def _probe_vendor_node(node: str):
    """Open `node`, send INFO, and return the open HidrawDevice on OK INFO.

    Fallback for when sysfs report descriptors are unavailable: the vendor
    interface answers Feature requests, the keyboard interface STALLs them.
    The OK INFO reply is drained (one extra full feature read) before the
    handle is returned, so the strict matcher can never later mistake that
    stale text for the reply to a new command.
    """
    try:
        device = HidrawDevice(node)
    except OSError as exc:
        print(_warn("探测 %s 失败 / probe of %s failed: %s" % (node, node, exc)))
        if _looks_like_permission_error(exc):
            print(HINT_PERMISSION)
        return None
    try:
        payload = "INFO".encode("ascii")[:REPORT_SIZE].ljust(REPORT_SIZE, b"\x00")
        device.send_feature(payload)
        for attempt in range(PROBE_RETRIES):
            if attempt:
                time.sleep(PROBE_DELAY)
            raw = normalize_response(device.get_feature())
            text = decode_text(raw)
            if INFO_OK_RE.match(text):
                device.get_feature()    # drain the probe reply fully
                return device
            if ERR_RE.match(text):
                break
    except Exception:
        pass
    device.close()
    return None


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
        match = TARGET_HID_ID in hid_id.upper()
        print("  - %s  HID_ID=%s  HID_NAME=%s%s"
              % (node, hid_id or "?", hid_name or "?",
                 "  <== MATCH VID:PID 1209:0001" if match else ""))
        print("      sysfs: %s" % source)
        if match:
            print("      role : %s" % _node_role_label(node))
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
        # No fallback to "any 1209:0001 entry": with two interfaces that would
        # open the keyboard collection, whose Feature requests are STALLed.
        print(_err("未找到厂商 HID 条目 / no vendor HID entry found (usage_page=0x%04X, "
                   "usage=0x%02X) for VID 0x%04X / PID 0x%04X."
                   % (VENDOR_USAGE_PAGE, VENDOR_USAGE, VENDOR_ID, PRODUCT_ID)))
        if entries:
            print("已枚举目录 / enumerated entries:")
            for entry in entries:
                print("  - " + format_entry(entry))
        else:
            print("未枚举到 VID 0x%04X / PID 0x%04X 设备 / no device with that VID/PID enumerated."
                  % (VENDOR_ID, PRODUCT_ID))
        print(_warn("可用 `--list` 复查 HID/hidraw 候选 / use `--list` to re-check HID/hidraw candidates"))
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

    vendor, other, unknown = classify_hidraw_nodes(nodes)

    if vendor:
        if len(vendor) > 1:
            print(_warn("多个节点报告 usage page 0xFF00 / several nodes expose usage page "
                        "0xFF00; using the first"))
        node, source = vendor[0]
        try:
            device = HidrawDevice(node)
        except OSError as exc:
            print(_err("打开 %s 失败 / failed to open %s: %s" % (node, node, exc)))
            if _looks_like_permission_error(exc):
                print(HINT_PERMISSION)
            return None, 2
        print(_ok("通过 hidraw 后备通道打开厂商接口 / opened the vendor interface via hidraw "
                  "fallback: %s (sysfs report_descriptor contains 0xFF00; sysfs: %s)"
                  % (node, source)))
        return device, None

    if other and not unknown:
        print(_err("匹配 VID:PID 的 hidraw 节点都不含 usage page 0xFF00 / none of the matching "
                   "hidraw nodes exposes usage page 0xFF00 (device without a vendor interface?)"))
        print(_warn("请用 `--list` 复查节点角色 / use `--list` to inspect node roles"))
        print(HINT_NOT_FOUND)
        return None, 2

    # sysfs report descriptors unavailable: probe each candidate with INFO.
    # The vendor interface answers "OK INFO"; the keyboard interface STALLs the
    # Feature request.  Unknown nodes are probed before known keyboard nodes.
    for node, source in unknown + other:
        device = _probe_vendor_node(node)
        if device is not None:
            print(_ok("通过 hidraw 后备通道打开厂商接口 / opened the vendor interface via hidraw "
                      "fallback: %s (probed INFO; sysfs: %s)" % (node, source)))
            return device, None

    print(_err("未能定位厂商 Feature 接口 / could not locate the vendor Feature interface "
               "(sysfs report_descriptor unavailable, INFO probe answered by none of %d node(s))"
               % len(unknown + other)))
    print(_warn("请用 `--list` 复查节点与权限 / use `--list` to inspect nodes & permissions"))
    print(HINT_NOT_FOUND)
    return None, 2


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
    """Send one command and return (raw, text); success judgement is the caller's."""
    placeholder = _current_buffer_text(device)
    raw, text = send_command(device, command, retries, delay, placeholder)
    show(raw, text)
    return raw, text


def command_set(device, pedal: int, value: int, retries: int, delay: float) -> int:
    _raw, text = command_simple(device, "SET %d 0x%02X" % (pedal, value), retries, delay)
    if ERR_RE.match(text):
        return 1
    if set_ok_re(pedal, value).match(text):
        return 0
    return unexpected_reply(text, "OK SET %d=0x%02X" % (pedal, value))


def command_reset(device, retries: int, delay: float) -> int:
    _raw, text = command_simple(device, "RESET", retries, delay)
    if ERR_RE.match(text):
        return 1
    if RESET_OK_RE.match(text):
        return 0
    return unexpected_reply(text, "OK RESET")


def command_info(device, retries: int, delay: float) -> int:
    _raw, text = command_simple(device, "INFO", retries, delay)
    if ERR_RE.match(text):
        return 1
    if INFO_OK_RE.match(text):
        return 0
    return unexpected_reply(text, "OK INFO")


def command_get(device, retries: int, delay: float) -> int:
    _raw, text = command_simple(device, "GET", retries, delay)
    if ERR_RE.match(text):
        return 1
    match = MAP_RE.match(text)
    if not match:
        return unexpected_reply(text, "OK MAP 0xNN 0xNN 0xNN")
    for index, value in enumerate(match.groups(), 1):
        print("  踏板 %d / pedal %d -> %s" % (index, index, value))
    return 0


def command_save(device, retries: int, delay: float) -> int:
    placeholder = _current_buffer_text(device)
    raw, text = send_command(device, "SAVE", retries, delay, placeholder)
    show(raw, text)
    if ERR_RE.match(text):
        return 1
    if SAVED_RE.match(text):
        return 0

    # "OK saving" received: the device writes the config later; poll for "OK saved".
    print(_warn("已受理, 等待写入完成 / accepted, waiting for flash write to finish ..."))
    for attempt in range(max(1, retries)):
        time.sleep(max(0.0, delay))
        raw = normalize_response(device.get_feature())
        text = decode_text(raw)
        if ERR_RE.match(text):
            show(raw, text)
            return 1
        if SAVED_RE.match(text):
            show(raw, text)
            return 0
    show(raw, text)
    print(_err("等待 `OK saved` 超时 / timed out waiting for `OK saved`"))
    print(_warn("设备可能仍在写入或已掉线, 请用 `--probe` 重新确认 / the device may still be writing or "
                "went away; re-check the device with `--probe`"))
    return 2


def command_probe(device, retries: int, delay: float) -> int:
    _raw, text = command_simple(device, "INFO", min(retries, 4), delay)
    if ERR_RE.match(text):
        return 1
    if INFO_OK_RE.match(text):
        print(_ok("设备在线 / device is reachable"))
        return 0
    if not text:
        print(_err("设备已打开但 INFO 无响应 / device opened but did not answer the INFO command"))
    else:
        print(_err("设备已打开但 INFO 应答无效 (疑似残留文本) / device opened but the INFO reply was "
                   "not recognised (looks stale)"))
    print(_warn("请用 `--list` 检查 hidraw 节点/权限, 或稍后重试 `--probe` / inspect nodes & permissions "
                "with `--list`, then retry `--probe`"))
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
                        help="列出 HID/hidraw 候选并标出厂商接口 "
                             "(usage page 0xFF00 / INFO-capable) / list HID/hidraw candidates "
                             "and mark the vendor interface")
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
            return command_set(device, pedal, value, args.retries, args.delay)
        if args.command == "get":
            return command_get(device, args.retries, args.delay)
        if args.command == "save":
            return command_save(device, args.retries, args.delay)
        if args.command == "reset":
            return command_reset(device, args.retries, args.delay)
        if args.command == "info":
            return command_info(device, args.retries, args.delay)
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
