#!/usr/bin/env python3
"""Validate the footswitch USB FS boot-keyboard descriptors.

Parses the uint8_t C arrays out of ROOT/app/usb_desc.c and the key map out
of ROOT/src/config.h (ROOT is the parent of this script's directory), then checks
them against the boot-keyboard requirements:

  device_desc : 18 bytes, bLength=18, bDescriptorType=1, bcdUSB=0x0110,
                bDeviceClass=0, bMaxPacketSize0=64, idVendor=0x1209,
                idProduct=0x0001, bNumConfigurations=1,
                iManufacturer=iProduct=iSerialNumber=0
  config_desc : bLength=9, bDescriptorType=2, wTotalLength==blob length,
                bNumInterfaces=1, bConfigurationValue=1, bmAttributes=0x80,
                bMaxPower<=100, then exactly one Interface (class 3,
                subclass 1, protocol 1, 1 endpoint), one HID descriptor
                (bcdHID=0x0111, 1 class descriptor of type 0x22) and one
                Endpoint (0x81, interrupt, wMaxPacketSize=8); the walk must
                end exactly at wTotalLength.
  report_desc : HID 1.11 appendix B.1 boot keyboard; top-level
                Usage Page(0x01)/Usage(0x06)/Collection(0x01), at least one
                Input item, 64 input bits (byte aligned), last item
                End Collection, collections balanced; length must match the
                HID descriptor's wDescriptorLength.
  vendor_feature : report descriptor declares a 48-byte vendor-defined
                Feature report: an Application collection (0xA1 0x01) with
                Usage Page 0xFF00 / Usage 0x01 containing a Feature main
                item (0xB1) with Report Size 8, Report Count 48 (384 bits)
                and the Data flag (bit0) set.
  keymap      : config.h defines PB9/PB8/PB7 and HID codes 0x1E/0x1F/0x20.

Stdlib only. Runnable from any cwd. Exit 0 iff every check passes.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
USB_DESC_C = ROOT / "app" / "usb_desc.c"
CONFIG_H = ROOT / "src" / "config.h"

ARRAY_RE = re.compile(
    r"(?:const\s+)?uint8_t\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}",
    re.S,
)

N_CHECKS = 5  # device_desc, config_desc, report_desc, vendor_feature, keymap


def strip_comments(text):
    """Remove C block and line comments."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


class CArray:
    """A parsed `uint8_t name[..] = { .. };` initializer.

    values[i] is the int value of token i, or None when the token is not a
    plain hex/dec literal (e.g. a macro); raws[i] keeps the original text.
    """

    __slots__ = ("name", "raws", "values")

    def __init__(self, name, body):
        self.name = name
        self.raws = []
        self.values = []
        for tok in body.split(","):
            tok = tok.strip()
            if not tok:
                continue
            self.raws.append(tok)
            self.values.append(self._parse(tok))

    @staticmethod
    def _parse(tok):
        try:
            return int(tok, 0)
        except ValueError:
            pass
        if re.fullmatch(r"\d+", tok):  # decimal with leading zeros
            try:
                return int(tok, 10)
            except ValueError:
                pass
        return None

    def __len__(self):
        return len(self.values)

    def at(self, i):
        if 0 <= i < len(self.values):
            return self.values[i]
        return None

    def raw(self, i):
        if 0 <= i < len(self.raws):
            return self.raws[i]
        return "<?>"

    def le16(self, i):
        lo, hi = self.at(i), self.at(i + 1)
        if lo is None or hi is None:
            return None
        return lo | (hi << 8)


def parse_arrays(text):
    arrays = {}
    for m in ARRAY_RE.finditer(strip_comments(text)):
        arr = CArray(m.group(1), m.group(2))
        arrays[arr.name] = arr
    return arrays


def find_array(arrays, *needles):
    """First array whose lowercase name contains all needles."""
    for name in arrays:
        low = name.lower()
        if all(n in low for n in needles):
            return arrays[name]
    return None


def num(arr, i, fname, problems, fmt="d"):
    """Fetch numeric field i; record a problem when not a plain number."""
    v = arr.at(i)
    if v is None:
        problems.append("%s=%r is not a plain number" % (fname, arr.raw(i)))
        return None
    return v


