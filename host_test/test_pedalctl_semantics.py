#!/usr/bin/env python3
"""Host-side unit test for pedalctl.py: drives it against a fake device to prove
the strict semantic success checks and exit codes (0 = reply matches the request,
1 = device returned ERR, 2 = stale/missing reply or transport failure)."""
import contextlib
import io
import sys
from pathlib import Path

# pedalctl.py lives in the self-contained pedal_panel/ (the desktop tool);
# the test imports it from there so a single copy stays tested and shipped.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "pedal_panel"))
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
d = FakeDevice(["", "OK MAP M 0x1F 0x20\n"])
check("get <- macro marker M", run(pedalctl.command_get, d, 1, 0.0), 0)
d = FakeDevice(["", "OK MAP 0x1E M M\n"])
check("get <- two macro markers", run(pedalctl.command_get, d, 1, 0.0), 0)
d = FakeDevice(["", "OK MAP 0x1E 0x1F\n"])
check("get <- truncated MAP", run(pedalctl.command_get, d, 1, 0.0), 2)
d = FakeDevice(["", "ERR unknown\n"])
check("get <- 'ERR unknown'", run(pedalctl.command_get, d, 1, 0.0), 1)

# --- mac ------------------------------------------------------------------ #
STEPS_SZC = [(0x02, 0x00), (0x00, 0x1D), (0x00, 0x06)]   # shift, z, c
d = FakeDevice(["", "OK MAC 1 02:00,1D,06\n"])
check("mac 1 shift z c <- exact echo", run(pedalctl.command_mac, d, 1, STEPS_SZC, 1, 0.0), 0)
print("   sent:", d.sent)
check("mac: wire text is 'MAC 1 02:00,1D,06'",
      d.sent[0].rstrip(b"\x00"), b"MAC 1 02:00,1D,06")

d = FakeDevice(["", "ok mac 1 02:00,1d,06\n"])          # case-insensitive reply
check("mac <- lowercase echo", run(pedalctl.command_mac, d, 1, STEPS_SZC, 1, 0.0), 0)

d = FakeDevice(["", "OK MAC 2 02:00,1D,06\n"])
check("mac 1 <- stale pedal-2 echo", run(pedalctl.command_mac, d, 1, STEPS_SZC, 1, 0.0), 2)

d = FakeDevice(["", "OK MAC 1 02:00,1D\n"])
check("mac 1 <- truncated echo", run(pedalctl.command_mac, d, 1, STEPS_SZC, 1, 0.0), 2)

d = FakeDevice(["", "OK MAC 1 02:00,1D,06,06\n"])
check("mac 1 <- echo with an extra step", run(pedalctl.command_mac, d, 1, STEPS_SZC, 1, 0.0), 2)

d = FakeDevice(["", "ERR range\n"])
check("mac <- 'ERR range'", run(pedalctl.command_mac, d, 1, STEPS_SZC, 1, 0.0), 1)

d = FakeDevice(["", "OK MAC 1 02:00,1D,06\n"])
check("mac 1 <- '' (empty, dropped)", run(pedalctl.command_mac, FakeDevice(["", ""]), 1, STEPS_SZC, 1, 0.0), 2)

# step token parsing (client side)
check("parse 'shift' -> (0x02, 0x00)", pedalctl.parse_step_token("shift"), (0x02, 0x00))
check("parse 'z' -> (0x00, 0x1D)", pedalctl.parse_step_token("z"), (0x00, 0x1D))
check("parse 'shift+z' -> chord", pedalctl.parse_step_token("shift+z"), (0x02, 0x1D))
check("parse 'ctrl+shift+z' -> chord", pedalctl.parse_step_token("ctrl+shift+z"), (0x03, 0x1D))
check("parse '02:00' raw", pedalctl.parse_step_token("02:00"), (0x02, 0x00))
check("parse '1D' raw hex", pedalctl.parse_step_token("1D"), (0x00, 0x1D))
check("parse '0x1d' raw hex", pedalctl.parse_step_token("0x1d"), (0x00, 0x1D))
check("parse '1' -> digit key 0x1E", pedalctl.parse_step_token("1"), (0x00, 0x1E))
for bad in ("", "00", "00:00", "02:E1", "66", "z+c", "shift+", "1D:06:02", "zz"):
    try:
        pedalctl.parse_step_token(bad)
        check("parse %r rejected" % (bad,), "accepted", "ValueError")
    except ValueError:
        check("parse %r rejected" % (bad,), "ValueError", "ValueError")
