#!/usr/bin/env python3
"""Read Linux input events from the footswitch USB HID keyboard (1209:0001).

Device lookup order:
  1. --device /dev/input/eventN (explicit)
  2. /proc/bus/input/devices: block mentioning 1209 that looks like a
     keyboard (name/handlers), taking the eventX node from its Handlers line
  3. first /dev/input/by-id/*1209* match

Prints one line per EV_KEY event:
  t=<sec> type=EV_KEY code=<num> [(KEY_name)] value=<0|1|2>
and a final `RESULT: PASS|FAIL <reason>` line. Exit code 0 = pass,
1 = fail, 2 = no device found. Stdlib only.
"""

import argparse
import glob
import os
import re
import struct
import sys
import time
from pathlib import Path

VENDOR = "1209"

# struct input_event { struct timeval time; __u16 type; __u16 code; __s32 value; }
FMT_64 = "llHHi"   # 64-bit: long sec, long usec
FMT_32 = "iiHHi"   # 32-bit fallback
EV_SYN = 0x00
EV_KEY = 0x01
SYN_REPORT = 0

KEY_NAMES = {
    30: "KEY_1",
    31: "KEY_2",
    32: "KEY_3",
}


def find_device():
    """Return (device_path, source) or (None, reason)."""
    try:
        text = Path("/proc/bus/input/devices").read_text(errors="replace")
    except OSError:
        text = ""
    for block in re.split(r"\n[ \t]*\n", text):
        if VENDOR not in block:
            continue
        name_m = re.search(r'^N:\s*Name="([^"]*)"', block, re.M)
        name = name_m.group(1) if name_m else ""
        handlers_m = re.search(r"^H:\s*Handlers=(.*)$", block, re.M)
        handlers = handlers_m.group(1) if handlers_m else ""
        looks_keyboard = (
            "keyboard" in block.lower()
            or "kbd" in handlers.split()
            or VENDOR in name
        )
        if not looks_keyboard:
            continue
        ev = re.search(r"\bevent\d+\b", handlers)
        if ev:
            return "/dev/input/" + ev.group(0), "/proc/bus/input/devices"
    by_id = sorted(glob.glob("/dev/input/by-id/*%s*" % VENDOR))
    if by_id:
        for path in by_id:  # prefer the *event* link when several exist
            if "event" in os.path.basename(path):
                return path, "/dev/input/by-id"
        return by_id[0], "/dev/input/by-id"
    return None, "no device found (%s:*)" % VENDOR


def pick_format(chunk):
    """Choose the input_event struct layout matching the first chunk read."""
    for fmt in (FMT_64, FMT_32):
        size = struct.calcsize(fmt)
        if len(chunk) >= size and len(chunk) % size == 0:
            return fmt
    return FMT_64