def check_u8(arr, i, fname, want, problems):
    v = num(arr, i, fname, problems)
    if v is not None and v != want:
        problems.append("%s=0x%02X want 0x%02X" % (fname, v, want))


def check_le16(arr, i, fname, want, problems):
    lo, hi = arr.at(i), arr.at(i + 1)
    if lo is None or hi is None:
        problems.append(
            "%s=<%s,%s> is not a plain number" % (fname, arr.raw(i), arr.raw(i + 1))
        )
        return
    v = lo | (hi << 8)
    if v != want:
        problems.append("%s=0x%04X want 0x%04X" % (fname, v, want))


# ---------------------------------------------------------------- device_desc

def check_device_desc(arrays):
    arr = find_array(arrays, "devicedescriptor")
    if arr is None:
        return False, "no uint8_t array named *DeviceDescriptor* found", {}
    if len(arr) != 18:
        return False, "%s has %d bytes, want 18" % (arr.name, len(arr)), {}
    problems = []
    check_u8(arr, 0, "bLength", 18, problems)
    check_u8(arr, 1, "bDescriptorType", 1, problems)
    check_le16(arr, 2, "bcdUSB", 0x0110, problems)
    check_u8(arr, 4, "bDeviceClass", 0, problems)
    check_u8(arr, 7, "bMaxPacketSize0", 64, problems)
    check_le16(arr, 8, "idVendor", 0x1209, problems)
    check_le16(arr, 10, "idProduct", 0x0001, problems)
    check_u8(arr, 17, "bNumConfigurations", 1, problems)
    for i, fname in ((14, "iManufacturer"), (15, "iProduct"), (16, "iSerialNumber")):
        check_u8(arr, i, fname, 0, problems)
    if problems:
        return False, "%s: %s" % (arr.name, "; ".join(problems)), {}
    return True, "%s[18]: USB FS device descriptor, 1209:0001" % arr.name, {}


# ---------------------------------------------------------------- config_desc

