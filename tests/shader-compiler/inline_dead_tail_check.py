#!/usr/bin/env python3
"""A returned helper path must not execute or reject its unreachable suffix.

Each accepted source has an independently reduced control with the dead suffix
removed. Complete containers must match in FP and, for non-runtime-return cases,
VP. FP programs additionally run through the established instruction evaluator
on inputs that exercise both sides of runtime conditions. No reference SDK is
required by this regression test.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval


# name, source, dead-tail-free control, expected FP result, supports VP.
# The controls retain file-scope declarations so reflection differences cannot
# masquerade as code differences.
CASES = [
    ("consecutive_returns",
     "float4 f(float4 c) { return c * 2.0; return c * 7.0; }",
     "float4 f(float4 c) { return c * 2.0; }",
     lambda c: [v * 2.0 for v in c], True),
    ("dead_global_write",
     """static float4 G = 0;
float4 f(float4 c) { G = c * 3.0; return c * 2.0; G = c * 9.0; return c * 7.0; }
float4 invoke(float4 c) { float4 r = f(c); return r + G; }""",
     """static float4 G = 0;
float4 f(float4 c) { G = c * 3.0; return c * 2.0; }
float4 invoke(float4 c) { float4 r = f(c); return r + G; }""",
     lambda c: [v * 5.0 for v in c], True),
    ("constant_taken_return",
     """static const int mode = 2;
float4 f(float4 c) {
    if (mode == 1) return c * 3.0;
    if (mode == 2) { float4 r = c * 2.0; return r; }
    return c * 7.0;
}""",
     """static const int mode = 2;
float4 f(float4 c) { float4 r = c * 2.0; return r; }""",
     lambda c: [v * 2.0 for v in c], True),
    ("constant_untaken_keeps_tail",
     "float4 f(float4 c) { if (false) return c * 9.0; c *= 2.0; return c; c *= 7.0; }",
     "float4 f(float4 c) { c *= 2.0; return c; }",
     lambda c: [v * 2.0 for v in c], True),
    ("nested_constant_blocks",
     """float4 f(float4 c) {
    if (true) {
        if (true) { return c * 2.0; return c * 9.0; }
        return c * 7.0;
    }
    return c * 5.0;
}""",
     "float4 f(float4 c) { return c * 2.0; }",
     lambda c: [v * 2.0 for v in c], True),
    ("void_out_copyback",
     """void setValue(float4 c, out float4 o) { o = c * 2.0; return; o = c * 9.0; }
float4 f(float4 c) { float4 r; setValue(c, r); return r; }""",
     """void setValue(float4 c, out float4 o) { o = c * 2.0; return; }
float4 f(float4 c) { float4 r; setValue(c, r); return r; }""",
     lambda c: [v * 2.0 for v in c], True),
    ("unreached_declaration_does_not_own_global",
     """static float4 G = 0;
float4 f(float4 c) { G = c * 3.0; return c * 2.0; float4 G = c * 9.0; }
float4 invoke(float4 c) { float4 r = f(c); return r + G; }""",
     """static float4 G = 0;
float4 f(float4 c) { G = c * 3.0; return c * 2.0; }
float4 invoke(float4 c) { float4 r = f(c); return r + G; }""",
     lambda c: [v * 5.0 for v in c], True),
    ("runtime_arm_dead_tail",
     "float4 f(float4 c) { if (c.x > 0.0) { return c * 2.0; c *= 9.0; } return c * 3.0; }",
     "float4 f(float4 c) { if (c.x > 0.0) { return c * 2.0; } return c * 3.0; }",
     lambda c: [v * (2.0 if c[0] > 0.0 else 3.0) for v in c], False),
    ("runtime_fallthrough_still_runs",
     "float4 f(float4 c) { if (c.x > 0.0) return c * 2.0; c *= 3.0; return c; c *= 9.0; }",
     "float4 f(float4 c) { if (c.x > 0.0) return c * 2.0; c *= 3.0; return c; }",
     lambda c: [v * (2.0 if c[0] > 0.0 else 3.0) for v in c], False),
    ("nested_helper_keeps_own_continuation",
     """float4 g(float4 c) { return c * 2.0; return c * 9.0; }
float4 f(float4 c) { if (c.x > 0.0) return g(c); return c * 3.0; }""",
     """float4 g(float4 c) { return c * 2.0; }