def read_events(fd, duration):
    """Poll the fd for `duration` seconds; return list of (t, type, code, value)."""
    events = []
    fmt = None
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        try:
            data = os.read(fd, 64 * struct.calcsize(FMT_64))
        except BlockingIOError:
            time.sleep(0.005)
            continue
        if not data:
            time.sleep(0.005)
            continue
        if fmt is None:
            fmt = pick_format(data)
        size = struct.calcsize(fmt)
        for off in range(0, len(data) - (len(data) % size), size):
            sec, usec, etype, ecode, evalue = struct.unpack_from(fmt, data, off)
            events.append((sec + usec / 1e6, etype, ecode, evalue))
    return events


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Read EV_KEY events from the %s:* USB HID keyboard" % VENDOR)
    ap.add_argument("--device", help="explicit device node, e.g. /dev/input/event3")
    ap.add_argument("--find", action="store_true",
                    help="only print the located device node and exit")
    ap.add_argument("--duration", type=float, default=5.0,
                    help="seconds to listen (default: 5)")
    ap.add_argument("--expect-none", action="store_true",
                    help="pass only if zero EV_KEY events are seen")
    ap.add_argument("--expect-pairs", type=int, default=None, metavar="N",
                    help="pass only if exactly N press+release pairs are seen")
    ap.add_argument("--expect-codes", default=None, metavar="LIST",
                    help="comma-separated allowed/required codes, e.g. 30,31,32")
    ap.add_argument("--same-frame", action="store_true",
                    help="pass only if all --expect-codes appear within one "
                         "SYN_REPORT frame (combo detection)")
    args = ap.parse_args(argv)

    if args.device:
        dev, _source = args.device, "--device"
    else:
        dev, _source = find_device()

    if dev is None:
        print("RESULT: FAIL no device found (%s:*)" % VENDOR)
        return 2

    if args.find:
        print(dev)
        return 0

    if not os.path.exists(dev):
        print("RESULT: FAIL device node does not exist: %s" % dev)
        return 1

    expected_codes = None
    if args.expect_codes is not None:
        try:
            expected_codes = [int(tok.strip(), 0)
                              for tok in args.expect_codes.split(",") if tok.strip()]
        except ValueError:
            print("RESULT: FAIL bad --expect-codes value: %r" % args.expect_codes)
            return 1
        if not expected_codes:
            print("RESULT: FAIL empty --expect-codes list")
            return 1

    try:
        fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    except PermissionError:
        print("RESULT: FAIL permission denied opening %s "
              "(add your user to the 'input' group or use sudo)" % dev)
        return 1
    except OSError as exc:
        print("RESULT: FAIL cannot open %s: %s" % (dev, exc))
        return 1

    print("listening on %s for %gs ..." % (dev, args.duration))
    try:
        events = read_events(fd, args.duration)
    except OSError as exc:
        os.close(fd)
        print("RESULT: FAIL read error on %s: %s" % (dev, exc))
        return 1
    os.close(fd)

    key_events = []
    for t, etype, ecode, evalue in events:
        if etype != EV_KEY:
            continue
        key_events.append((t, ecode, evalue))
        name = KEY_NAMES.get(ecode)
        label = "%d (%s)" % (ecode, name) if name else str(ecode)
        print("t=%.6f type=EV_KEY code=%s value=%d" % (t, label, evalue))

    # --expect-codes: verify nothing outside the list, restrict counting to it.
    if expected_codes is not None:
        bad = sorted({c for _, c, _ in key_events if c not in expected_codes})
        if bad:
            print("RESULT: FAIL unexpected key codes %s seen (allowed: %s)"
                  % (bad, expected_codes))
            return 1
        counted = [(t, c, v) for t, c, v in key_events if c in expected_codes]
    else:
        counted = key_events

    if args.expect_none:
        if not key_events:
            print("RESULT: PASS zero EV_KEY events in %gs" % args.duration)
            return 0
        print("RESULT: FAIL %d EV_KEY events seen, expected none" % len(key_events))
        return 1

    if args.same_frame:
        if not expected_codes:
            print("RESULT: FAIL --same-frame requires --expect-codes")
            return 1
        need = set(expected_codes)
        frame = set()
        found = False
        for _t, etype, ecode, _value in events:
            if etype == EV_SYN and ecode == SYN_REPORT:
                if need <= frame:
                    found = True
                    break
                frame = set()
            elif etype == EV_KEY:
                frame.add(ecode)
        if not found and need <= frame:
            found = True
        if found:
            print("RESULT: PASS codes %s appeared within one SYN_REPORT frame"
                  % sorted(need))
            return 0
        print("RESULT: FAIL codes %s never appeared together within one "
              "SYN_REPORT frame" % sorted(need))
        return 1

    if args.expect_pairs is not None:
        presses = {}
        releases = {}
        for _t, c, v in counted:
            if v == 1:
                presses[c] = presses.get(c, 0) + 1
            elif v == 0:
                releases[c] = releases.get(c, 0) + 1
        pairs = sum(min(presses.get(c, 0), releases.get(c, 0))
                    for c in set(presses) | set(releases))
        if pairs == args.expect_pairs:
            print("RESULT: PASS exactly %d press+release pair(s)" % pairs)
            return 0
        print("RESULT: FAIL %d press+release pair(s) seen, expected %d"
              % (pairs, args.expect_pairs))
        return 1

    print("RESULT: PASS captured %d EV_KEY event(s) in %gs"
          % (len(key_events), args.duration))
    return 0


if __name__ == "__main__":
    sys.exit(main())