def check_config_desc(arrays):
    arr = find_array(arrays, "config", "descriptor")
    extra = {"hid_report_length": None}
    if arr is None:
        return False, "no uint8_t array named *Config*Descriptor* found", extra
    name = arr.name
    if len(arr) < 9:
        return False, "%s has %d bytes, need at least 9" % (name, len(arr)), extra

    problems = []
    check_u8(arr, 0, "bLength", 9, problems)
    check_u8(arr, 1, "bDescriptorType", 2, problems)
    wtot = arr.le16(2)
    if wtot is None:
        problems.append(
            "wTotalLength=<%s,%s> is not a plain number"
            % (arr.raw(2), arr.raw(3))
        )
    elif wtot != len(arr):
        problems.append("wTotalLength=%d want %d (total blob length)" % (wtot, len(arr)))
    check_u8(arr, 4, "bNumInterfaces", 1, problems)
    check_u8(arr, 5, "bConfigurationValue", 1, problems)
    check_u8(arr, 7, "bmAttributes", 0x80, problems)
    mp = num(arr, 8, "bMaxPower", problems)
    if mp is not None and mp > 100:
        problems.append("bMaxPower=%d want <=100" % mp)

    # Walk sub-descriptors by bLength.
    interfaces, hids, endpoints = [], [], []
    off = 9
    walk_err = None
    while off < len(arr):
        blen, btype = arr.at(off), arr.at(off + 1)
        if blen is None or btype is None:
            walk_err = "non-numeric bLength/bDescriptorType at offset %d" % off
            break
        if blen < 2:
            walk_err = "invalid bLength=%d at offset %d" % (blen, off)
            break
        if off + blen > len(arr):
            walk_err = "descriptor at offset %d (bLength=%d) overruns %d-byte blob" % (
                off, blen, len(arr))
            break
        sub = [arr.at(off + i) for i in range(blen)]
        if btype == 4:
            interfaces.append(sub)
        elif btype == 0x21:
            hids.append(sub)
        elif btype == 5:
            endpoints.append(sub)
        else:
            walk_err = "unexpected descriptor type 0x%02X at offset %d" % (btype, off)
            break
        off += blen
    if walk_err:
        problems.append("walk: " + walk_err)
    elif wtot is not None and off != wtot:
        problems.append("walk ended at offset %d, wTotalLength=%d" % (off, wtot))

    if len(interfaces) != 1:
        problems.append("found %d interface descriptors, want exactly 1" % len(interfaces))
    else:
        itf = interfaces[0]
        if len(itf) < 9:
            problems.append("interface bLength=%d want 9" % itf[0])
        else:
            if itf[4] != 1:
                problems.append("interface bNumEndpoints=%d want 1" % itf[4])
            if itf[5] != 3:
                problems.append("interface class=%d want 3 (HID)" % itf[5])
            if itf[6] != 1:
                problems.append("interface subclass=%d want 1 (boot)" % itf[6])
            if itf[7] != 1:
                problems.append("interface protocol=%d want 1 (keyboard)" % itf[7])

    if len(hids) != 1:
        problems.append("found %d HID descriptors, want exactly 1" % len(hids))
    else:
        h = hids[0]
        if len(h) < 9:
            problems.append("HID bLength=%d want 9" % h[0])
        else:
            bcdhid = None if h[2] is None or h[3] is None else h[2] | (h[3] << 8)
            if bcdhid is None:
                problems.append("bcdHID is not a plain number")
            elif bcdhid != 0x0111:
                problems.append("bcdHID=0x%04X want 0x0111" % bcdhid)
            if h[5] != 1:
                problems.append("HID bNumDescriptors=%r want 1" % h[5])
            if h[6] != 0x22:
                problems.append("HID class descriptor type=%r want 0x22 (report)" % h[6])
            if h[7] is None or h[8] is None:
                problems.append("HID wDescriptorLength is not a plain number")
            else:
                extra["hid_report_length"] = h[7] | (h[8] << 8)

    if len(endpoints) != 1:
        problems.append("found %d endpoint descriptors, want exactly 1" % len(endpoints))
    else:
        ep = endpoints[0]
        if len(ep) < 7:
            problems.append("endpoint bLength=%d want 7" % ep[0])
        else:
            if ep[2] != 0x81:
                problems.append("endpoint address=%r want 0x81 (IN 1)" % ep[2])
            if ep[3] != 0x03:
                problems.append("endpoint attributes=%r want 0x03 (interrupt)" % ep[3])
            mps = None if ep[4] is None or ep[5] is None else ep[4] | (ep[5] << 8)
            if mps is None:
                problems.append("endpoint wMaxPacketSize is not a plain number")
            elif mps != 8:
                problems.append("endpoint wMaxPacketSize=%d want 8" % mps)

    if problems:
        return False, "%s[%d]: %s" % (name, len(arr), "; ".join(problems)), extra
    detail = ("%s[%d]: config + 1 HID boot-keyboard interface + HID(0x0111) "
              "+ EP 0x81 interrupt/8" % (name, len(arr)))
    return True, detail, extra


# ---------------------------------------------------------------- report_desc

BTYPE_MAIN, BTYPE_GLOBAL, BTYPE_LOCAL = 0, 1, 2
TAG_USAGE_PAGE, TAG_REPORT_SIZE, TAG_REPORT_COUNT = 0x0, 0x7, 0x9
TAG_USAGE = 0x0
TAG_INPUT, TAG_COLLECTION, TAG_END_COLLECTION = 0x8, 0xA, 0xC


def parse_hid_items(data):
    """Parse HID short items. Returns list of (offset, bTag, bType, value, prefix).

    Raises ValueError when an item's data does not fit in the descriptor.
    """
    items = []
    off = 0
    while off < len(data):
        prefix = data[off]
        bsize = prefix & 0x03
        btype = (prefix >> 2) & 0x03
        btag = (prefix >> 4) & 0x0F
        nbytes = 4 if bsize == 3 else bsize
        if off + 1 + nbytes > len(data):
            raise ValueError(
                "item at offset %d (prefix 0x%02X) needs %d data bytes, only %d left"
                % (off, prefix, nbytes, len(data) - off - 1))
        value = 0
        for i in range(nbytes):
            value |= data[off + 1 + i] << (8 * i)
        items.append((off, btag, btype, value, prefix))
        off += 1 + nbytes
    return items


