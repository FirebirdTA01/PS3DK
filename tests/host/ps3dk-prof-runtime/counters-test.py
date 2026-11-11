#!/usr/bin/env python3
"""Named per-frame counters, end to end on the host PAL, and the D4 check that zone records are unchanged.

Builds counters_test.c with the production core, drain and PAL twice: the default runtime and the counters runtime
(-DPS3DK_PROF_WITH_COUNTERS, src/counters).  Each writes a capture; the reader must accept both with `check`.

Counters: `ps3dk-prof counters --json` on the counters capture must show exactly the expected names, samples,
per-frame values, refused = 12 and dropped = 0, FINAL seen; the default capture must carry no capability.

D4: an independent decoder here (not the reader) walks both files' DESCRIPTORS and RECORDS chunks and compares the
zone names and, per thread name, the record kind/depth/descriptor-name sequence (timestamps and span ids differ
between runs and are ignored).  They must be identical, so the counters runtime leaves wait.* / drain.* zones and
their records unchanged.  The file header differs only in file_flags bit 8.

Lock failures: counters_lock_test.c, built with the test hooks, makes the drainer's counter-mutex lock fail once
at each site (the drain snapshot, the final freeze, an emit before the sink, an emit after the sink accepted the
chunk; ordinary and final).  Each failure must fail the capture (the drain and the shutdown return ESINKFAIL, no
TRAILER), no FINAL may follow a failed freeze or copy, and no name or sample may be emitted twice.  A mutant whose
drain discards the emission error must fail.  --lock-only runs just these cases (no reader needed).
"""
import argparse, json, os, shutil, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "sdk/libps3dkprof/src"


