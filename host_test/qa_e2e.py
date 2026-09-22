#!/usr/bin/env python3
"""End-to-end acceptance test: USB enumeration + HID key delivery.

Switch presses are injected over SWD (pyocd) while evdev_read.py listens on the
host, so the whole chain is exercised for real: USB enumeration -> EP1 IN
reports -> Linux input events.

Requires: the board's OWN micro-USB plugged into this host (VM passthrough),
the DAPLink connected, and pyocd available.
"""

import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
EVDEV = ROOT / "evdev_read.py"
PYOCD = PROJECT.parent / "venv" / "bin" / "pyocd"

VIDPID = "1209:0001"

GPIOB_CRL = 0x40010C00
GPIOB_CRH = 0x40010C04
GPIOB_BSRR = 0x40010C10
GPIOB_BRR = 0x40010C14

# (press reg, press val, press bit, release reg, release val, release bit)
SW = {
    "SW1": (GPIOB_CRH, 0x00000028, 0x00000200, GPIOB_CRH, 0x00000088, 0x00000200),
    "SW2": (GPIOB_CRH, 0x00000022, 0x00000100, GPIOB_CRH, 0x00000088, 0x00000100),
    "SW3": (GPIOB_CRL, 0x20000000, 0x00000080, GPIOB_CRL, 0x80000000, 0x00000080),
}
KEYCODE = {"SW1": 2, "SW2": 3, "SW3": 4}

results = []


def record(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}", flush=True)


def pyocd(cmds, timeout=20):
    args = [str(PYOCD), "cmd", "-t", "cortex_m"]
    for c in cmds:
        args += ["-c", c]
    args += ["-c", "exit"]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def press(name):
    reg, val, bit, _, _, _ = SW[name]
    pyocd([f"write32 0x{reg:08X} 0x{val:08X}", f"write32 0x{GPIOB_BRR:08X} 0x{bit:08X}"])


def release(name):
    _, _, _, reg, val, bit = SW[name]
    pyocd([f"write32 0x{reg:08X} 0x{val:08X}", f"write32 0x{GPIOB_BSRR:08X} 0x{bit:08X}"])


def release_all():
    for name in SW:
        release(name)


def tap(name, hold=0.4):
    press(name)
    time.sleep(hold)
    release(name)


def lsusb_instances():
    r = subprocess.run(["lsusb", "-d", VIDPID], capture_output=True, text=True)
    return len([l for l in r.stdout.splitlines() if "1209" in l])


def find_device():
    r = subprocess.run([sys.executable, str(EVDEV), "--find"], capture_output=True, text=True)
    return r.returncode == 0, r.stdout.strip()


def parse_events(text):
    events = []
    for line in text.splitlines():
        m = re.search(r"t=([\d.]+) type=EV_KEY code=(\d+) value=(\d)", line)
        if m:
            events.append((float(m.group(1)), int(m.group(2)), int(m.group(3))))
    return events


def can_read(path):
    try:
        os.close(os.open(path, os.O_RDONLY | os.O_NONBLOCK))
        return True
    except OSError:
        return False


def sysfs_evidence():
    found = []
    for vendor_file in Path("/sys/bus/usb/devices").glob("*/idVendor"):
        try:
            if vendor_file.read_text().strip().lower() != "1209":
                continue
            dev = vendor_file.parent
            keys = ("idVendor", "idProduct", "manufacturer", "product", "bNumInterfaces", "bDeviceClass")
            info = {k: (dev / k).read_text().strip() for k in keys if (dev / k).exists()}
            found.append(f"{dev.name}: " + " ".join(f"{k}={v}" for k, v in info.items()))
        except OSError:
            pass
    return found