def check_report_desc(arrays, hid_report_length):
    arr = find_array(arrays, "report", "descriptor")
    if arr is None:
        return False, "no uint8_t array named *Report*Descriptor* found", {}
    name = arr.name
    problems = []
    if None in arr.values:
        bad = [r for r, v in zip(arr.raws, arr.values) if v is None]
        return False, "%s: non-numeric tokens %s" % (name, bad), {}
    if hid_report_length is not None and len(arr) != hid_report_length:
        problems.append(
            "%s is %d bytes, HID descriptor wDescriptorLength=%d"
            % (name, len(arr), hid_report_length))
    data = arr.values
    try:
        items = parse_hid_items(data)
    except ValueError as exc:
        return False, "%s: item parse error: %s" % (name, exc), {}
    if not items:
        return False, "%s: empty report descriptor" % name, {}

    depth = 0
    saw_top_usage_page = saw_top_usage = saw_top_collection = False
    saw_input = False
    input_bits = 0
    report_size = report_count = 0
    for off, btag, btype, value, prefix in items:
        if depth == 0:
            if btype == BTYPE_GLOBAL and btag == TAG_USAGE_PAGE and value == 0x01:
                saw_top_usage_page = True
            if btype == BTYPE_LOCAL and btag == TAG_USAGE and value == 0x06:
                saw_top_usage = True
            if btype == BTYPE_MAIN and btag == TAG_COLLECTION and value == 0x01:
                saw_top_collection = True
        if btype == BTYPE_GLOBAL:
            if btag == TAG_REPORT_SIZE:
                report_size = value
            elif btag == TAG_REPORT_COUNT:
                report_count = value
        elif btype == BTYPE_MAIN:
            if btag == TAG_INPUT:
                saw_input = True
                input_bits += report_size * report_count
            elif btag == TAG_COLLECTION:
                depth += 1
            elif btag == TAG_END_COLLECTION:
                depth -= 1
                if depth < 0:
                    problems.append("End Collection at offset %d with none open" % off)
                    depth = 0
    if not saw_top_usage_page:
        problems.append("no top-level Usage Page (0x01 Generic Desktop)")
    if not saw_top_usage:
        problems.append("no top-level Usage (0x06 Keyboard)")
    if not saw_top_collection:
        problems.append("no top-level Collection (0x01 Application)")
    if not saw_input:
        problems.append("no Input item at all")
    if input_bits != 64:
        problems.append("total input bits=%d want 64" % input_bits)
    if input_bits % 8 != 0:
        problems.append("total input bits %d not byte aligned" % input_bits)
    if items[-1][4] != 0xC0:
        problems.append(
            "last item prefix=0x%02X want 0xC0 (End Collection)" % items[-1][4])
    if depth != 0:
        problems.append("collection nesting ends at depth %d, want 0" % depth)

    if problems:
        return False, "%s[%d]: %s" % (name, len(arr), "; ".join(problems)), {}
    return True, "%s[%d]: boot keyboard report, 64 input bits, %d items" % (
        name, len(arr), len(items)), {}


# ------------------------------------------------------------ vendor_feature

TAG_FEATURE = 0xB
VENDOR_USAGE_PAGE = 0xFF00
VENDOR_USAGE = 0x01
VENDOR_FEATURE_FLAGS = 0x02