check("encode (0x02, 0x00)", pedalctl.encode_step(0x02, 0x00), "02:00")
check("encode (0x00, 0x1D)", pedalctl.encode_step(0x00, 0x1D), "1D")
check("encode (0x02, 0x1D)", pedalctl.encode_step(0x02, 0x1D), "02:1D")
check("build_mac_command canonical",
      pedalctl.build_mac_command(1, STEPS_SZC), ("MAC 1 02:00,1D,06", "02:00,1D,06"))
try:
    pedalctl.build_mac_command(1, [(0x00, 0x1D)] * 7)
    check("7 steps rejected client-side", "accepted", "ValueError")
except ValueError:
    check("7 steps rejected client-side", "ValueError", "ValueError")
try:
    pedalctl.build_mac_command(1, [(0x02, 0x1D)] * 6)   # 41 chars: fits
    check("6 chord steps accepted", "ok", "ok")
except ValueError as exc:
    check("6 chord steps accepted", str(exc), "ok")

# --- mget ----------------------------------------------------------------- #
d = FakeDevice(["", "OK MAC 1 02:00,1D,06\n"])
check("mget 1 <- canonical steps", run(pedalctl.command_mget, d, 1, 1, 0.0), 0)
d = FakeDevice(["", "OK MAC 1 -\n"])
check("mget 1 <- '-' (no steps)", run(pedalctl.command_mget, d, 1, 1, 0.0), 0)
d = FakeDevice(["", "OK SET 1=0x04\n"])
check("mget 1 <- stale SET echo", run(pedalctl.command_mget, d, 1, 1, 0.0), 2)
d = FakeDevice(["", "OK MAC 1 02:00,zz\n"])
check("mget 1 <- non-canonical step", run(pedalctl.command_mget, d, 1, 1, 0.0), 2)
d = FakeDevice(["", "ERR pedal\n"])
check("mget 4 <- 'ERR pedal'", run(pedalctl.command_mget, d, 4, 1, 0.0), 1)

# --- tim ------------------------------------------------------------------ #
d = FakeDevice(["", "OK MTIM 1 15 25\n"])
check("tim 1 15 25 <- echo", run(pedalctl.command_tim, d, 1, 15, 25, 1, 0.0), 0)
print("   sent:", d.sent)
d = FakeDevice(["", "OK MTIM 1 15 26\n"])
check("tim 1 15 25 <- wrong gap echo", run(pedalctl.command_tim, d, 1, 15, 25, 1, 0.0), 2)
d = FakeDevice(["", "OK MTIM 2 15 25\n"])
check("tim 1 15 25 <- wrong pedal echo", run(pedalctl.command_tim, d, 1, 15, 25, 1, 0.0), 2)
d = FakeDevice(["", "OK MTIM 1 30 30\n"])
check("tim 1 (query) <- values", run(pedalctl.command_tim, d, 1, None, None, 1, 0.0), 0)
d = FakeDevice(["", "OK MAC 1 1D\n"])
check("tim 1 (query) <- stale MAC echo", run(pedalctl.command_tim, d, 1, None, None, 1, 0.0), 2)
d = FakeDevice(["", "ERR syntax\n"])
check("tim <- 'ERR syntax'", run(pedalctl.command_tim, d, 1, 15, 25, 1, 0.0), 1)

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

