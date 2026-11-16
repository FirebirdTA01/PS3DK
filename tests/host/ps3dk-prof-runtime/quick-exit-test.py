#!/usr/bin/env python3
"""The at_quick_exit drain (prof_drain.c p_quick_exit_drain) under the real core, drain and host PAL.

Schedules (quick_exit_app.c), each a fresh process with a 20 s timeout (a hang fails the row):
  plain     the handler drains once: the capture reads trailerless COMPLETE with the zone; a later shutdown is
            EALREADY (no seal can race the handler).
  midpass   the drain thread is parked inside a pass: the handler gives up within its bound (well under the 3 s park).
  callback  quick_exit from inside a sink callback: the handler skips; the later shutdown is EBUSY; no deadlock.
  after     shutdown, then quick_exit: the handler does nothing; the sealed file keeps its size and reads COMPLETE.
  cbmutex   a callback sink whose callback takes a mutex the exiting thread holds: no callback from quick_exit, a
            prompt exit.
  tmutex    a file sink with thread_mutex held by another thread for good: the bounded try-lock gives up (lock_failed)
            and the exit is prompt; the file keeps the HEADER and CLOCKS the init wrote.
--mutant no-bound replaces the bounded drain_busy retry with an unbounded one: midpass must then fail (the handler
 waits out the whole park).  --mutant blocking-lock restores the blocking thread_mutex lock in the quick_exit drain:
 tmutex must then fail (it hangs until the timeout).  --mutant callback-drain drains callback sinks too: cbmutex must
 then fail (the callback deadlocks on the held mutex).
usage: quick-exit-test.py OUT --reader PS3DK_PROF [--sanitize] [--mutant no-bound|blocking-lock|callback-drain]
       (exit 0 all pass, 1 otherwise)
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument("out", type=Path)
ap.add_argument("--reader", required=True, type=Path)
ap.add_argument("--sanitize", action="store_true")
ap.add_argument("--mutant", choices=["no-bound", "blocking-lock", "callback-drain"])
a = ap.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
lib = root / "sdk/libps3dkprof/src"
(out / "include/ps3dk").mkdir(parents=True)
(out / "include/ps3dk/prof.h").write_bytes((root / "sdk/include/ps3dk/prof.h").read_bytes())
cc = os.environ.get("CC", "cc")
flags = ["-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
         "-DPS3DK_PROF_PAL_HOST", "-DPS3DK_PROF=1", "-DPS3DK_PROF_TEST_HOOKS",
         "-I" + str(out / "include"), "-I" + str(lib / "core")]   # prof.h alone: the SDK's PS3 pthread.h is not the host's
if a.sanitize:
    flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer"]

drain = (lib / "core/prof_drain.c").read_text()
if a.mutant == "no-bound":
    cut = "        if (i + 1 >= P_QX_TRIES)\n            return;"
    if cut not in drain:
        sys.exit("mutant anchor missing")
    drain = drain.replace(cut, "        (void)i;", 1)
if a.mutant == "callback-drain":
    cut = "if (!p || p->snk.kind != 1 || t->in_drain"
    if cut not in drain:
        sys.exit("mutant anchor missing")
    drain = drain.replace(cut, "if (!p || t->in_drain", 1)
core = (lib / "core/prof_core.c").read_text()
if a.mutant == "blocking-lock":
    cut = "    if (g_tls.qx) {"
    if core.count(cut) != 1:
        sys.exit("mutant anchor missing")
    core = core.replace(cut, "    if (0) {", 1)
mdir = out / "src"
mdir.mkdir()
(mdir / "prof_drain.c").write_text(drain)   # its quoted includes resolve through -I core (../pal, ../auto_rsx)
(mdir / "prof_core.c").write_text(core)
sources = [mdir / "prof_core.c", mdir / "prof_drain.c", lib / "core/prof_version.c", lib / "pal/pal_host.c",
           root / "tests/host/ps3dk-prof-runtime/quick_exit_app.c"]
exe = out / "quick-exit"
b = subprocess.run([cc, *flags, *map(str, sources), "-o", str(exe)], capture_output=True, text=True)
(out / "build.log").write_text(b.stdout + b.stderr)
if b.returncode:
    print("FAIL build (build.log)")
    sys.exit(1)

EBUSY, EALREADY = -2, -3
fails = 0


def row(name, ok, info=""):
    global fails
    fails += not ok
    print(("PASS " if ok else "FAIL ") + name + (": " + info if info else ""))


for mode in ["plain", "midpass", "callback", "after", "cbmutex", "tmutex"]:
    d = out / mode
    d.mkdir()
    try:
        r = subprocess.run([str(exe), mode], cwd=d, capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired:
        row(mode, False, "hung (20 s timeout)")
        continue
    (d / "stdout").write_text(r.stdout)
    (d / "stderr").write_text(r.stderr)
    m = re.search(r"witness mode=(\w+) elapsed_ms=(-?\d+) shutdown=(-?\d+) size_same=(\d) cb_calls=(\d+) "
                  r"lock_failed=(-?\d+)", r.stdout)
    if r.returncode != 0 or not m:
        row(mode, False, f"exit {r.returncode}, witness {'missing' if not m else 'ok'}")
        continue
    ms, shut, same = int(m.group(2)), int(m.group(3)), m.group(4) == "1"
    cb_calls, lock_failed = int(m.group(5)), int(m.group(6))
    files = list(d.glob("*.prof"))
    rep = None
    if files:
        rep = subprocess.run([str(a.reader), "report", str(files[0])], capture_output=True, text=True)
        (d / "reader.txt").write_text(rep.stdout + rep.stderr)
    text = rep.stdout if rep else ""
    trailerless = rep is not None and rep.returncode == 0 and "ending: NO-TRAILER" in text
    if mode == "plain":
        row(mode, trailerless and "qx-zone: 1 instances" in text and shut == EALREADY,
            f"trailerless={trailerless} shutdown={shut}")
    elif mode == "midpass":
        row(mode, ms < 1000 and len(files) == 1 and "clock: PPU timebase" in text, f"handler {ms} ms")
    elif mode == "callback":
        row(mode, shut == EBUSY and not files, f"shutdown={shut}")
    elif mode == "cbmutex":
        row(mode, cb_calls == 0 and ms < 1000 and not files, f"callbacks {cb_calls}, handler {ms} ms")
    elif mode == "tmutex":
        row(mode, lock_failed == 1 and ms < 1000 and len(files) == 1 and "clock: PPU timebase" in text,
            f"lock_failed={lock_failed}, handler {ms} ms")
    else:
        complete = rep is not None and rep.returncode == 0 and "TRANSPORT COMPLETE" in text
        row(mode, same and complete and shut == EALREADY, f"size_same={same} complete={complete} shutdown={shut}")

print(f"quick-exit-test: {'PASS' if not fails else 'FAIL'} ({fails} failed)")
sys.exit(1 if fails else 0)