def main():
    print("=== footswitch_hid 端到端验收 (S1-S5) ===\n")

    if not PYOCD.exists():
        print(f"pyocd not found at {PYOCD}")
        return 2

    n = lsusb_instances()
    record("S1 lsusb shows 1209:0001 exactly once", n == 1, f"count={n}")
    ok, out = find_device()
    record("S1 evdev input node found", ok, out.splitlines()[-1] if out else "")

    r = subprocess.run(["dmesg"], capture_output=True, text=True)
    if r.returncode == 0:
        hits = [l for l in r.stdout.splitlines() if "1209" in l or "Keyboard" in l][-2:]
        record("S1 dmesg mentions the device", bool(hits), " | ".join(h.strip() for h in hits)[:140])
    else:
        ev = sysfs_evidence()
        record("S1 sysfs USB descriptors (dmesg restricted)", bool(ev), " | ".join(ev)[:180])

    if not ok:
        print("\n设备未找到: 请把开发板自己的 micro-USB 插入电脑 (VM 需 USB 直通)。")
        return 2

    node = out.strip().splitlines()[-1].strip()
    if not can_read(node):
        print(f"\n无权读取 {node} (该节点通常是 root:input 660)。一次性修复(需 sudo 密码):")
        print("  A) sudo usermod -aG input $USER    # 之后重新登录; 本次可用: sg input -c 'python3 host_test/qa_e2e.py'")
        print("  B) sudo tee /etc/udev/rules.d/99-input-plugdev.rules <<< 'SUBSYSTEM==\"input\", KERNEL==\"event*\", GROUP=\"plugdev\", MODE=\"0660\"'")
        print("     sudo udevadm control --reload && sudo udevadm trigger")
        return 2

    print("\n--- S5: 静置 5s 应无按键事件 ---", flush=True)
    r = subprocess.run([sys.executable, str(EVDEV), "--duration", "5", "--expect-none"],
                       capture_output=True, text=True)
    record("S5 idle 5s -> no key events", r.returncode == 0,
           r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "")

    print("\n--- S2/S3: 采集 30s, 期间用 SWD 注入踩键 ---", flush=True)
    proc = subprocess.Popen([sys.executable, str(EVDEV), "--duration", "30"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)

    print("  inject: SW1 tap, SW2 tap, SW3 tap, combo SW1+SW2, 3x SW1 taps", flush=True)
    tap("SW1")
    time.sleep(0.5)
    tap("SW2")
    time.sleep(0.5)
    tap("SW3")
    time.sleep(0.5)
    press("SW1")
    press("SW2")
    time.sleep(0.6)
    release_all()
    time.sleep(0.5)
    for _ in range(3):
        tap("SW1", 0.3)
        time.sleep(0.3)

    out = proc.communicate(timeout=40)[0]
    events = parse_events(out)
    codes = sorted({c for _, c, _ in events})

    for name, code in KEYCODE.items():
        downs = sum(1 for _, c, v in events if c == code and v == 1)
        ups = sum(1 for _, c, v in events if c == code and v == 0)
        record(f"S2 {name} -> KEY code {code} down/up", downs >= 1 and ups >= 1,
               f"down={downs} up={ups}")

    sw1, sw2 = KEYCODE["SW1"], KEYCODE["SW2"]
    combo_ok = False
    for i, (_, c, v) in enumerate(events):
        if v == 1 and c in (sw1, sw2):
            seen = {c}
            for _, c2, v2 in events[i + 1:]:
                if v2 == 1 and c2 in (sw1, sw2):
                    seen.add(c2)
                if v2 == 0:
                    break
            if len(seen) == 2:
                combo_ok = True
                break
    record("S3 combo SW1+SW2 pressed in the same frame", combo_ok)

    sw1_pairs = min(sum(1 for _, c, v in events if c == sw1 and v == 1),
                    sum(1 for _, c, v in events if c == sw1 and v == 0))
    record("S3 SW1 pairs == 1(tap)+1(combo)+3(taps) = 5", sw1_pairs == 5, f"pairs={sw1_pairs}")

    unexpected = [c for c in codes if c not in tuple(KEYCODE.values())]
    record("S3 no unexpected key codes", not unexpected, f"codes={codes}")

    print("\n--- S4: 复位键 -> 重新枚举 ---", flush=True)
    print("  请按开发板复位键(SW1)... 3s 内自动检测", flush=True)
    time.sleep(3)
    t0 = time.time()
    ok4 = False
    while time.time() - t0 < 8:
        if lsusb_instances() >= 1 and find_device()[0]:
            ok4 = True
            break
        time.sleep(0.25)
    record("S4 re-enumerated after reset", ok4, f"{time.time() - t0:.1f}s")
    record("S4 still exactly one instance", lsusb_instances() == 1, f"count={lsusb_instances()}")

    print("\n--- 原始事件日志 ---")
    print(out.strip() if out.strip() else "(无事件)")

    print("\n=== 汇总 ===")
    bad = sum(1 for _, ok_, _ in results if not ok_)
    for name, ok_, detail in results:
        print(f"{'PASS' if ok_ else 'FAIL'}  {name}  {detail}")
    print(f"\nRESULT: {'PASS' if bad == 0 else f'FAIL ({bad} item(s))'}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
