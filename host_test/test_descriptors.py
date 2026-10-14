#!/usr/bin/env python3
"""Validate the footswitch USB FS descriptors (option B: two HID interfaces).

Parses the uint8_t C arrays out of ROOT/app/usb_desc.c and the key map out
of ROOT/src/config.h (ROOT is the parent of this script's directory), then checks
them against the two-interface QMK/VIA-style layout:

  device_desc : 18 bytes, bLength=18, bDescriptorType=1, bcdUSB=0x0110,
                bcdDevice=0x0004 (displayed 0.04; the revision was bumped so
                Windows re-reads its cached descriptors), bDeviceClass=0,
                bMaxPacketSize0=64, idVendor=0x1209, idProduct=0x0001,
                bNumConfigurations=1, iManufacturer=1, iProduct=2,
                iSerialNumber=0
  config_desc : 59 bytes, bLength=9, bDescriptorType=2, wTotalLength==blob
                length, bNumInterfaces=2, bConfigurationValue=1,
                bmAttributes=0x80, bMaxPower<=100, then:
                  interface 0 (boot keyboard): class 3/sub 1/proto 1, 1 endpoint,
                    HID descriptor with wDescriptorLength=63,
                    endpoint 0x81 interrupt / 8 bytes / 1 ms;
                  interface 1 (vendor HID): class 3/sub 0/proto 0, 1 endpoint,
                    HID descriptor with wDescriptorLength=21,
                    endpoint 0x82 interrupt / 8 bytes / 1 ms.
                The walk must end exactly at wTotalLength.
  two_interfaces : both interfaces present, each with exactly one HID descriptor
                and one interrupt IN endpoint, and each HID descriptor's
                wDescriptorLength matching its own report descriptor array's
                length (63 keyboard / 21 vendor).  Windows parses HID per USB
                interface, so each interface must be a self-consistent HID device.
  report_desc : parses CustomHID_ReportDescriptor (63 bytes): HID 1.11 appendix
                B.1 boot keyboard; top-level Usage Page(0x01)/Usage(0x06)/
                Collection(0x01), at least one Input item, 64 input bits (byte
                aligned), last item End Collection, collections balanced, and no
                Report ID items.
  vendor_feature : parses CustomHID_VendorReportDescriptor (21 bytes): Usage
                Page 0xFF00 / Usage 0x01, exactly one Application collection,
                a Feature main item (0xB1) with Report Size 8, Report Count 48
                (384 bits) and the Data flag (bit0) set, a Usage of its OWN
                before that Feature item (the collection's Usage is consumed by
                the Collection; Windows rejects a data main item without usage:
                0xC00000B9 -> MI_01 Code 10), and no Report ID items.
  single_tlc  : BOTH report descriptors: exactly one top-level Application
                collection (0xA1 0x01 at nesting depth 0) and NO Report ID items
                (0x85) anywhere.  Windows HidUsb rejects a report descriptor
                whose interface has more than one top-level collection when none
                declares a report ID (STATUS_COULD_NOT_INTERPRET / 0xC00000B9).
  keymap      : config.h defines PB9/PB8/PB7 and HID codes 0x1E/0x1F/0x20.
  strings     : CustomHID_StringLangID == {04 03 09 04} (LANGID 0x0409);
                CustomHID_StringVendor == header {16 03} + UTF-16LE "FootSwitch"
                (bLength=0x16=22); CustomHID_StringProduct == header {14 03} +
                UTF-16LE "HID Pedal" (bLength=0x14=20); serial stays the
                empty {02 03}; each array's first byte equals its length and
                its second byte is 0x03.

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

# Role-based lookup: the vendor array name also contains "ReportDescriptor",
# so a loose substring search would be ambiguous.  Exact names only.
KEYBOARD_REPORT_NAME = "CustomHID_ReportDescriptor"
VENDOR_REPORT_NAME = "CustomHID_VendorReportDescriptor"
KEYBOARD_REPORT_LEN = 63
VENDOR_REPORT_LEN = 21
CONFIG_LEN = 59

N_CHECKS = 8  # device_desc, config_desc, two_interfaces, report_desc, vendor_feature, single_tlc, keymap, strings


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


def get_array(arrays, name):
    """Exact-name lookup (role-based; avoids ReportDescriptor substring clashes)."""
    return arrays.get(name)


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
    # bcdDevice 0.04 = BCD 0x0004, stored little-endian as bytes {04,00}
    check_le16(arr, 12, "bcdDevice", 0x0004, problems)
    check_u8(arr, 17, "bNumConfigurations", 1, problems)
    check_u8(arr, 14, "iManufacturer", 1, problems)
    check_u8(arr, 15, "iProduct", 2, problems)
    check_u8(arr, 16, "iSerialNumber", 0, problems)
    if problems:
        return False, "%s: %s" % (arr.name, "; ".join(problems)), {}
    return True, ("%s[18]: USB FS device descriptor, 1209:0001, bcdDevice=0.04, "
                  "iManufacturer=1 iProduct=2 iSerialNumber=0") % arr.name, {}


# ---------------------------------------------------------------- config_desc

def walk_config(arr):
    """Walk sub-descriptors by bLength, grouping HID + endpoints per interface.

    Returns (interfaces, end_offset, problems).  Each interface is a dict:
    {"desc": bytes, "hids": [bytes], "endpoints": [bytes]}.
    """
    interfaces = []
    current = None
    problems = []
    off = 9
    while off < len(arr):
        blen, btype = arr.at(off), arr.at(off + 1)
        if blen is None or btype is None:
            problems.append("non-numeric bLength/bDescriptorType at offset %d" % off)
            break
        if blen < 2:
            problems.append("invalid bLength=%d at offset %d" % (blen, off))
            break
        if off + blen > len(arr):
            problems.append("descriptor at offset %d (bLength=%d) overruns %d-byte blob" % (
                off, blen, len(arr)))
            break
        sub = [arr.at(off + i) for i in range(blen)]
        if btype == 4:
            current = {"desc": sub, "offset": off, "hids": [], "endpoints": []}
            interfaces.append(current)
        elif btype == 0x21:
            if current is None:
                problems.append("HID descriptor at offset %d before any interface" % off)
            else:
                current["hids"].append(sub)
        elif btype == 5:
            if current is None:
                problems.append("endpoint at offset %d before any interface" % off)
            else:
                current["endpoints"].append(sub)
        else:
            problems.append("unexpected descriptor type 0x%02X at offset %d" % (btype, off))
            break
        off += blen
    return interfaces, off, problems


def _check_hid_len(h, want, label, problems):
    """Validate one HID descriptor and return its wDescriptorLength (or None)."""
    if len(h) < 9:
        problems.append("%s HID bLength=%d want 9" % (label, h[0]))
        return None
    bcdhid = None if h[2] is None or h[3] is None else h[2] | (h[3] << 8)
    if bcdhid is None:
        problems.append("%s bcdHID is not a plain number" % label)
    elif bcdhid != 0x0111:
        problems.append("%s bcdHID=0x%04X want 0x0111" % (label, bcdhid))
    if h[5] != 1:
        problems.append("%s HID bNumDescriptors=%r want 1" % (label, h[5]))
    if h[6] != 0x22:
        problems.append("%s HID class descriptor type=%r want 0x22 (report)" % (label, h[6]))
    if h[7] is None or h[8] is None:
        problems.append("%s HID wDescriptorLength is not a plain number" % label)
        return None
    wlen = h[7] | (h[8] << 8)
    if want is not None and wlen != want:
        problems.append("%s HID wDescriptorLength=%d want %d" % (label, wlen, want))
    return wlen


def _check_ep(ep, want_addr, want_mps, want_interval, label, problems):
    if len(ep) < 7:
        problems.append("%s endpoint bLength=%d want 7" % (label, ep[0]))
        return
    if ep[2] != want_addr:
        problems.append("%s endpoint address=%r want 0x%02X" % (label, ep[2], want_addr))
    if ep[3] != 0x03:
        problems.append("%s endpoint attributes=%r want 0x03 (interrupt)" % (label, ep[3]))
    mps = None if ep[4] is None or ep[5] is None else ep[4] | (ep[5] << 8)
    if mps is None:
        problems.append("%s endpoint wMaxPacketSize is not a plain number" % label)
    elif mps != want_mps:
        problems.append("%s endpoint wMaxPacketSize=%d want %d" % (label, mps, want_mps))
    if want_interval is not None and ep[6] != want_interval:
        problems.append("%s endpoint bInterval=%r want %d" % (label, ep[6], want_interval))


def check_config_desc(arrays):
    arr = find_array(arrays, "config", "descriptor")
    extra = {"interfaces": []}
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
    if len(arr) != CONFIG_LEN:
        problems.append("%s has %d bytes, want %d" % (name, len(arr), CONFIG_LEN))
    check_u8(arr, 4, "bNumInterfaces", 2, problems)
    check_u8(arr, 5, "bConfigurationValue", 1, problems)
    check_u8(arr, 7, "bmAttributes", 0x80, problems)
    mp = num(arr, 8, "bMaxPower", problems)
    if mp is not None and mp > 100:
        problems.append("bMaxPower=%d want <=100" % mp)

    interfaces, end_off, walk_problems = walk_config(arr)
    problems.extend("walk: " + p for p in walk_problems)
    if not walk_problems and wtot is not None and end_off != wtot:
        problems.append("walk ended at offset %d, wTotalLength=%d" % (end_off, wtot))
    extra["interfaces"] = interfaces

    # interface 0 = boot keyboard; interface 1 = vendor HID
    specs = (
        (0, 0, 1, 3, 1, 1, 0x81, KEYBOARD_REPORT_LEN),
        (1, 0, 0, 3, 0, 1, 0x82, VENDOR_REPORT_LEN),
    )
    if len(interfaces) != 2:
        problems.append("found %d interface descriptors, want exactly 2" % len(interfaces))
    else:
        for want_num, want_alt, want_sub, want_cls, want_proto, want_nep, want_ep, want_hidlen in specs:
            label = "interface %d" % want_num
            itf = interfaces[want_num]
            d = itf["desc"]
            if len(d) < 9:
                problems.append("%s bLength=%d want 9" % (label, d[0]))
                continue
            if d[2] != want_num:
                problems.append("%s bInterfaceNumber=%r want %d" % (label, d[2], want_num))
            if d[3] != want_alt:
                problems.append("%s bAlternateSetting=%r want %d" % (label, d[3], want_alt))
            if d[4] != want_nep:
                problems.append("%s bNumEndpoints=%d want %d" % (label, d[4], want_nep))
            if d[5] != want_cls:
                problems.append("%s class=%d want %d (HID)" % (label, d[5], want_cls))
            if d[6] != want_sub:
                problems.append("%s subclass=%d want %d" % (label, d[6], want_sub))
            if d[7] != want_proto:
                problems.append("%s protocol=%d want %d" % (label, d[7], want_proto))
            if len(itf["hids"]) != 1:
                problems.append("%s: found %d HID descriptor(s), want exactly 1"
                                % (label, len(itf["hids"])))
            else:
                _check_hid_len(itf["hids"][0], want_hidlen, label, problems)
            if len(itf["endpoints"]) != 1:
                problems.append("%s: found %d endpoint descriptor(s), want exactly 1"
                                % (label, len(itf["endpoints"])))
            else:
                _check_ep(itf["endpoints"][0], want_ep, 8, 1, label, problems)

    if problems:
        return False, "%s[%d]: %s" % (name, len(arr), "; ".join(problems)), extra
    detail = ("%s[%d]: config + iface0 HID boot-keyboard (HID desc %d, EP 0x81 "
              "interrupt/8/1ms) + iface1 vendor HID (HID desc %d, EP 0x82 "
              "interrupt/8/1ms)" % (name, len(arr), KEYBOARD_REPORT_LEN, VENDOR_REPORT_LEN))
    return True, detail, extra


# ------------------------------------------------------------- two_interfaces

def check_two_interfaces(arrays, cfg_extra):
    """Both interfaces self-consistent: 1 HID + 1 interrupt IN EP each.

    Each HID descriptor's wDescriptorLength must match its own report
    descriptor array (interface 0 -> 63-byte keyboard, interface 1 -> 21-byte
    vendor).  Windows parses HID per USB interface, so each interface is its
    own HID device.
    """
    problems = []
    kb = get_array(arrays, KEYBOARD_REPORT_NAME)
    vd = get_array(arrays, VENDOR_REPORT_NAME)
    if kb is None:
        problems.append("no uint8_t array named %s" % KEYBOARD_REPORT_NAME)
    if vd is None:
        problems.append("no uint8_t array named %s" % VENDOR_REPORT_NAME)

    interfaces = cfg_extra.get("interfaces") or []
    if len(interfaces) != 2:
        problems.append("config has %d interface descriptor(s), want 2" % len(interfaces))
        return False, "; ".join(problems), {}

    by_num = {}
    for itf in interfaces:
        d = itf["desc"]
        if len(d) < 9 or d[2] is None:
            problems.append("interface descriptor with unusable bInterfaceNumber")
            continue
        by_num[d[2]] = itf

    specs = (
        (0, kb, KEYBOARD_REPORT_LEN, 0x81, "keyboard"),
        (1, vd, VENDOR_REPORT_LEN, 0x82, "vendor"),
    )
    for want_num, report_arr, want_len, want_ep, role in specs:
        itf = by_num.get(want_num)
        if itf is None:
            problems.append("no interface %d (%s)" % (want_num, role))
            continue
        hids = itf["hids"]
        eps = itf["endpoints"]
        if len(hids) != 1:
            problems.append("interface %d (%s): %d HID descriptor(s), want 1"
                            % (want_num, role, len(hids)))
        if len(eps) != 1:
            problems.append("interface %d (%s): %d endpoint descriptor(s), want 1"
                            % (want_num, role, len(eps)))
        if len(eps) == 1:
            ep = eps[0]
            if len(ep) < 7 or ep[2] != want_ep or ep[3] != 0x03:
                problems.append("interface %d (%s): endpoint %r attr %r, want 0x%02X interrupt IN"
                                % (want_num, role, ep[2] if len(ep) > 2 else None,
                                   ep[3] if len(ep) > 3 else None, want_ep))
        if len(hids) == 1 and report_arr is not None:
            h = hids[0]
            wlen = None if len(h) < 9 or h[7] is None or h[8] is None else h[7] | (h[8] << 8)
            if wlen is None:
                problems.append("interface %d (%s): HID wDescriptorLength is not a plain number"
                                % (want_num, role))
            else:
                if wlen != len(report_arr):
                    problems.append("interface %d (%s): HID wDescriptorLength=%d != %s length %d"
                                    % (want_num, role, wlen, report_arr.name, len(report_arr)))
                if len(report_arr) != want_len:
                    problems.append("interface %d (%s): %s is %d bytes, want %d"
                                    % (want_num, role, report_arr.name, len(report_arr), want_len))

    if problems:
        return False, "; ".join(problems), {}
    return True, ("2 interfaces, each = 1 HID descriptor + 1 interrupt IN endpoint; "
                  "wDescriptorLength matches its own report array (%d keyboard, %d vendor)"
                  % (KEYBOARD_REPORT_LEN, VENDOR_REPORT_LEN)), {}


# ---------------------------------------------------------------- report_desc

BTYPE_MAIN, BTYPE_GLOBAL, BTYPE_LOCAL = 0, 1, 2
TAG_USAGE_PAGE, TAG_REPORT_SIZE, TAG_REPORT_COUNT = 0x0, 0x7, 0x9
TAG_REPORT_ID = 0x8
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


def check_report_desc(arrays):
    arr = get_array(arrays, KEYBOARD_REPORT_NAME)
    if arr is None:
        return False, "no uint8_t array named %s" % KEYBOARD_REPORT_NAME, {}
    name = arr.name
    problems = []
    if None in arr.values:
        bad = [r for r, v in zip(arr.raws, arr.values) if v is None]
        return False, "%s: non-numeric tokens %s" % (name, bad), {}
    if len(arr) != KEYBOARD_REPORT_LEN:
        problems.append("%s is %d bytes, want %d" % (name, len(arr), KEYBOARD_REPORT_LEN))
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
    report_ids = 0
    report_size = report_count = 0
    for off, btag, btype, value, prefix in items:
        if prefix == 0x85 or (btype == BTYPE_GLOBAL and btag == TAG_REPORT_ID):
            report_ids += 1
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
    if report_ids:
        problems.append("found %d Report ID item(s) (0x85 global), want none" % report_ids)

    if problems:
        return False, "%s[%d]: %s" % (name, len(arr), "; ".join(problems)), {}
    return True, "%s[%d]: boot keyboard report, 64 input bits, no report IDs, %d items" % (
        name, len(arr), len(items)), {}


# ------------------------------------------------------------ vendor_feature

TAG_FEATURE = 0xB
VENDOR_USAGE_PAGE = 0xFF00
VENDOR_USAGE = 0x01
VENDOR_FEATURE_FLAGS = 0x02


def check_vendor_feature(arrays):
    arr = get_array(arrays, VENDOR_REPORT_NAME)
    if arr is None:
        return False, "no uint8_t array named %s" % VENDOR_REPORT_NAME, {}
    name = arr.name
    if None in arr.values:
        bad = [r for r, v in zip(arr.raws, arr.values) if v is None]
        return False, "%s: non-numeric tokens %s" % (name, bad), {}
    problems = []
    if len(arr) != VENDOR_REPORT_LEN:
        problems.append("%s is %d bytes, want %d" % (name, len(arr), VENDOR_REPORT_LEN))
    try:
        items = parse_hid_items(arr.values)
    except ValueError as exc:
        return False, "%s: item parse error: %s" % (name, exc), {}

    depth = 0
    top_app_collections = 0
    report_ids = 0
    usage_page = usage = None
    usage_pending = 0        # local-pool usages not yet consumed by a main item
    report_size = report_count = 0
    saw_vendor_usage_page = saw_vendor_usage = False
    feature = None
    for off, btag, btype, value, prefix in items:
        if prefix == 0x85 or (btype == BTYPE_GLOBAL and btag == TAG_REPORT_ID):
            report_ids += 1
        if btype == BTYPE_GLOBAL:
            if btag == TAG_USAGE_PAGE:
                usage_page = value
                if value == VENDOR_USAGE_PAGE:
                    saw_vendor_usage_page = True
            elif btag == TAG_REPORT_SIZE:
                report_size = value
            elif btag == TAG_REPORT_COUNT:
                report_count = value
        elif btype == BTYPE_LOCAL:
            if btag == TAG_USAGE:
                usage = value
                usage_pending += 1
                if usage_page == VENDOR_USAGE_PAGE and value == VENDOR_USAGE:
                    saw_vendor_usage = True
        elif btype == BTYPE_MAIN:
            if btag == TAG_COLLECTION:
                usage_pending = max(0, usage_pending - 1)   # Collection consumes one usage
                if depth == 0 and value == 0x01:
                    top_app_collections += 1
                depth += 1
            elif btag == TAG_END_COLLECTION:
                depth -= 1
                if depth < 0:
                    depth = 0
            elif btag == TAG_FEATURE and feature is None:
                feature = (off, report_size, report_count, value, usage_pending > 0)
                usage_pending = max(0, usage_pending - 1)

    if not saw_vendor_usage_page:
        problems.append("no Usage Page 0xFF00 item in the report descriptor")
    if not saw_vendor_usage:
        problems.append("no Usage 0x01 item under Usage Page 0xFF00")
    if top_app_collections != 1:
        problems.append("found %d top-level Application collection(s), want exactly 1"
                        % top_app_collections)
    if feature is None:
        problems.append("no Feature item (0xB1) in the report descriptor")
    else:
        off, fsize, fcount, flags, had_usage = feature
        if not had_usage:
            problems.append(
                "Feature item at offset %d has no Usage of its own (the "
                "collection's Usage was consumed by the Collection) - Windows "
                "rejects a data main item without usage: 0xC00000B9 -> MI_01 "
                "Code 10, the vendor interface never starts" % off)
        if fsize != 8:
            problems.append("Feature Report Size=%d want 8 at offset %d" % (fsize, off))
        if fcount != 48:
            problems.append("Feature Report Count=%d want 48 at offset %d" % (fcount, off))
        if not (flags & 0x01 or flags == VENDOR_FEATURE_FLAGS):
            problems.append(
                "Feature flags=0x%02X want Data (bit0 set, 0x02 accepted) at offset %d"
                % (flags, off))
    if report_ids:
        problems.append("found %d Report ID item(s) (0x85 global), want none" % report_ids)

    if problems:
        return False, "%s[%d]: %s" % (name, len(arr), "; ".join(problems)), {}
    return True, ("%s[%d]: vendor Feature report, Usage Page 0xFF00 / Usage 0x01 in its own "
                  "Application collection, Feature with its own Usage, 8 bits x 48 = 48 bytes, "
                  "no report IDs"
                  % (name, len(arr))), {}


# ------------------------------------------------------------------- single_tlc

def _single_tlc_one(arr):
    """Check one report descriptor: exactly 1 top-level Application collection,
    zero Report ID items."""
    name = arr.name
    if None in arr.values:
        bad = [r for r, v in zip(arr.raws, arr.values) if v is None]
        return False, "%s: non-numeric tokens %s" % (name, bad), {}
    try:
        items = parse_hid_items(arr.values)
    except ValueError as exc:
        return False, "%s: item parse error: %s" % (name, exc), {}

    problems = []
    depth = 0
    top_collections = 0
    top_app_collections = 0
    report_ids = 0
    for off, btag, btype, value, prefix in items:
        if prefix == 0x85 or (btype == BTYPE_GLOBAL and btag == TAG_REPORT_ID):
            report_ids += 1
        elif btype == BTYPE_MAIN:
            if btag == TAG_COLLECTION:
                if depth == 0:
                    top_collections += 1
                    if value == 0x01:
                        top_app_collections += 1
                depth += 1
            elif btag == TAG_END_COLLECTION:
                depth -= 1
                if depth < 0:
                    depth = 0

    if top_app_collections != 1:
        problems.append("found %d top-level Application collection(s) "
                        "(0xA1 0x01 at depth 0), want exactly 1" % top_app_collections)
    elif top_collections != 1:
        problems.append("found %d top-level collection(s), want exactly 1" % top_collections)
    if report_ids:
        problems.append("found %d Report ID item(s) (0x85 global), want none" % report_ids)

    if problems:
        return False, "%s[%d]: %s" % (name, len(arr), "; ".join(problems)), {}
    return True, ("%s[%d]: exactly 1 top-level Application collection, no Report ID "
                  "items (Windows HidUsb compatible)" % (name, len(arr))), {}


def check_single_tlc(arrays):
    """Windows HidUsb compatibility guard, applied to BOTH report descriptors.

    A report descriptor with more than one top-level collection is rejected by
    Windows when none of them declares a report ID (STATUS_COULD_NOT_INTERPRET /
    0xC00000B9).  Windows parses HID per USB interface, so each interface's
    report descriptor must independently satisfy this invariant.
    """
    problems = []
    details = []
    for label, name in (("keyboard", KEYBOARD_REPORT_NAME), ("vendor", VENDOR_REPORT_NAME)):
        arr = get_array(arrays, name)
        if arr is None:
            problems.append("no uint8_t array named %s (%s report descriptor)" % (name, label))
            continue
        ok, detail, _ = _single_tlc_one(arr)
        if ok:
            details.append(detail)
        else:
            problems.append(detail)
    if problems:
        return False, "; ".join(problems), {}
    return True, " | ".join(details), {}


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


# -------------------------------------------------------------------- strings

def utf16le_string(s):
    """Expected USB string descriptor bytes: 2-byte header + UTF-16LE text."""
    text = list(s.encode("utf-16-le"))
    return [len(text) + 2, 0x03] + text


def fmt_bytes(vals):
    return "{" + ",".join("0x%02X" % v for v in vals) + "}"


def check_strings(arrays):
    specs = (
        ("StringLangID", find_array(arrays, "string", "langid"),
         [0x04, 0x03, 0x09, 0x04], "LANGID 0x0409"),
        ("StringVendor", find_array(arrays, "string", "vendor"),
         utf16le_string("FootSwitch"), 'UTF-16LE "FootSwitch"'),
        ("StringProduct", find_array(arrays, "string", "product"),
         utf16le_string("HID Pedal"), 'UTF-16LE "HID Pedal"'),
        ("StringSerial", find_array(arrays, "string", "serial"),
         [0x02, 0x03], "empty (2-byte header)"),
    )
    problems = []
    for label_name, arr, want, label in specs:
        if arr is None:
            problems.append("no uint8_t array named *%s* found" % label_name)
            continue
        bad = [r for r, v in zip(arr.raws, arr.values) if v is None]
        if bad:
            problems.append("%s: non-numeric tokens %s" % (arr.name, bad))
            continue
        got = arr.values
        if len(got) < 2:
            problems.append("%s has %d bytes, need at least 2" % (arr.name, len(got)))
            continue
        if got[0] != len(got) or got[1] != 0x03:
            problems.append(
                "%s: bLength=%d bDescriptorType=0x%02X want bLength=%d, type 0x03"
                % (arr.name, got[0], got[1], len(got)))
        if got != want:
            problems.append(
                "%s=%s want %s (%s)" % (arr.name, fmt_bytes(got), fmt_bytes(want), label))
    if problems:
        return False, "; ".join(problems), {}
    return True, ('langid=0x0409, vendor="FootSwitch", product="HID Pedal", '
                  "serial empty; bLength/type headers valid"), {}


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

    ok, detail, _ = check_two_interfaces(arrays, extra)
    record("two_interfaces", ok, detail)

    ok, detail, _ = check_report_desc(arrays)
    record("report_desc", ok, detail)

    ok, detail, _ = check_vendor_feature(arrays)
    record("vendor_feature", ok, detail)

    ok, detail, _ = check_single_tlc(arrays)
    record("single_tlc", ok, detail)

    ok, detail, _ = check_keymap()
    record("keymap", ok, detail)

    ok, detail, _ = check_strings(arrays)
    record("strings", ok, detail)

    good = passed == N_CHECKS
    print("RESULT: %s (%d/%d checks)" % ("PASS" if good else "FAIL", passed, N_CHECKS))
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
