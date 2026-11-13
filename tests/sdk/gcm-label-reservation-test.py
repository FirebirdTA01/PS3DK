#!/usr/bin/env python3
"""Every RSX label / local report index our code uses must lie in the declared set of <cell/gcm/ps3tc_gcm_labels.h>.

The ranges come from the header itself (gcm-labels-test --dump, built by gcm-labels-test.sh), never re-typed here.
Scanned, with the owners each tree may use:

  sdk/                         SDK libraries and headers: PSGL- or PROFILER-owned indices only
  samples/profiling/           profiler samples: PROFILER-owned indices only (EXCEPTIONS below, each with a reason)
  APP_SAMPLES                  application samples for the rule: application (unreserved) indices only

Every call of a label or report API is found; its index argument is evaluated from the object-like #defines of its
own tree.  A constant index is checked against the policy.  A non-constant index is accepted only in a FORWARDER
file (an API layer that passes a caller's index through) or when SITES declares the values it can take (each value
is evaluated from the code's own macros and checked).  Anything else fails closed.  Three drift checks pin the code
that hard-codes the rule: libPSGL's label macros, the profiler GCM port's reserved-label test, and the automatic
profiler's default labels and report pool (PS3DK_PROF_AUTO_OPTIONS_INIT), which must be PROFILER-owned.

--self-test proves the scanner refuses: an SDK file using an application label, an unresolvable SDK index, a
profiler sample using a PSGL label, an application sample using a reserved label, a header without the PSGL
ranges and a header without the automatic profiler's report range (the real tree must then fail).
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYSTEM, PSGL, PROFILER = 1, 2, 3
OWNER_NAMES = {0: "APPLICATION", SYSTEM: "SYSTEM", PSGL: "PSGL", PROFILER: "PROFILER"}

# name -> (kind, index argument position by argument count)
LABEL_CALLS = {
    "cellGcmGetLabelAddress": {1: 0}, "gcmGetLabelAddress": {1: 0}, "cellGcmIsLabelPassed": {2: 0},
    **{n: {2: 0, 3: 1} for n in (
        "cellGcmSetWriteBackEndLabel", "cellGcmSetWriteTextureLabel", "cellGcmSetWaitLabel",
        "cellGcmSetWriteCommandLabel", "cellGcmSetWriteBackEndLabelUnsafe", "cellGcmSetWriteTextureLabelUnsafe",
        "cellGcmSetWaitLabelUnsafe", "cellGcmSetWriteCommandLabelUnsafe", "rsxSetWriteBackendLabel",
        "rsxSetWriteTextureLabel", "rsxSetWaitLabel", "rsxSetWriteCommandLabel")},
}
REPORT_CALLS = {
    "cellGcmSetTimeStamp": {1: 0, 2: 1}, "rsxSetTimeStamp": {2: 1}, "cellGcmGetTimeStamp": {1: 0},
    "gcmGetTimeStamp": {1: 0}, "cellGcmGetTimeStampLocation": {2: 0}, "cellGcmGetReportDataAddress": {1: 0},
    "gcmGetReportDataAddress": {1: 0}, "cellGcmGetReportDataAddressLocation": {2: 0}, "cellGcmGetReport": {2: 1},
    "cellGcmSetReport": {2: 1, 3: 2}, "rsxSetReport": {3: 2},
}
ATTACH = "ps3dk_prof_gpu_attach"  # (ctx, report_first, report_count, label, out)

FORWARDERS = {
    "sdk/include/cell/gcm.h", "sdk/include/cell/gcm/gcm_cmd_overloads.h", "sdk/include/cell/gcm/gcm_command_c.h",
    "sdk/include/cell/gcm/gcm_command_cpp_explicit.h", "sdk/include/cell/gcm/ps3tc_gcm_labels.h",
    "sdk/libgcm_sys_legacy/src/gcm_legacy_wrappers.c", "sdk/libgcm_cmd/src/gcm_rsx_wrappers.c",
    "sdk/libgcm_cmd/src/gcm_segment.c",  # the segment helper writes the label its caller chose
    "sdk/libps3dkprof/src/rsx/gpu_port_gcm.c",
}
# (path, call, normalised index expression regex) -> declared value specs: "A" or "A..B", evaluated from the tree
SITES = [
    ("sdk/libPSGL/src/psgl_context.c", "label", r"\(uint8_t\)\(PSGL_LABEL_BUFFER_STATUS\+[\w.>-]+\)",
     ["PSGL_LABEL_BUFFER_STATUS..PSGL_LABEL_BUFFER_STATUS+PSGL_MAX_FRAME_BUFFERS-1"]),
    # pe_fence's port: only ever called with PE_FENCE_LABEL (prof_ext.c, host-tested: "writes only label 242")
    ("samples/profiling/heterogeneous-workload/source/prof_ext_bind.c", "label", r"label", ["PE_FENCE_LABEL"]),
    # the ob_rsx port's bench reports lie below the domain (ob_gpu_run.h static assertion)
    ("samples/profiling/overhead-bench/source/rsx_bind.c", "report", r"i", ["OB_BENCH_REPORT..OB_BENCH_REPORT+1"]),
    ("samples/profiling/overhead-bench/source/rsx_bind.c", "report", r"index",
     ["OB_BENCH_REPORT..OB_GPU_REPORT_FIRST+OB_GPU_REPORT_COUNT-1"]),
    ("samples/profiling/overhead-bench/source/rsx_bind.c", "attach", r"first,count,label",
     ["OB_GPU_REPORT_FIRST,OB_GPU_REPORT_COUNT,OB_GPU_LABEL"]),
]
# profiler-sample indices outside the PROFILER block, each with its reason (never an SDK library)
EXCEPTIONS = {
    ("samples/profiling/ext-probe/source/main.c", "label", 64):
        "E1 probe predates the rule; label 64 is an application label, not SDK-reserved, and the probe links no PSGL",
}
APP_SAMPLES = ["samples/gcm/hello-ppu-gcm-label-reuse", "samples/gcm/hello-ppu-gcm-main-arena"]

DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+([^\n]*?)[ \t]*(?:/\*.*|//.*)?$", re.M)
CAST = re.compile(r"\(\s*(?:const\s+)?(?:u?int(?:8|16|32|64)_t|u8|u16|u32|u64|s32|unsigned(?:\s+int)?|int)\s*\)")
DECL_ARG = re.compile(r"^\s*(?:const\s+)?(?:u?int(?:8|16|32|64)_t|u8|u16|u32|u64|s32|unsigned|int|CellGcmContextData|"
                      r"gcmContextData|ps3dk_prof_gcm_ctx|uint8_t)\b")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def macros_of(files):
    env, clash = {}, set()
    for f in files:
        for name, value in DEFINE.findall(strip_comments(f.read_text(errors="replace"))):
            if name in env and env[name] != value:
                clash.add(name)
            env.setdefault(name, value)
    for name in clash:
        env.pop(name, None)
    return env


def evaluate(expr, env, depth=0):
    """The integer value of a C index expression, or None when it is not a compile-time constant here."""
    s = CAST.sub("", expr)
    for _ in range(12):
        new = re.sub(r"[A-Za-z_]\w*", lambda m: "(" + env[m.group(0)] + ")" if m.group(0) in env else m.group(0), s)
        new = CAST.sub("", new)
        if new == s:
            break
        s = new
    s = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", s)
    if not re.fullmatch(r"[\s0-9a-fA-FxX()+\-*/<>|&~]*", s) or not s.strip():
        return None
    try:
        v = eval(s.replace("/", "//"), {"__builtins__": {}}, {})  # digits and operators only (checked above)
    except Exception:
        return None
    return v if isinstance(v, int) else None


def split_args(text, start):
    """Arguments of the call whose '(' is at text[start]; None if unbalanced."""
    depth, args, cur, i = 0, [], [], start
    while i < len(text):
        c = text[i]
        if c == "(":
            depth += 1
            if depth > 1:
                cur.append(c)
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(cur).strip())
                return args
            cur.append(c)
        elif c == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    return None


CALL = re.compile(r"\b(" + "|".join(sorted(set(LABEL_CALLS) | set(REPORT_CALLS) | {ATTACH}, key=len, reverse=True))
                  + r")\s*\(")


def call_sites(path):
    text = strip_comments(path.read_text(errors="replace"))
    for m in CALL.finditer(text):
        args = split_args(text, m.end() - 1)
        if args is None:
            continue
        if args and any(DECL_ARG.match(a) and re.search(r"\w\s*$", a) and " " in a.strip() for a in args):
            continue  # a definition or a prototype, not a call
        line = text.count("\n", 0, m.start()) + 1
        yield m.group(1), args, line


def site_values(rel, kind, expr, env):
    norm = re.sub(r"\s+", "", expr)
    for path, k, rx, specs in SITES:
        if path == rel and k == kind and re.fullmatch(rx, norm):
            out = []
            for spec in specs:
                if kind == "attach":
                    first, count, label = (evaluate(x, env) for x in spec.split(","))
                    if None in (first, count, label) or count < 1:
                        return None
                    out.append((first, count, label))
                    continue
                lo, _, hi = spec.partition("..")
                a, b = evaluate(lo, env), evaluate(hi or lo, env)
                if a is None or b is None or a > b:
                    return None
                out.extend(range(a, b + 1))
            return out
    return None


def owner(ranges, kind, index):
    for r in ranges[kind]:
        if r["first"] <= index <= r["last"]:
            return r["owner"]
    return 0


def scan(root, ranges, extra=()):
    """Returns (findings, problems).  extra: (rel path, policy, text) files added for the self-test."""
    limits = ranges["limits"]
    units = []
    for lib in sorted((root / "sdk").iterdir()):
        if lib.is_dir():
            units.append(("sdk", lib, [p for p in lib.rglob("*") if p.suffix in (".c", ".h", ".cc", ".cpp")]))
    for sample in sorted((root / "samples/profiling").iterdir()):
        if sample.is_dir():
            units.append(("profiler", sample, [p for p in sample.rglob("*") if p.suffix in (".c", ".h", ".cc", ".cpp")
                                               and "host-tests" not in p.parts]))
    for rel in APP_SAMPLES:
        d = root / rel
        if not d.is_dir():
            units.append(("missing", d, []))
            continue
        units.append(("app", d, [p for p in d.rglob("*") if p.suffix in (".c", ".h")]))
    for rel, policy, text in extra:
        p = root / ".gcm-label-selftest" / rel
        units.append((policy, p.parent, [(p, text)]))

    allowed = {"sdk": {PSGL, PROFILER}, "profiler": {PROFILER}, "app": {0}}
    findings, problems = [], []
    for policy, unit, files in units:
        if policy == "missing":
            problems.append(f"{unit.relative_to(root)}: application sample for the rule is missing")
            continue
        real = [f for f in files if isinstance(f, Path)]
        env = macros_of(real)
        for f in files:
            if isinstance(f, tuple):
                path, text = f
                tmp_text = text
            else:
                path, tmp_text = f, None
            rel = str(path.relative_to(root)).replace("\\", "/")
            if rel.endswith("cell/gcm/ps3tc_gcm_labels.h"):
                continue
            if tmp_text is not None:
                fake = root / ".gcm-label-selftest.tmp"
                fake.write_text(tmp_text)
                sites = list(call_sites(fake))
                fake.unlink()
                local_env = dict(env, **dict(DEFINE.findall(strip_comments(tmp_text))))
            else:
                sites = list(call_sites(path))
                local_env = env
            for name, args, line in sites:
                where = f"{rel}:{line} {name}"
                if name == ATTACH:
                    if len(args) != 5:
                        problems.append(f"{where}: unexpected argument count {len(args)}")
                        continue
                    first, count, label = (evaluate(a, local_env) for a in args[1:4])
                    if None in (first, count, label):
                        declared = site_values(rel, "attach", ",".join(args[1:4]), local_env)
                        if declared is None:
                            if rel in FORWARDERS:
                                continue
                            problems.append(f"{where}: non-constant attach ({', '.join(args[1:4])}) not declared in SITES")
                            continue
                        triples = declared
                    else:
                        triples = [(first, count, label)]
                    for first, count, label in triples:
                        checks = [("label", label)] + [("report", i) for i in range(first, first + count)]
                        for kind, index in checks:
                            findings.append(check(rel, where, policy, kind, index, ranges, limits, allowed, problems))
                    continue
                kind = "label" if name in LABEL_CALLS else "report"
                pos = (LABEL_CALLS if kind == "label" else REPORT_CALLS)[name].get(len(args))
                if pos is None:
                    problems.append(f"{where}: unexpected argument count {len(args)}")
                    continue
                expr = args[pos]
                value = evaluate(expr, local_env)
                if value is None:
                    declared = site_values(rel, kind, expr, local_env)
                    if declared is None:
                        if rel in FORWARDERS:
                            continue
                        problems.append(f"{where}: {kind} index '{expr}' is not a constant and is not declared in SITES")
                        continue
                    values = declared
                else:
                    values = [value]
                for index in values:
                    findings.append(check(rel, where, policy, kind, index, ranges, limits, allowed, problems))
    return [f for f in findings if f], problems


def check(rel, where, policy, kind, index, ranges, limits, allowed, problems):
    limit = limits["labels" if kind == "label" else "reports"]
    if not 0 <= index < limit:
        problems.append(f"{where}: {kind} {index} out of range 0..{limit - 1}")
        return None
    o = owner(ranges, kind, index)
    if o in allowed[policy]:
        return (where, kind, index, OWNER_NAMES[o], "ok")
    reason = EXCEPTIONS.get((rel, kind, index)) if policy == "profiler" and o not in (SYSTEM, PSGL) else None
    if reason:
        return (where, kind, index, OWNER_NAMES[o], "exception: " + reason)
    problems.append(f"{where}: {kind} {index} is {OWNER_NAMES[o]}-owned; the {policy} policy allows "
                    + "/".join(OWNER_NAMES[a] for a in sorted(allowed[policy])))
    return None


def drift(root, ranges):
    """Code that hard-codes the rule must agree with the header."""
    problems = []
    psgl_labels = {i for r in ranges["label"] if r["owner"] == PSGL for i in range(r["first"], r["last"] + 1)}
    env = macros_of([root / "sdk/libPSGL/src/psgl_context.c", root / "sdk/libPSGL/src/psgl_context.h"])
    idle, prep, status, frames = (evaluate(n, env) for n in (
        "PSGL_LABEL_INDEX", "PSGL_LABEL_PREPARED_BUFFER", "PSGL_LABEL_BUFFER_STATUS", "PSGL_MAX_FRAME_BUFFERS"))
    if None in (idle, prep, status, frames):
        problems.append("libPSGL: a label macro could not be evaluated")
    else:
        used = {idle, prep} | set(range(status, status + frames))
        if used != psgl_labels:
            problems.append(f"libPSGL uses labels {sorted(used)}, the header reserves {sorted(psgl_labels)} for PSGL")
    port = strip_comments((root / "sdk/libps3dkprof/src/rsx/gpu_port_gcm.c").read_text())
    m = re.search(r"static int gpp_label_reserved\(uint32_t l\)\s*\{\s*return ([^;]+);", port)
    first = evaluate("GPP_LABEL_USER_FIRST", macros_of([root / "sdk/libps3dkprof/src/rsx/gpu_port_gcm.h"]))
    if not m or first is None:
        problems.append("profiler GCM port: gpp_label_reserved or GPP_LABEL_USER_FIRST not found")
    else:
        expr = re.sub(r"(\d+)u", r"\1", m.group(1)).replace("||", " or ").replace("&&", " and ")
        refused = {i for i in range(256) if eval(expr, {"__builtins__": {}}, {"l": i})}
        if refused != psgl_labels:
            problems.append(f"profiler port refuses labels {sorted(refused)}, the header's SDK set is {sorted(psgl_labels)}")
        system_last = max(r["last"] for r in ranges["label"] if r["owner"] == SYSTEM)
        if first != system_last + 1:
            problems.append(f"profiler port's first lendable label {first} is not the first after SYSTEM ({system_last + 1})")
    # The automatic profiler's defaults (size, report_first, report_count, operation_label, clock_label, ...).
    text = strip_comments((root / "sdk/include/ps3dk/prof_auto.h").read_text())
    m = re.search(r"#\s*define\s+PS3DK_PROF_AUTO_OPTIONS_INIT\s*\{([^}]*)\}", text)
    if not m:
        problems.append("prof_auto.h: PS3DK_PROF_AUTO_OPTIONS_INIT not found")
    else:
        vals = [int(v.strip().rstrip("uU")) for v in m.group(1).split(",")]
        first, count, op, clock = vals[1:5]
        for name, label in (("operation", op), ("clock", clock)):
            if owner(ranges, "label", label) != PROFILER:
                problems.append(f"prof_auto.h: default {name} label {label} is not PROFILER-owned")
        bad = [i for i in range(first, first + count) if owner(ranges, "report", i) != PROFILER]
        if bad:
            problems.append(f"prof_auto.h: default report pool {first}..{first + count - 1} includes "
                            f"{len(bad)} non-PROFILER reports (first {bad[0]})")
    return problems


def load_ranges(path):
    out = {"label": [], "report": [], "limits": None}
    for line in Path(path).read_text().splitlines():
        row = json.loads(line)
        if row["kind"] == "limits":
            out["limits"] = row
        else:
            out[row["kind"]].append(row)
    if not out["label"] or not out["limits"]:
        raise SystemExit("FAIL: the range dump is incomplete")
    return out


def run(root, ranges, extra=(), quiet=False):
    findings, problems = scan(root, ranges, extra)
    problems += drift(root, ranges)
    if not quiet:
        for where, kind, index, own, verdict in findings:
            print(f"  {where}: {kind} {index} [{own}] {verdict}")
    return findings, problems


def self_test(root, ranges):
    cases = [
        ("an SDK library using application label 100",
         [("sdk/libfake/src/x.c", "sdk", "void f(void*c){cellGcmSetWriteBackEndLabel(c, 100, 1);}\n")], ranges),
        ("an unresolvable SDK index",
         [("sdk/libfake/src/x.c", "sdk", "void f(void*c,int k){cellGcmSetWaitLabel(c, k + 3, 1);}\n")], ranges),
        ("a profiler sample using PSGL label 65",
         [("samples/profiling/fake/main.c", "profiler", "#define L 65u\nvoid f(void){cellGcmGetLabelAddress(L);}\n")],
         ranges),
        ("a profiler sample lending SYSTEM label 10 to the GPU domain",
         [("samples/profiling/fake/main.c", "profiler", "void f(void*c,unsigned*g){ps3dk_prof_gpu_attach(c,32,2,10,g);}\n")],
         ranges),
        ("an application sample using profiler report 7",
         [("samples/gcm/fake/main.c", "app", "void f(void*c){cellGcmSetTimeStamp(c, 7);}\n")], ranges),
        ("a header without the PSGL ranges", [],
         dict(ranges, label=[r for r in ranges["label"] if r["owner"] != PSGL])),
        ("a header without the automatic profiler's report range", [],
         dict(ranges, report=[r for r in ranges["report"] if r["first"] != 1024])),
    ]
    ok = True
    _, baseline = run(root, ranges, quiet=True)
    for name, extra, rng in cases:
        _, problems = run(root, rng, extra, quiet=True)
        # the refusal must be a NEW problem, and an injected case's must name the injected file
        new = [p for p in problems if p not in baseline and (not extra or extra[0][0] in p)]
        good = bool(new)
        ok &= good
        print(f"self-test refuses {name}: {'ok' if good else 'FAIL'}" + (f" ({new[0]})" if new else ""))
    problems = baseline
    good = not problems
    ok &= good
    print(f"self-test accepts the real tree: {'ok' if good else 'FAIL'}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ranges", required=True, help="JSON lines from gcm-labels-test --dump")
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    ranges = load_ranges(a.ranges)
    if a.self_test:
        ok = self_test(a.root, ranges)
        print("gcm-label-reservation self-test:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    findings, problems = run(a.root, ranges)
    for p in problems:
        print("FAIL:", p)
    print(f"gcm-label-reservation: {len(findings)} checked indices, {len(problems)} problem(s):",
          "PASS" if not problems else "FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