def build(out, counters, cc):
    exe = out / ("counters" if counters else "default")
    # only the profiler's public headers: the rest of sdk/include (pthread.h, ...) is target-only
    inc = out / "include" / "ps3dk"
    inc.mkdir(parents=True, exist_ok=True)
    for h in ("prof.h", "prof_counter.h"):
        (inc / h).write_bytes((ROOT / "sdk/include/ps3dk" / h).read_bytes())
    flags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread", "-Wno-prio-ctor-dtor",
             "-DPS3DK_PROF=1", "-DPS3DK_PROF_PAL_HOST", "-I", str(inc.parent), "-I", str(SRC / "core")]
    files = [SRC / f for f in ("core/prof_core.c", "core/prof_drain.c", "core/prof_version.c", "pal/pal_host.c")]
    if counters:
        flags.append("-DPS3DK_PROF_WITH_COUNTERS=1")
        files += sorted((SRC / "counters").glob("*.c"))
    if os.environ.get("SANITIZE"):
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-ftls-model=local-exec"]
    cmd = [cc, *flags, *map(str, files), str(Path(__file__).with_name("counters_test.c")), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (out / (exe.name + ".compile.txt")).write_text(" ".join(cmd) + "\n" + r.stdout + r.stderr)
    if r.returncode:
        sys.exit(f"FAIL: compile {exe.name}: {r.stderr}")
    return exe


def chunks(data):
    assert data[:8] == b"PS3DKPRF", "magic"
    flags = struct.unpack_from("<I", data, 12)[0]
    pos, out = 16, []
    while pos < len(data):
        ty, ln = struct.unpack_from("<II", data, pos)
        out.append((ty, data[pos + 16:pos + 16 + ln]))
        pos += 16 + ln + 4
    return flags, out


def string(p, at):
    n = struct.unpack_from("<H", p, at)[0]
    return p[at + 2:at + 2 + n].decode(), at + 2 + n


def zones(data):
    """(descriptor names by index, {thread name: [(kind, depth, desc name or '')...]})"""
    flags, cs = chunks(data)
    descs, threads, recs = {}, {}, {}
    for ty, p in cs:
        if ty == 3:
            n, at = struct.unpack_from("<I", p, 0)[0], 4
            for _ in range(n):
                idx = struct.unpack_from("<I", p, at)[0]
                at += 4 + 8 + 1 + 1 + 4
                name, at = string(p, at)
                _, at = string(p, at)
                _, at = string(p, at)
                descs[idx] = name
        elif ty == 4:
            n, at = struct.unpack_from("<I", p, 0)[0], 4
            for _ in range(n):
                inc = struct.unpack_from("<I", p, at)[0]
                at += 4 + 4 + 8
                name, at = string(p, at)
                threads[inc] = name
        elif ty == 5:
            inc, n = struct.unpack_from("<II", p, 0)
            for i in range(n):
                kind, depth, _flags, a = struct.unpack_from("<BBHI", p, 8 + 32 * i)
                ref = descs.get(a, "?") if kind in (1, 3) else ""
                recs.setdefault(threads[inc], []).append((kind, depth, ref))
    return flags, sorted(descs.values()), recs


# Counter-mutex lock failures (counters_lock_test.c, the counter_lock_fail_site hook): (name, site, skip, expected).
# drain = the result of both manual drains (a failed first drain fails the second too), "fail" = PS3DK_PROF_ESINKFAIL.
# Every failing case must leave no TRAILER and no FINAL COUNTERS chunk unless the sink accepted that FINAL before
# the failure (commit-final), and no case may emit a name or a sample twice.
ESINKFAIL = -10
LOCK_CASES = [
    ("no-failure", 0, 0, {"drains": (0, 0), "shutdown": 0, "trailer": True, "final": True}),
    ("snapshot", 1, 0, {"drains": (ESINKFAIL, ESINKFAIL), "shutdown": ESINKFAIL, "trailer": False, "final": False}),
    ("freeze", 2, 0, {"drains": (0, 0), "shutdown": ESINKFAIL, "trailer": False, "final": False}),
    ("copy", 3, 0, {"drains": (ESINKFAIL, ESINKFAIL), "shutdown": ESINKFAIL, "trailer": False, "final": False}),
    ("copy-final", 3, 2, {"drains": (0, 0), "shutdown": ESINKFAIL, "trailer": False, "final": False}),
    ("commit", 4, 0, {"drains": (ESINKFAIL, ESINKFAIL), "shutdown": ESINKFAIL, "trailer": False, "final": False}),
    ("commit-final", 4, 2, {"drains": (0, 0), "shutdown": ESINKFAIL, "trailer": False, "final": True}),
]


def build_lock(out, cc, src, name):
    inc = out / "include" / "ps3dk"
    inc.mkdir(parents=True, exist_ok=True)
    for h in ("prof.h", "prof_counter.h"):
        (inc / h).write_bytes((ROOT / "sdk/include/ps3dk" / h).read_bytes())
    exe = out / name
    flags = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread", "-Wno-prio-ctor-dtor",
             "-DPS3DK_PROF=1", "-DPS3DK_PROF_PAL_HOST", "-DPS3DK_PROF_WITH_COUNTERS=1", "-DPS3DK_PROF_TEST_HOOKS",
             "-I", str(inc.parent), "-I", str(src / "core")]
    if os.environ.get("SANITIZE"):
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-ftls-model=local-exec"]
    files = [src / f for f in ("core/prof_core.c", "core/prof_drain.c", "core/prof_version.c", "pal/pal_host.c")]
    files += sorted((src / "counters").glob("*.c"))
    cmd = [cc, *flags, *map(str, files), str(Path(__file__).with_name("counters_lock_test.c")), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (out / (name + ".compile.txt")).write_text(" ".join(cmd) + "\n" + r.stdout + r.stderr)
    if r.returncode:
        sys.exit(f"FAIL: compile {name}: {r.stderr}")
    return exe


def counter_chunks(data):
    """[(final, [name ids], [32-byte samples])] of every COUNTERS chunk, and whether a TRAILER was written."""
    _, cs = chunks(data)
    found = []
    for ty, p in cs:
        if ty != 39:
            continue
        final, nn, ns = struct.unpack_from("<HII", p, 2)
        at, ids = 32, []
        for _ in range(nn):
            ident, ln = struct.unpack_from("<IH", p, at)
            ids.append(ident)
            at += 6 + ln
        found.append((final, ids, [p[at + 32 * i:at + 32 * (i + 1)] for i in range(ns)]))
    return found, any(ty == 6 for ty, _ in cs)


def lock_cases(out, cc, src, tag):
    """Run every lock case against the runtime in `src`; return the names of the cases that failed."""
    exe = build_lock(out, cc, src, f"lock-{tag}")
    failed = []
    for name, site, skip, want in LOCK_CASES:
        cap = out / f"lock-{tag}-{name}.prof"
        r = subprocess.run([str(exe), str(site), str(skip), str(cap)], capture_output=True, text=True)
        (out / f"lock-{tag}-{name}.run.txt").write_text(r.stdout + r.stderr)
        problems = []
        if r.returncode:
            problems.append(f"driver rc={r.returncode}")
        else:
            got = dict(kv.split("=") for kv in r.stdout.split())
            drains, shutdown = (int(got["drain1"]), int(got["drain2"])), int(got["shutdown"])
            found, trailer = counter_chunks(cap.read_bytes())
            ids = [i for _, n, _ in found for i in n]
            samples = [s for _, _, ss in found for s in ss]
            finals = sum(1 for f, _, _ in found if f)
            if drains != want["drains"]:
                problems.append(f"drains {drains} (want {want['drains']})")
            if shutdown != want["shutdown"]:
                problems.append(f"shutdown {shutdown} (want {want['shutdown']})")
            if trailer != want["trailer"]:
                problems.append(f"trailer {trailer} (want {want['trailer']})")
            if finals != (1 if want["final"] else 0):
                problems.append(f"{finals} FINAL chunk(s) (want {1 if want['final'] else 0})")
            if len(ids) != len(set(ids)) or len(samples) != len(set(samples)):
                problems.append(f"emitted twice: names {sorted(ids)}, {len(samples)} samples, "
                                f"{len(set(samples))} distinct")
            if name == "no-failure" and (len(set(ids)) != 3 or len(samples) != 6):
                problems.append(f"no-failure capture: {len(set(ids))} names, {len(samples)} samples (want 3, 6)")
        print(f"  lock {tag} {name}: " + ("ok" if not problems else "FAIL " + "; ".join(problems)))
        if problems:
            failed.append(name)
    return failed


def lock_suite(out, cc):
    """The fixed runtime passes every lock case; a mutant whose drain discards the emission error must fail."""
    ok = not lock_cases(out, cc, SRC, "runtime")
    mut = out / "mutant-src"
    shutil.copytree(SRC, mut)
    drain = mut / "core/prof_drain.c"
    text = drain.read_text()
    edited = text.replace("if (p_counter_emit(p, 0) != 0)", "if ((void)p_counter_emit(p, 0), 0)")
    if edited == text:
        sys.exit("FAIL: the discard-emission mutant edit did not apply")
    drain.write_text(edited)
    failed = lock_cases(out, cc, mut, "mutant")
    if "copy" not in failed:
        print("  lock mutant: FAIL the discard-emission mutant passed the copy case")
        ok = False
    else:
        print(f"  lock mutant: the discard-emission mutant fails {', '.join(failed)}")
    print("counter lock failures:", "PASS" if ok else "FAIL")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--reader")
    ap.add_argument("--lock-only", action="store_true", help="run only the counter-mutex lock-failure cases")
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cc = os.environ.get("CC", "cc")
    if a.lock_only:
        return 0 if lock_suite(out, cc) else 1
    if not a.reader:
        ap.error("--reader is required unless --lock-only")
    lock_ok = lock_suite(out, cc)
    caps = {}
    for counters in (False, True):
        exe = build(out, counters, cc)
        cap = out / (exe.name + ".prof")
        r = subprocess.run([str(exe), str(cap)], capture_output=True, text=True)
        (out / (exe.name + ".run.txt")).write_text(r.stdout + r.stderr)
        if r.returncode:
            sys.exit(f"FAIL: driver {exe.name} rc={r.returncode}: {r.stdout}{r.stderr}")
        c = subprocess.run([a.reader, "check", str(cap)], capture_output=True, text=True)
        (out / (exe.name + ".check.txt")).write_text(c.stdout + c.stderr)
        if c.returncode:
            sys.exit(f"FAIL: reader check {exe.name} rc={c.returncode}: {c.stdout}{c.stderr}")
        caps[counters] = cap
        print(f"{exe.name}: driver ok, reader check rc=0")

    j = subprocess.run([a.reader, "counters", "--json", str(caps[True])], capture_output=True, text=True)
    (out / "counters.json").write_text(j.stdout + j.stderr)
    if j.returncode:
        sys.exit(f"FAIL: counters --json rc={j.returncode}")
    d = json.loads(j.stdout)
    names = [n["name"] for n in d["names"]]
    want_names = ["arena.bytes_used", "arena.fallbacks", "worker.jobs"] + [f"n{i:02d}" for i in range(61)]
    problems = []
    if not d["capability"] or not d["final"]:
        problems.append("capability/final")
    if names != want_names:
        problems.append(f"names {names[:5]}... ({len(names)})")
    if d["refused"] != 12 or d["dropped"] != 0:
        problems.append(f"refused={d['refused']} dropped={d['dropped']} (want 12, 0)")
    s = d["samples"]
    if len(s) != 70:
        problems.append(f"{len(s)} samples (want 70)")
    used = [(x["frame"], x["value"]) for x in s if x["name"] == "arena.bytes_used"]
    if used != [(0, 4096), (1, 8192), (2, 4096), (3, 8192)]:  # two zones(2) calls; marker = frames so far
        problems.append(f"arena.bytes_used per frame {used}")
    fb = [(x["frame"], x["value"]) for x in s if x["name"] == "arena.fallbacks"]
    if fb != [(0, 0), (1, -1), (2, 0), (3, -1)]:
        problems.append(f"arena.fallbacks per frame {fb}")
    wj = [x for x in s if x["name"] == "worker.jobs"]
    if len(wj) != 1 or wj[0]["frame"] != 0 or wj[0]["value"] != 7:
        problems.append(f"worker.jobs {wj}")
    tbs = [x["tb"] for x in s]
    if tbs != sorted(tbs):
        problems.append("sample timebases not in order")
    j0 = subprocess.run([a.reader, "counters", "--json", str(caps[False])], capture_output=True, text=True)
    if j0.returncode or json.loads(j0.stdout) != {"capability": False}:
        problems.append(f"default capture: {j0.stdout.strip()}")
    print("counters:", "PASS" if not problems else "FAIL " + "; ".join(problems))

    f0, d0, r0 = zones(caps[False].read_bytes())
    f1, d1, r1 = zones(caps[True].read_bytes())
    d4 = []
    if f0 != 0 or f1 != 256:
        d4.append(f"file_flags default={f0:#x} counters={f1:#x} (want 0 and 0x100)")
    if d0 != d1 or not {"wait.rsx_label", "drain.retired", "wait.spu_job"} <= set(d0):
        d4.append(f"zone names differ: {d0} vs {d1}")
    if r0 != r1 or not r0:
        d4.append("record kind/depth/zone sequences differ")
    (out / "d4.json").write_text(json.dumps({"default": {"flags": f0, "zones": d0, "records": r0},
                                             "counters": {"flags": f1, "zones": d1, "records": r1}}, indent=1))
    print("D4 wait.*/drain.* zones and records unchanged:", "PASS" if not d4 else "FAIL " + "; ".join(d4))
    ok = not problems and not d4 and lock_ok
    print("counters-test:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