def check_vendor_feature(arrays):
    arr = find_array(arrays, "report", "descriptor")
    if arr is None:
        return False, "no uint8_t array named *Report*Descriptor* found", {}
    name = arr.name
    if None in arr.values:
        bad = [r for r, v in zip(arr.raws, arr.values) if v is None]
        return False, "%s: non-numeric tokens %s" % (name, bad), {}
    try:
        items = parse_hid_items(arr.values)
    except ValueError as exc:
        return False, "%s: item parse error: %s" % (name, exc), {}

    problems = []
    usage_page = usage = None
    report_size = report_count = 0
    depth = 0
    vendor_app_depth = None
    saw_vendor_app = False
    feature = None
    for off, btag, btype, value, prefix in items:
        if btype == BTYPE_GLOBAL:
            if btag == TAG_USAGE_PAGE:
                usage_page = value
            elif btag == TAG_REPORT_SIZE:
                report_size = value
            elif btag == TAG_REPORT_COUNT:
                report_count = value
        elif btype == BTYPE_LOCAL:
            if btag == TAG_USAGE:
                usage = value
        elif btype == BTYPE_MAIN:
            if btag == TAG_COLLECTION:
                depth += 1
                if (value == 0x01 and usage_page == VENDOR_USAGE_PAGE
                        and usage == VENDOR_USAGE):
                    saw_vendor_app = True
                    if vendor_app_depth is None:
                        vendor_app_depth = depth
            elif btag == TAG_END_COLLECTION:
                if vendor_app_depth is not None and depth == vendor_app_depth:
                    vendor_app_depth = None
                depth -= 1
                if depth < 0:
                    depth = 0
            elif (btag == TAG_FEATURE and vendor_app_depth is not None
                    and depth >= vendor_app_depth and feature is None):
                feature = (off, report_size, report_count, value)

    if not saw_vendor_app:
        problems.append(
            "no Application collection (0xA1 0x01) with Usage Page 0xFF00 / Usage 0x01")
    if feature is None:
        problems.append(
            "no Feature item (0xB1) inside the 0xFF00/0x01 Application collection")
    else:
        off, fsize, fcount, flags = feature
        if fsize != 8:
            problems.append("Feature Report Size=%d want 8 at offset %d" % (fsize, off))
        if fcount != 48:
            problems.append("Feature Report Count=%d want 48 at offset %d" % (fcount, off))
        if not (flags & 0x01 or flags == VENDOR_FEATURE_FLAGS):
            problems.append(
                "Feature flags=0x%02X want Data (bit0 set, 0x02 accepted) at offset %d"
                % (flags, off))

    if problems:
        return False, "%s[%d]: %s" % (name, len(arr), "; ".join(problems)), {}
    return True, ("%s[%d]: vendor Feature report, Usage Page 0xFF00 / Usage 0x01, "
                  "8 bits x 48 = 48 bytes" % (name, len(arr))), {}


# --------------------------------------------------------------------- keymap

def has_pin(text, pin):
    """Accept any sane C syntax for 'port B, pin N': PB9, GPIO_PIN_9, {GPIOB, 9}..."""
    pats = [
        r"(?i)\bPB\s*0*%d\b" % pin,
        r"(?i)\bGPIO_PIN_0*%d\b" % pin,
        r"(?i)\bPIN_0*%d\b" % pin,
        r"(?i)\bGPIOB\b[^\n;,]*\b0*%d\b" % pin,
    ]
    return any(re.search(p, text) for p in pats)


def check_keymap():
    if not CONFIG_H.exists():
        return False, "file not found: %s" % CONFIG_H, {}
    text = strip_comments(CONFIG_H.read_text())
    problems = []
    for pin in (9, 8, 7):
        if not has_pin(text, pin):
            problems.append("no PB%d (GPIO port B pin %d) switch definition found" % (pin, pin))
    for code, ch in ((0x1E, "'1'"), (0x1F, "'2'"), (0x20, "'3'")):
        if not re.search(r"(?i)\b0x%02X\b" % code, text):
            problems.append("HID keycode 0x%02X (%s) not found" % (code, ch))
    if problems:
        return False, "; ".join(problems), {}
    return True, "config.h maps SW1/SW2/SW3 to PB9/PB8/PB7 with HID codes 0x1E/0x1F/0x20", {}


# ----------------------------------------------------------------------- main

def main():
    if not USB_DESC_C.exists():
        print("FAIL device_desc: file not found: %s" % USB_DESC_C)
        print("RESULT: FAIL (0/%d checks)" % N_CHECKS)
        return 1

    arrays = parse_arrays(USB_DESC_C.read_text())

    passed = 0

    def record(name, ok, detail):
        nonlocal passed
        print("%s %s: %s" % ("PASS" if ok else "FAIL", name, detail))
        if ok:
            passed += 1

    ok, detail, _ = check_device_desc(arrays)
    record("device_desc", ok, detail)

    ok, detail, extra = check_config_desc(arrays)
    record("config_desc", ok, detail)

    ok, detail, _ = check_report_desc(arrays, extra.get("hid_report_length"))
    record("report_desc", ok, detail)

    ok, detail, _ = check_vendor_feature(arrays)
    record("vendor_feature", ok, detail)

    ok, detail, _ = check_keymap()
    record("keymap", ok, detail)

    good = passed == N_CHECKS
    print("RESULT: %s (%d/%d checks)" % ("PASS" if good else "FAIL", passed, N_CHECKS))
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
