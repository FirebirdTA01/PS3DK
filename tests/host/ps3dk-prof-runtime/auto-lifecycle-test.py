#!/usr/bin/env python3
"""Build real lifecycle/core/PAL and check captures with the production reader.

Removing startup, finalization or retirement must fail the complete cases;
inventing retirement/final drainage must fail the abandoned/abrupt cases.
The pre-implementation baseline substitutes only an empty archive anchor and
notification, and is explicitly selected with --baseline.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument("out", type=Path)
p.add_argument("--reader", required=True, type=Path)
p.add_argument("--baseline", action="store_true")
p.add_argument("--sanitize", action="store_true")
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
inc = out / "include/ps3dk"
inc.mkdir(parents=True)
(inc / "prof.h").write_bytes((root / "sdk/include/ps3dk/prof.h").read_bytes())
commands = []


def run(argv, name, cwd=out):
    r = subprocess.run([str(x) for x in argv], cwd=cwd, capture_output=True, timeout=60)
    (out / (name + ".stdout")).write_bytes(r.stdout)
    (out / (name + ".stderr")).write_bytes(r.stderr)
    commands.append({"argv": [str(x) for x in argv], "cwd": str(cwd), "rc": r.returncode})
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    return r


def build(argv, name):
    r = run(argv, name)
    if r.returncode:
        raise SystemExit(f"build failed: {name}: {r.stderr.decode(errors='replace')}")


cc, cxx = os.environ.get("CC", "cc"), os.environ.get("CXX", "c++")
def priority_warning(compiler):
    version = run([compiler, "--version"], Path(compiler).name + "-version")
    if version.returncode:
        raise SystemExit("compiler version probe failed")
    return [] if b"clang" in version.stdout.lower() else ["-Wno-prio-ctor-dtor"]


c_priority, cxx_priority = priority_warning(cc), priority_warning(cxx)
flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
         "-DPS3DK_PROF_PAL_HOST", "-DPS3DK_PROF=1", "-I" + str(inc.parent)]
if a.sanitize:
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie"]
lib = root / "sdk/libps3dkprof/src"
sources = [lib / "core/prof_core.c", lib / "core/prof_drain.c", lib / "core/prof_version.c", lib / "pal/pal_host.c"]
if a.baseline:
    stub = out / "absent-startup.c"
    stub.write_text("void __ps3dk_prof_startup_anchor(void) {}\n"
                    "void __ps3dk_prof_thread_exit(void) {}\n")
    sources.append(stub)
else:
    sources.append(lib / "auto/startup.c")
objects = []
for i, source in enumerate(sources):
    obj = out / f"lib-{i}.o"
    build([cc, "-std=c11", *flags, *c_priority, "-c", source, "-o", obj], f"compile-{i}")
    objects.append(obj)
archive = out / "libps3prof.a"
build(["ar", "rcs", archive, *objects], "archive")
exe = out / "lifecycle"
link = ["-no-pie"] if a.sanitize else []
build([cxx, "-std=c++17", *flags, *cxx_priority, "-DPS3PROF_HOST_EXIT_MODEL",
       root / "tests/host/ps3dk-prof-runtime/auto_lifecycle.cpp",
       "-Wl,-u,__ps3dk_prof_startup_anchor", archive, *link, "-o", exe], "link")

results = []
for mode in ["normal", "exit", "unregistered", "open", "abandoned", "immediate", "quick"]:
    directory = out / mode
    directory.mkdir()
    r = run([exe, mode], mode, directory)
    files = list(directory.glob("*.prof"))
    errors = []
    if r.returncode != 0:
        errors.append(f"application exit {r.returncode}")
    if len(files) != 1:
        errors.append(f"expected one capture, got {len(files)}")
    else:
        report = run([a.reader, "report", files[0]], mode + "-reader")
        text = report.stdout.decode(errors="replace")
        if mode in ("normal", "exit", "unregistered"):
            if report.returncode != 0 or "MEASUREMENT VALID" not in text:
                errors.append("expected complete valid capture")
            names = ["early-user", "late-user", "user-object-ctor", "user-object-dtor", "main"]
            if mode != "unregistered":
                names += ["worker", "worker-tls-cleanup"]
                if "  worker-key-cleanup: 2 instances" not in text:
                    errors.append("missing two key-cleanup rounds")
            for name in names:
                if f"  {name}: 1 instances" not in text:
                    errors.append(f"missing exact single zone {name}")
        elif mode == "abandoned":
            if report.returncode != 1 or "NOT_QUIESCED" not in text:
                errors.append("missing unretired-producer refusal")
        elif mode == "open":
            # v1 retains a valid lane but censors this unfinished instance.
            if (report.returncode != 0 or "1 censored of 1" not in text
                    or "duration unknown (censored)" not in text):
                errors.append("open worker span incorrectly accepted")
        elif (report.returncode != 0 or "verdict: COMPLETE (ended without a trailer" not in text
              or "ending: NO-TRAILER" not in text or "TRAILER is missing" in text):
            # a valid capture that ends without a trailer reads COMPLETE, exit 0, ending NO-TRAILER
            errors.append("abrupt exit not read as a trailerless complete capture")
    results.append({"case": mode, "pass": not errors, "errors": errors})
    print(("PASS " if not errors else "FAIL ") + mode + (": " + "; ".join(errors) if errors else ""))

# Force failure of the real PAL allocation without changing startup/core code.
fault = out / "allocation-fault.c"
fault.write_text("#include <stddef.h>\n#include <errno.h>\n"
                 "int __wrap_posix_memalign(void **p, size_t a, size_t n) "
                 "{ (void)p; (void)a; (void)n; return ENOMEM; }\n")
build([cc, *flags, "-c", fault, "-o", out / "fault.o"], "fault-compile")
build([cxx, "-std=c++17", *flags, *cxx_priority, "-DPS3PROF_HOST_EXIT_MODEL",
       root / "tests/host/ps3dk-prof-runtime/auto_lifecycle.cpp", out / "fault.o",
       "-Wl,--wrap=posix_memalign,-u,__ps3dk_prof_startup_anchor", archive, *link,
       "-o", out / "allocation-failure"], "fault-link")
directory = out / "init-failure"
directory.mkdir()
r = run([out / "allocation-failure"], "init-failure", directory)
ok = (r.returncode == 0 and not list(directory.glob("*.prof"))
      and b"automatic initialization failed" in r.stderr)
results.append({"case": "init-failure", "pass": ok})
print(("PASS " if ok else "FAIL ") + "init-failure")

# Linking the archive without the forced anchor must leave normal applications
# untouched. This fixture has no profiler API references or compiler hooks.
control = out / "off.c"
control.write_text("int main(void) { return 0; }\n")
build([cc, *flags, control, archive, *link, "-o", out / "off"], "off-link")
directory = out / "off-control"
directory.mkdir()
r = run([out / "off"], "off-control", directory)
ok = r.returncode == 0 and not list(directory.glob("*.prof")) and not r.stderr
results.append({"case": "off-control", "pass": ok})
print(("PASS " if ok else "FAIL ") + "off-control")
(out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
passed = sum(x["pass"] for x in results)
print(f"tests={len(results)} pass={passed} fail={len(results)-passed}")
raise SystemExit(0 if passed == len(results) else 1)
