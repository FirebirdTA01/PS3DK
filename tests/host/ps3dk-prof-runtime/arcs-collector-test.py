#!/usr/bin/env python3
"""Real collector -> serializer -> reader, plus counter/ownership invariants.

Caller holds the normal build slot. This is a host-only pure collector test;
producer registration, compiler ABI preservation and live snapshots are later
gates. No synthetic counts are described as observed compiler calls.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("out", type=Path)
p.add_argument("--reader", required=True)
p.add_argument("--sanitize", action="store_true")
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
commands = []


def run(label, argv, rc=0):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")
    r = subprocess.run([str(v) for v in argv], capture_output=True, env=env, timeout=120)
    (out / (label + ".stdout")).write_bytes(r.stdout)
    (out / (label + ".stderr")).write_bytes(r.stderr)
    commands.append({"argv": [str(v) for v in argv], "rc": r.returncode})
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    assert r.returncode == rc, f"{label}: rc={r.returncode}, expected {rc}; see {out}"
    return r.stdout.decode()


hooks = root / "sdk/libps3dkprof/src/hooks"
core = root / "sdk/libps3dkprof/src/core"
# A fixed flag list never inherits application CFLAGS. Clang does not accept
# GCC's -fno-instrument-functions; no instrumentation flag is enabled here.
flags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror"]
if a.sanitize:
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
cc = os.environ.get("CC", "cc")
obj = out / "arcs.o"
run("compile", [cc, *flags, "-I", hooks, "-c", hooks / "arcs.c", "-o", obj])
symbols = run("symbols", ["nm", "-u", obj])
# In a normal build the only external service permitted is cold-init memset.
# Sanitizer helper symbols are instrumentation and are checked by that run.
if not a.sanitize:
    assert all(line.split()[-1] == "memset" for line in symbols.splitlines()), symbols
exe = out / "collector"
run("link", [cc, *flags, "-I", hooks, Path(__file__).with_name("arcs_collector_test.c"),
             obj, core / "prof_arcs_writer.c", "-o", exe])
result = run("collector", [exe, out])
totals = re.search(r"collector tests=(\d+) pass=(\d+) fail=(\d+)", result)
assert totals and totals[1] == totals[2] and totals[3] == "0", result
print(totals[0], flush=True)

checks = 0
for name, complete in [("workers", True), ("capacity", False), ("max-exact", True),
                       ("hot-limit", False), ("refusal-limit", False)]:
    capture = out / (name + ".arcs")
    # arcs-json emits all observations even when incomplete; its exit status
    # still carries the same validity verdict as check-arcs.
    data = json.loads(run(name + "-json", [a.reader, "arcs-json", capture], 0 if complete else 1))
    assert data["complete"] is complete
    finals = data["finals"]
    if name == "workers":
        assert [(t["incarnation"], t["name"]) for t in data["threads"]] == [(1, "worker-a"), (2, "worker-b")]
        expected = {(0x1004, 0x2004): 10, (0x2008, 0x3004): 20,
                    (0x3008, 0x3004): 1, (0x2008, 0x4004): 1, (0, 0x4004): 1}
        for incarnation in (1, 2):
            actual = {(int(row["caller"], 16), int(row["callee"], 16)): int(row["count"])
                      for row in data["arcs"] if row["incarnation"] == incarnation}
            assert actual == expected
        assert all((f["attempted"], f["recorded"], f["refused"]) == ("33", "33", "0") for f in finals)
    elif name == "capacity":
        assert [(f["attempted"], f["recorded"], f["refused"]) for f in finals] == [("3", "2", "1")]
    elif name in ("max-exact", "hot-limit"):
        assert len(data["arcs"]) == 1 and data["arcs"][0]["count"] == str(2**64-1)
        assert finals[0]["recorded"] == str(2**64-1) and finals[0]["refused"] == "0"
        assert finals[0]["limit_flags"] == (0 if complete else 1)
    else:
        assert data["arcs"] == [] and finals[0]["recorded"] == "0"
        assert finals[0]["attempted"] == finals[0]["refused"] == str(2**64-1)
        assert finals[0]["limit_flags"] == 1
    checks += 1
    print(f"PASS {name}: complete={complete}", flush=True)
print(f"collector reader tests={checks} pass={checks} fail=0", flush=True)
