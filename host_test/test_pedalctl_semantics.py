#!/usr/bin/env python3
"""Host-side unit test for pedalctl.py: drives it against a fake device to prove
the strict semantic success checks and exit codes (0 = reply matches the request,
1 = device returned ERR, 2 = stale/missing reply or transport failure)."""
import contextlib
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pedalctl  # noqa: E402


class FakeDevice:
    """get_feature() pops responses; the last one repeats (device buffer)."""

    def __init__(self, responses):
        self.responses = [r if isinstance(r, bytes) else r.encode() for r in responses]
        self.sent = []

    def send_feature(self, payload):
        self.sent.append(bytes(payload))

    def get_feature(self):
        if len(self.responses) > 1:
            return self.responses.pop(0)
        return self.responses[0]


def run(fn, *args, **kwargs):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args, **kwargs)


failures = []


def check(name, got, want):
    status = "PASS" if got == want else "FAIL"
    if got != want:
        failures.append(name)
    print("%-58s got=%s want=%s  %s" % (name, got, want, status))


# --- set: strict success -------------------------------------------------- #
d = FakeDevice(["", "OK SET 1=0x2C\n"])
check("set 1 0x2C <- 'OK SET 1=0x2C'", run(pedalctl.command_set, d, 1, 0x2C, 1, 0.0), 0)
print("   sent:", d.sent)

d = FakeDevice(["", "ok set 2=0x1f\n"])          # case-insensitive reply
check("set 2 0x1F <- 'ok set 2=0x1f' (ignorecase)", run(pedalctl.command_set, d, 2, 0x1F, 1, 0.0), 0)

d = FakeDevice(["", "OK SET 1=0x2C\n"])
check("set 3 0x04 <- stale 'OK SET 1=0x2C'", run(pedalctl.command_set, d, 3, 0x04, 1, 0.0), 2)

d = FakeDevice(["", "OK SET 1=0x2D\n"])
check("set 1 0x2C <- wrong value 'OK SET 1=0x2D'", run(pedalctl.command_set, d, 1, 0x2C, 1, 0.0), 2)

d = FakeDevice(["", ""])
check("set 1 0x2C <- empty buffer (dropped)", run(pedalctl.command_set, d, 1, 0x2C, 1, 0.0), 2)

d = FakeDevice(["", "ERR range\n"])
check("set 1 0x68 <- 'ERR range'", run(pedalctl.command_set, d, 1, 0x68, 1, 0.0), 1)

d = FakeDevice(["", "OK SET 1=0x2C0\n"])         # word-boundary must reject
check("set 1 0x2C <- 'OK SET 1=0x2C0' rejected", run(pedalctl.command_set, d, 1, 0x2C, 1, 0.0), 2)

# --- reset / info --------------------------------------------------------- #
d = FakeDevice(["", "OK RESET\n"])
check("reset <- 'OK RESET'", run(pedalctl.command_reset, d, 1, 0.0), 0)
d = FakeDevice(["", "OK INFO fw=1.0 keys=0x1E,0x1F,0x20 dirty=0\n"])
check("reset <- stale INFO reply", run(pedalctl.command_reset, d, 1, 0.0), 2)
d = FakeDevice(["", "ERR syntax\n"])
check("reset <- 'ERR syntax'", run(pedalctl.command_reset, d, 1, 0.0), 1)

d = FakeDevice(["", "OK INFO fw=1.0 keys=0x1E,0x1F,0x20 dirty=0\n"])
check("info <- 'OK INFO ...'", run(pedalctl.command_info, d, 1, 0.0), 0)
d = FakeDevice(["", ""])
check("info <- empty (dropped)", run(pedalctl.command_info, d, 1, 0.0), 2)

# --- get ------------------------------------------------------------------ #
d = FakeDevice(["", "OK MAP 0x1E 0x1F 0x20\n"])
check("get <- 'OK MAP 0x1E 0x1F 0x20'", run(pedalctl.command_get, d, 1, 0.0), 0)
d = FakeDevice(["", "OK MAP 0x1E 0x1F\n"])
check("get <- truncated MAP", run(pedalctl.command_get, d, 1, 0.0), 2)
d = FakeDevice(["", "ERR unknown\n"])
check("get <- 'ERR unknown'", run(pedalctl.command_get, d, 1, 0.0), 1)

# --- save ----------------------------------------------------------------- #
d = FakeDevice(["", "OK saving\n", "OK saved\n"])
check("save <- saving then saved", run(pedalctl.command_save, d, 1, 0.0), 0)
d = FakeDevice(["", "OK saved\n"])
check("save <- immediate 'OK saved'", run(pedalctl.command_save, d, 1, 0.0), 0)
d = FakeDevice(["", "ERR flash\n"])
check("save <- 'ERR flash'", run(pedalctl.command_save, d, 1, 0.0), 1)
d = FakeDevice(["", "OK saving\n"])               # never completes
check("save <- timeout after 'OK saving'", run(pedalctl.command_save, d, 2, 0.0), 2)

# --- probe ---------------------------------------------------------------- #
d = FakeDevice(["", "OK INFO fw=1.0 keys=0x1E,0x1F,0x20 dirty=1\n"])
check("probe <- 'OK INFO ...'", run(pedalctl.command_probe, d, 1, 0.0), 0)
d = FakeDevice(["", "OK saved\n"])
check("probe <- stale 'OK saved'", run(pedalctl.command_probe, d, 1, 0.0), 2)

print()
print("FAILURES:", failures if failures else "none")
sys.exit(1 if failures else 0)
