#!/usr/bin/env python3
"""Run the actual profiler install recipe and inspect its four SDK archives.

Use a private SDK prefix. Missing either ABI/archive, accidentally putting the
startup member into the manual library, or installing a stale header must fail.
The linked runtime/anchor behavior is checked by the companion RPCS3 rows.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument("out", type=Path)
p.add_argument("--sdk", required=True, type=Path)
p.add_argument("--cross", required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out, sdk = a.out.resolve(), a.sdk.resolve()
out.mkdir(parents=True, exist_ok=False)
records = []


def run(argv, name):
    r = subprocess.run([str(x) for x in argv], capture_output=True, timeout=180)
    (out / (name + ".stdout")).write_bytes(r.stdout)
    (out / (name + ".stderr")).write_bytes(r.stderr)
    records.append({"argv": [str(x) for x in argv], "rc": r.returncode})
    (out / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
    return r


header = sdk / "ppu/include/ps3dk/prof.h"
# This is an installation test on a PRIVATE prefix: a stale header must not
# satisfy the byte check if the top-level header install forgets this path.
header.parent.mkdir(parents=True, exist_ok=True)
header.write_bytes(b"/* deliberately stale install-test header */\n")
headers = run(["make", "-C", root / "sdk", "install-headers",
               "PS3DEV=" + str(sdk), "PS3DK=" + str(sdk)], "headers")
result = run(["make", "-C", root / "sdk/libps3dkprof", "-j2", "install",
              "CROSS=" + a.cross, "PS3DEV=" + str(sdk), "PS3DK=" + str(sdk),
              "CFLAGS=-pg -finstrument-functions", "CPPFLAGS=-DPS3DK_PROF_TEST_HOOKS",
              "PS3_BUILD_ROOT=" + str(out / "build")], "install")
rows = []
for abi, subdir in [("ilp32", ""), ("lp64", "lp64")]:
    for name, automatic in [("ps3dkprof", False), ("ps3prof", True)]:
        archive = sdk / "ppu/lib" / subdir / ("lib" + name + ".a")
        errors = []
        if result.returncode:
            errors.append(f"make install rc={result.returncode}")
        if not archive.is_file():
            errors.append("installed archive missing")
        else:
            members = run([a.cross + "ar", "t", archive], abi + "-" + name + "-members")
            expected = {"prof_core.o", "prof_drain.o", "pal_lv2.o"}
            if automatic:
                expected |= {"startup.o", "prof_arcs_writer.o"}
            actual = members.stdout.decode().splitlines()
            if members.returncode or set(actual) != expected or len(actual) != len(expected):
                errors.append("wrong archive members: " + repr(actual))
            symbols = run([a.cross + "nm", "-g", archive], abi + "-" + name + "-symbols")
            defined = [s for s in symbols.stdout.decode().splitlines() if " U " not in s]
            has_anchor = any("__ps3dk_prof_startup_anchor" in s for s in defined)
            if symbols.returncode or has_anchor != automatic:
                errors.append("wrong startup anchor ownership")
            if any(hook in symbols.stdout.decode() for hook in ("_mcount", "__cyg_profile_func_", "ps3dk_prof_test_")):
                errors.append("application flags contaminated the runtime archive")
        row = {"case": abi + "-" + name, "pass": not errors, "errors": errors}
        if archive.is_file():
            row["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        rows.append(row)
        print(("PASS " if row["pass"] else "FAIL ") + row["case"] + ": " + "; ".join(errors))
ok = headers.returncode == 0 and header.is_file() and header.read_bytes() == (root / "sdk/include/ps3dk/prof.h").read_bytes()
rows.append({"case": "installed-header", "pass": ok})
print(("PASS " if ok else "FAIL ") + "installed-header")
(out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
passed = sum(x["pass"] for x in rows)
print(f"tests={len(rows)} pass={passed} fail={len(rows)-passed}")
raise SystemExit(0 if passed == len(rows) else 1)