float4 f(float4 c) { if (c.x > 0.0) return g(c); return c * 3.0; }""",
     lambda c: [v * (2.0 if c[0] > 0.0 else 3.0) for v in c], False),
]

REFUSALS = [
    ("missing_runtime_return", "fp",
     "float4 f(float4 c) { if (c.x > 0.0) return c * 2.0; c *= 3.0; }",
     "a path through it has no return expression"),
    ("vp_runtime_return_stays_guarded", "vp",
     "float4 f(float4 c) { if (c.x > 0.0) return c * 2.0; return c * 3.0; }",
     "a return inside control flow"),
    ("bare_nested_block_stays_guarded", "fp",
     "float4 f(float4 c) { { return c * 2.0; } return c * 3.0; }",
     "a return inside control flow"),
]
INPUTS = [
    [-1.0, 0.5, 0.25, 1.0],
    [0.0, -0.25, 0.75, 0.5],
    [0.25, 0.5, -1.0, 0.125],
]


def source_text(body, stage):
    semantic_in, semantic_out = ("TEXCOORD0", "COLOR") if stage == "fp" else ("POSITION", "POSITION")
    entry = "invoke" if "float4 invoke(" in body else "f"
    return body + "\nfloat4 main(float4 t : %s) : %s { return %s(t); }\n" % (
        semantic_in, semantic_out, entry)


def compile_one(compiler, work, name, stage, body):
    source = work / (name + "." + stage + ".cg")
    output = work / (name + "." + stage + ".bin")
    output.unlink(missing_ok=True)
    source.write_text(source_text(body, stage), encoding="utf-8")
    run = subprocess.run(
        [compiler, "-p", "sce_" + stage + "_rsx", "--emit-container", str(output), str(source)],
        capture_output=True, text=True, timeout=30)
    log = run.stdout + run.stderr
    (work / (name + "." + stage + ".log")).write_text(log, encoding="utf-8")
    return run.returncode, output.read_bytes() if output.exists() else b"", log


def run_checks(compiler, work):
    failures = []
    if not fp_eval.self_test():
        raise RuntimeError("FP evaluator self-test failed")
    for name, body, control, expected, vertex in CASES:
        for stage in (("fp", "vp") if vertex else ("fp",)):
            try:
                rc, blob, log = compile_one(compiler, work, name, stage, body)
                crc, cblob, clog = compile_one(compiler, work, name + "_control", stage, control)
            except (OSError, subprocess.TimeoutExpired) as exc:
                failures.append("%s/%s: compiler execution failed: %s" % (name, stage, exc))
                continue
            if crc != 0 or not cblob:
                failures.append("%s/%s: control refused rc=%d: %s" % (name, stage, crc, clog.strip()))
                continue
            if rc != 0 or not blob:
                failures.append("%s/%s: dead-tail shader refused rc=%d: %s" % (name, stage, rc, log.strip()))
                continue
            if blob != cblob:
                failures.append("%s/%s: dead suffix changed container bytes" % (name, stage))
            if stage == "fp":
                for inputs in INPUTS:
                    want = expected(inputs)
                    for label, program in (("source", blob), ("control", cblob)):
                        actual = fp_eval.evaluate(program, {"TEX0": inputs})
                        if actual != want:
                            failures.append("%s/%s %s: input %s returned %s, want %s" % (
                                name, stage, label, inputs, actual, want))
            print("checked %s/%s" % (name, stage))
    for name, stage, body, diagnostic in REFUSALS:
        try:
            rc, blob, log = compile_one(compiler, work, name, stage, body)
        except (OSError, subprocess.TimeoutExpired) as exc:
            failures.append("%s: compiler execution failed: %s" % (name, exc))
            continue
        if rc != 1 or blob or diagnostic not in log:
            failures.append("%s: expected named refusal, got rc=%d bytes=%d: %s" % (
                name, rc, len(blob), log.strip()))
    for failure in failures:
        print("FAIL: " + failure)
    print("inline-dead-tail: " + ("FAIL (%d)" % len(failures) if failures else "PASS"))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler")
    parser.add_argument("--work", type=Path, help="retain shader sources, containers and logs here")
    args = parser.parse_args()
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        return run_checks(args.compiler, args.work)
    with tempfile.TemporaryDirectory(prefix="inline-dead-tail-") as directory:
        return run_checks(args.compiler, Path(directory))


if __name__ == "__main__":
    raise SystemExit(main())