# --- api_* data layer (used by pedal_panel) -------------------------------- #
pedalctl.QUIET = True          # no CLI-style response dumps in this section

d = FakeDevice(["", "OK INFO fw=0.0.2 keys=M,0x1F,0x20 dirty=1\n"])
check("api_info parses", pedalctl.api_info(d),
      {"fw": "0.0.2", "keys": ["M", "0x1F", "0x20"], "dirty": True})
d = FakeDevice(["", "OK SET 1=0x04\n"])
check("api_info rejects stale", pedalctl.api_info(d), None)

d = FakeDevice(["", "OK MAP M 0x1F 0x20\n"])
check("api_map parses", pedalctl.api_map(d), ["M", "0x1F", "0x20"])
d = FakeDevice(["", "OK MAP 0x1E 0x1F\n"])
check("api_map truncated -> None", pedalctl.api_map(d), None)

d = FakeDevice(["", "OK SET 2=0x2C\n"])
check("api_set ok", pedalctl.api_set(d, 2, 0x2C), (True, "OK SET 2=0x2C"))
d = FakeDevice(["", "OK SET 1=0x2C\n"])
check("api_set stale wrong pedal -> False", pedalctl.api_set(d, 2, 0x2C)[0], False)
d = FakeDevice(["", "ERR range\n"])
check("api_set err -> False", pedalctl.api_set(d, 2, 0x68)[0], False)

STEPS_SZC = [(0x02, 0x00), (0x00, 0x1D), (0x00, 0x06)]
d = FakeDevice(["", "OK MAC 1 02:00,1D,06\n"])
check("api_mac ok", pedalctl.api_mac(d, 1, STEPS_SZC, 15, 25),
      (True, "OK MAC 1 02:00,1D,06", "02:00,1D,06"))
d = FakeDevice(["", "ERR range\n"])
check("api_mac device error -> False", pedalctl.api_mac(d, 1, [(0x02, 0)])[0], False)
check("api_mac 7 steps -> client-side False",
      pedalctl.api_mac(FakeDevice(["", ""]), 1, [(0x02, 0x1D)] * 7)[0], False)

d = FakeDevice(["", "OK MAC 1 02:00,1D,06\n"])
check("api_steps parses", pedalctl.api_steps(d, 1), [(2, 0), (0, 0x1D), (0, 6)])
d = FakeDevice(["", "OK MAC 1 -\n"])
check("api_steps none -> []", pedalctl.api_steps(d, 1), [])
d = FakeDevice(["", "OK MAP M 0x1F 0x20\n"])
check("api_steps stale -> None", pedalctl.api_steps(d, 1), None)

d = FakeDevice(["", "OK MTIM 1 15 25\n"])
check("api_timing query", pedalctl.api_timing(d, 1), (True, (15, 25), "OK MTIM 1 15 25"))
d = FakeDevice(["", "OK MTIM 1 15 25\n"])
check("api_timing set", pedalctl.api_timing(d, 1, 15, 25),
      (True, (15, 25), "OK MTIM 1 15 25"))
d = FakeDevice(["", "OK MTIM 1 15 26\n"])
check("api_timing set wrong echo -> False", pedalctl.api_timing(d, 1, 15, 25)[0], False)

d = FakeDevice(["", "OK saving\n", "OK saved\n"])
check("api_save ok", pedalctl.api_save(d), (True, "OK saved"))
d = FakeDevice(["", "OK saving\n"])
check("api_save timeout", pedalctl.api_save(d, 2, 0.0),
      (False, "timed out waiting for OK saved"))

d = FakeDevice(["", "OK RESET\n"])
check("api_reset ok", pedalctl.api_reset(d), (True, "OK RESET"))
d = FakeDevice(["", "ERR syntax\n"])
check("api_reset err -> False", pedalctl.api_reset(d)[0], False)

print()
print("FAILURES:", failures if failures else "none")
sys.exit(1 if failures else 0)
