#!/usr/bin/env python3
"""A returned helper path must not execute or reject its unreachable suffix.

Each accepted source has an independently reduced control with the dead suffix
removed. Complete containers must match in FP and VP.
Both stages additionally run through the established instruction evaluators
on inputs that exercise both sides of runtime conditions. No reference SDK is
required by this regression test.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp_eval


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
     lambda c: [v * (2.0 if c[0] > 0.0 else 3.0) for v in c], True),
    ("runtime_fallthrough_still_runs",
     "float4 f(float4 c) { if (c.x > 0.0) return c * 2.0; c *= 3.0; return c; c *= 9.0; }",
     "float4 f(float4 c) { if (c.x > 0.0) return c * 2.0; c *= 3.0; return c; }",
     lambda c: [v * (2.0 if c[0] > 0.0 else 3.0) for v in c], True),
    ("nested_helper_keeps_own_continuation",
     """float4 g(float4 c) { return c * 2.0; return c * 9.0; }
float4 f(float4 c) { if (c.x > 0.0) return g(c); return c * 3.0; }""",
     """float4 g(float4 c) { return c * 2.0; }
float4 f(float4 c) { if (c.x > 0.0) return g(c); return c * 3.0; }""",
     lambda c: [v * (2.0 if c[0] > 0.0 else 3.0) for v in c], True),
    ("scalar_both_arms",
     "float minimum(float2 v) { if(v.x>v.y) return v.y; else return v.x; return 7; } float4 f(float4 c) { return float4(minimum(c.xy),minimum(c.zw),c.zw); }",
     "float minimum(float2 v) { if(v.x>v.y) return v.y; else return v.x; } float4 f(float4 c) { return float4(minimum(c.xy),minimum(c.zw),c.zw); }",
     lambda c: [min(c[0],c[1]),min(c[2],c[3]),c[2],c[3]], True),
    ("conditional_out_copyback",
     "void h(float4 c,out float4 o) { if(c.x>0) { o=c*2; return; o=c*9; } o=c*3; } float4 f(float4 c) { float4 r; h(c,r); return r; }",
     "void h(float4 c,out float4 o) { if(c.x>0) { o=c*2; return; } o=c*3; } float4 f(float4 c) { float4 r; h(c,r); return r; }",
     lambda c: [v*(2.0 if c[0]>0 else 3.0) for v in c], True),
    ("conditional_global_write",
     "static float4 G=0; float4 f(float4 c) { if(c.x>0) { G=c*2; return c; G=c*9; } G=c*3; return c; } float4 invoke(float4 c) { float4 r=f(c); return r+G; }",
     "static float4 G=0; float4 f(float4 c) { if(c.x>0) { G=c*2; return c; } G=c*3; return c; } float4 invoke(float4 c) { float4 r=f(c); return r+G; }",
     lambda c: [v*(3.0 if c[0]>0 else 4.0) for v in c], True),
    ("nested_runtime_branches",
     "float4 f(float4 c) { if(c.x>0) { if(c.y>0) return c*2; return c*3; c*=9; } return c*4; }",
     "float4 f(float4 c) { if(c.x>0) { if(c.y>0) return c*2; return c*3; } return c*4; }",
     lambda c: [v*(2.0 if c[1]>0 else 3.0) if c[0]>0 else v*4.0 for v in c], True),
    ("large_finite_return",
     "float4 f(float4 c) { if(c.x>0) return float4(1,2,3,4); return float4(1073741824,1073741824,1073741824,1073741824); c*=9; }",
     "float4 f(float4 c) { if(c.x>0) return float4(1,2,3,4); return float4(1073741824,1073741824,1073741824,1073741824); }",
     lambda c: [1.0,2.0,3.0,4.0] if c[0]>0 else [1073741824.0]*4, True),
    ("zero_then_vector_return",
     "float4 f(float4 c) { if(c.x<=0) return 0; return c.wzyx; return c; }",
     "float4 f(float4 c) { if(c.x<=0) return 0; return c.wzyx; }",
     lambda c: [0.0]*4 if c[0]<=0 else list(reversed(c)), True),
    ("zero_else_vector_return",
     "float4 f(float4 c) { if(c.x>0) return c.wzyx; return 0; return c; }",
     "float4 f(float4 c) { if(c.x>0) return c.wzyx; return 0; }",
     lambda c: list(reversed(c)) if c[0]>0 else [0.0]*4, True),
    ("scalar_lane_vector_return",
     "float4 f(float4 c) { if(c.x>0) return c.y; return c.wzyx; return c; }",
     "float4 f(float4 c) { if(c.x>0) return c.y; return c.wzyx; }",
     lambda c: [c[1]]*4 if c[0]>0 else list(reversed(c)), True),
    ("zero_then_scalar_lane_return",
     "float pick(float4 c) { if(c.y<=0) return 0; return c.w; return c.x; } float4 f(float4 c) { return float4(pick(c),c.xyz); }",
     "float pick(float4 c) { if(c.y<=0) return 0; return c.w; } float4 f(float4 c) { return float4(pick(c),c.xyz); }",
     lambda c: [0.0 if c[1]<=0 else c[3],*c[:3]], True),
    ("scalar_return_evaluated_once",
     "static float G; float4 h(float4 c) { if(c.x>0) return G++; return c.wzyx; return c; } float4 f(float4 c) { G=c.y; float4 r=h(c); return r+G; }",
     "static float G; float4 h(float4 c) { if(c.x>0) return G++; return c.wzyx; } float4 f(float4 c) { G=c.y; float4 r=h(c); return r+G; }",
     lambda c: [2*c[1]+1]*4 if c[0]>0 else [v+c[1] for v in reversed(c)], True),
    ("partial_output_postincrement",
     "float4 f(float4 c) { float x=c.x; float4 r=c; if(x++) r*=2; r.y=x; return r; r*=9; }",
     "float4 f(float4 c) { float x=c.x; float4 r=c; if(x++) r*=2; r.y=x; return r; }",
     lambda c: [c[0]*(2 if c[0]!=0 else 1),c[0]+1,c[2]*(2 if c[0]!=0 else 1),c[3]*(2 if c[0]!=0 else 1)], True),
]

REFUSALS = [
    ("missing_runtime_return", "fp",
     "float4 f(float4 c) { if (c.x > 0.0) return c * 2.0; c *= 3.0; }",
     "a path through it has no return expression"),
    ("vp_missing_runtime_return", "vp",
     "float4 f(float4 c) { if(c.x>0) return c*2; c*=3; }",
     "a path through it has no return expression"),
    ("vp_loop_return", "vp",
     "float4 f(float4 c) { for(int i=0;i<2;i++) { if(c.x>i) return c; } return c*2; }",
     "a return inside control flow"),
    ("bare_nested_block_stays_guarded", "fp",
     "float4 f(float4 c) { { return c * 2.0; } return c * 3.0; }",
     "a return inside control flow"),
]
INPUTS = [
    [-1.0, 0.5, 0.25, 1.0],
    [0.0, -0.25, 0.75, 0.5],
    [0.25, 0.5, -1.0, 0.125],
    [0.25, -0.5, 0.125, -1.0],
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
    vp_eval.predication_selftest()
    # Output assignment can route the conditional write directly to COLOR,
    # while a later masked write updates only y. Check both output registers
    # and the postincrement, not merely acceptance of an output destination.
    source = work / 'direct_output_postincrement.cg'
    output = source.with_suffix('.bin')
    output.unlink(missing_ok=True)
    source.write_text('''void main(float4 p:POSITION, float4 t:TEXCOORD0,
        out float4 pos:POSITION, out float4 colour:COLOR) {
        float x=t.x; float4 c=t; if(x++) c*=2; c.y=x;
        pos=p; colour=c;
    }''')
    result = subprocess.run([compiler, '-p', 'sce_vp_rsx', '--emit-container',
                             str(output), str(source)], capture_output=True, text=True, timeout=30)
    (work / 'direct_output_postincrement.log').write_text(result.stdout + result.stderr)
    if result.returncode != 0 or not output.exists():
        failures.append('direct_output_postincrement: refused: '+result.stderr)
    else:
        position = [.125, -.25, .5, 1.]
        for t in INPUTS:
            want = [v*(2 if t[0] != 0 else 1) for v in t]
            want[1] = t[0]+1
            got = vp_eval.evaluate(output.read_bytes(), {}, inputs={0: position, 8: t},
                                   binary32=True, predication=True)
            if got.get(0) != position or got.get(1) != want:
                failures.append('direct_output_postincrement: input %s got %s, want position=%s colour=%s' % (
                    t, got, position, want))
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
            if stage in ("fp", "vp"):
                for inputs in INPUTS:
                    want = expected(inputs)
                    for label, program in (("source", blob), ("control", cblob)):
                        actual = (fp_eval.evaluate(program, {"TEX0": inputs}) if stage == "fp" else
                                  vp_eval.evaluate(program, {}, inputs={0: inputs}, binary32=True, predication=True).get(0))
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
