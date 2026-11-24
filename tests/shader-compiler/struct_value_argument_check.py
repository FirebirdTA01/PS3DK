"""A struct passed BY VALUE to a helper is the argument's value (struct-value-argument).

A struct has no whole value in the IR, only `name.member` bindings, and the
inliner did not rename the argument's bindings to the parameter's name.
Inside the helper `q.a` then meant one of two wrong things:

  * the caller's own variable `q`, when the caller had one (name capture);
  * a fresh entry load named after the parameter, which dropped every write
    the caller had made to the argument - or, for a struct type without
    semantics, refused the program.

Measured on the established compiler: `l.a = i.b * 2; l.b = i.a * 3; f(l)`
with `f` returning `q.a + q.b` is 3*TEX0 + 2*TEX1; the unfixed compiler
emitted TEX0 + TEX1 with exit 0.

Every row is judged by VALUE with the evaluators (inputs are small dyadic
numbers, so each expected value is exact in binary32).  Three rows must stay
refused: a member the argument never assigned has no input behind it, and
a call result passed straight in is a gap, not a silent read.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp_eval

SEM = 'struct V { float3 a : TEXCOORD0; float3 b : TEXCOORD1; };\nfloat3 f(V q) { return q.a + q.b; }\n'
PLAIN = 'struct V { float3 a; float3 b; };\nfloat3 f(V q) { return q.a + q.b; }\n'
PLAN = ('struct S { float2 uvA; float2 uvB; float w; };\n'
        'S make(float2 uv) { S p; p.uvA = uv * 2; p.uvB = uv * 3; p.w = 0.25; return p; }\n'
        'float3 inner(S p) { return float3(p.uvA + p.uvB, p.w); }\n'
        'float3 blend(float k, S atPlan) { return inner(atPlan) * k; }\n')
SAME = PLAN.replace('inner(S p)', 'inner(S atPlan)').replace('p.uvA + p.uvB, p.w', 'atPlan.uvA + atPlan.uvB, atPlan.w')
MAIN2 = 'float4 main(float3 n : TEXCOORD0, float3 m : TEXCOORD1) : COLOR {\n'
UV2 = 'float4 main(float2 uv : TEXCOORD0, float2 uv2 : TEXCOORD1) : COLOR {\n'


def v3(fn):
    return lambda a, b, c: [fn(a[i], b[i], c[i]) for i in range(3)] + [1.0]


def plan(uv):
    return [5 * uv[0], 5 * uv[1], 0.25]


# name -> (source, expected(TEX0, TEX1, TEX2) -> COLOR0)
FP_ROWS = {
    # writes to a local struct reach the helper
    'local_writes': (SEM + 'float4 main(V i) : COLOR {\n V l; l.a = i.b * 2; l.b = i.a * 3;\n'
                     ' return float4(f(l), 1);\n}\n', v3(lambda a, b, c: 2 * b + 3 * a)),
    # a write to the entry struct itself reaches the helper; the unwritten
    # member is still the entry input
    'entry_modified': (SEM + 'float4 main(V i) : COLOR {\n i.a = i.b * 2;\n'
                       ' return float4(f(i), 1);\n}\n', v3(lambda a, b, c: 3 * b)),
    'copy_modified': (SEM + 'float4 main(V i) : COLOR {\n V l = i; l.a = i.b * 2;\n'
                      ' return float4(f(l), 1);\n}\n', v3(lambda a, b, c: 3 * b)),
    # a local copied whole from the entry struct still reads the entry's
    # members, and carries a write made before the copy
    'copy_plain': (SEM + 'float4 main(V i) : COLOR {\n V l = i;\n'
                   ' return float4(f(l), 1);\n}\n', v3(lambda a, b, c: a + b)),
    'modified_then_copied': (SEM + 'float4 main(V i) : COLOR {\n i.a = i.b * 2; V l = i;\n'
                             ' return float4(f(l), 1);\n}\n', v3(lambda a, b, c: 3 * b)),
    # the caller's own `q` is not the helper's parameter `q`
    'name_capture': (SEM + 'float4 main(V i, float3 c : TEXCOORD2) : COLOR {\n'
                     ' V q; q.a = c * 5; q.b = c * 7;\n float3 r = f(i);\n'
                     ' return float4(r + q.a, 1);\n}\n', v3(lambda a, b, c: a + b + 5 * c)),
    # a struct type without semantics: these were refusals
    'plain_assigned': (PLAIN + MAIN2 + ' V lv; lv.a = n * 2; lv.b = m * 3;\n'
                       ' return float4(f(lv), 1);\n}\n', v3(lambda a, b, c: 2 * a + 3 * b)),
    'plain_braced': (PLAIN + MAIN2 + ' V lv = { n * 2, m * 3 };\n'
                     ' return float4(f(lv), 1);\n}\n', v3(lambda a, b, c: 2 * a + 3 * b)),
    'nested_braced': ('struct V { float3 a; float3 b; };\nstruct P { float3 c; float k; };\n'
                      'struct I { V v; P p; };\n'
                      'float3 g(I input) { return input.v.a * input.p.c + input.v.b * input.p.k; }\n'
                      + MAIN2 + ' V lv; lv.a = n; lv.b = m;\n P lp = { m, 2 };\n I li = { lv, lp };\n'
                      ' return float4(g(li), 1);\n}\n', v3(lambda a, b, c: a * b + b * 2)),
    'member_argument': ('struct V { float3 a; float3 b; };\nstruct I { V v; float k; };\n'
                        'float3 f(V q) { return q.a + q.b; }\n'
                        + MAIN2 + ' I li; li.v.a = n * 2; li.v.b = m * 3; li.k = 1;\n'
                        ' return float4(f(li.v), 1);\n}\n', v3(lambda a, b, c: 2 * a + 3 * b)),
    # a member that is not a float or int kind travels with the argument:
    # unread (these two compiled before the fix and must keep compiling) ...
    'spare_bool': ('struct W { float3 a : TEXCOORD0; bool spare; };\nfloat3 f(W q) { return q.a; }\n'
                   'float4 main(float3 p : TEXCOORD0) : COLOR {\n W l; l.a = p; l.spare = true;\n'
                   ' return float4(f(l), 1);\n}\n', v3(lambda a, b, c: a)),
    'spare_unsigned': ('struct W { float3 a : TEXCOORD0; unsigned int spare; };\nfloat3 f(W q) { return q.a; }\n'
                       'float4 main(float3 p : TEXCOORD0) : COLOR {\n W l; l.a = p; l.spare = 1;\n'
                       ' return float4(f(l), 1);\n}\n', v3(lambda a, b, c: a)),
    # ... and read
    'bool_member_read': ('struct W { float3 a; bool on; };\nfloat3 f(W q) { return q.on ? q.a : q.a * 2; }\n'
                         + MAIN2 + ' W l; l.a = n; l.on = m.x > 1.25;\n return float4(f(l), 1);\n}\n',
                         lambda a, b, c: [x if b[0] > 1.25 else 2 * x for x in a[:3]] + [1.0]),
    # a struct returned by one helper, passed by value to another
    'returned_then_passed': (PLAN + UV2 + ' S atPlan = make(uv);\n'
                             ' return float4(inner(atPlan), 1);\n}\n',
                             lambda a, b, c: plan(a) + [1.0]),
    # forwarded from helper to helper, every combination of names
    'forwarded_same_name': (PLAN + UV2 + ' S atPlan = make(uv);\n'
                            ' return float4(blend(2, atPlan), 1);\n}\n',
                            lambda a, b, c: [2 * x for x in plan(a)] + [1.0]),
    'forwarded_other_name': (PLAN + UV2 + ' S plan = make(uv);\n'
                             ' return float4(blend(2, plan), 1);\n}\n',
                             lambda a, b, c: [2 * x for x in plan(a)] + [1.0]),
    'forwarded_shadowed': (PLAN + UV2 + ' S atPlan = make(uv);\n S other = make(uv2);\n'
                           ' return float4(blend(2, other) + inner(atPlan), 1);\n}\n',
                           lambda a, b, c: [2 * x + y for x, y in zip(plan(b), plan(a))] + [1.0]),
    # all three names equal, a DIFFERENT variable passed: read TEX1, not TEX0
    'same_name_other_value': (SAME + UV2 + ' S atPlan = make(uv);\n S other = make(uv2);\n'
                              ' return float4(blend(2, other), 1);\n}\n',
                              lambda a, b, c: [2 * x for x in plan(b)] + [1.0]),
}
# name -> (source, text the refusal must carry)
FP_REFUSED = {
    # V.b carries TEXCOORD1: loading it for an argument that never assigned
    # it would read that varying.  An unwritten VECTOR member is an undefined
    # value, which lowering refuses; an unwritten scalar has no binding at
    # all and is refused by the rule's own name.
    'unassigned_member': (SEM + 'float4 main(V i) : COLOR {\n V l; l.a = i.a;\n return float4(f(l), 1);\n}\n',
                          'could not be resolved'),
    'unassigned_scalar_member': ('struct W { float3 a : TEXCOORD0; float k : TEXCOORD1; };\n'
                                 'float3 f(W q) { return q.a * q.k; }\n'
                                 'float4 main(float3 n : TEXCOORD0) : COLOR {\n W l; l.a = n;\n'
                                 ' return float4(f(l), 1);\n}\n', 'struct-value-argument'),
    'call_result_argument': ('struct V { float3 a : TEXCOORD0; float3 b : TEXCOORD1; };\n'
                             'V g(float3 n, float3 m) { V r; r.a = n * 2; r.b = m * 3; return r; }\n'
                             'float3 f(V q) { return q.a + q.b; }\n'
                             'float4 main(float3 n : TEXCOORD2, float3 m : TEXCOORD3) : COLOR {\n'
                             ' return float4(f(g(n, m)), 1);\n}\n', 'struct-value-argument'),
}
VPH = ('struct A { float4 p : POSITION; float3 a : TEXCOORD0; float3 b : TEXCOORD1; };\n'
       'float3 f(A q) { return q.a + q.b; }\n'
       'void main(A i, out float4 op : POSITION, out float3 o : TEXCOORD0) {\n')
# name -> (source, expected(TEXCOORD0, TEXCOORD1) -> TEXCOORD0 output)
VP_ROWS = {
    # the entry struct passed whole, no member read first: was a refusal
    'vp_entry_whole': (VPH + ' op = i.p; o = f(i);\n}\n', lambda a, b: [a[i] + b[i] for i in range(3)]),
    'vp_name_capture': (VPH + ' A q; q.p = i.p; q.a = i.a * 5; q.b = i.a * 7;\n float3 r = f(i);\n'
                        ' op = i.p; o = r + q.a;\n}\n', lambda a, b: [a[i] + b[i] + 5 * a[i] for i in range(3)]),
    'vp_entry_modified': (VPH + ' i.a = i.b * 2;\n op = i.p; o = f(i);\n}\n',
                          lambda a, b: [3 * b[i] for i in range(3)]),
    'vp_local_writes': (VPH + ' A l; l.p = i.p; l.a = i.b * 2; l.b = i.a * 3;\n op = i.p; o = f(l);\n}\n',
                        lambda a, b: [2 * b[i] + 3 * a[i] for i in range(3)]),
}
VECTORS = [
    ([0.25, 0.5, 0.75, 1.0], [1.5, 1.25, 1.0, 0.5], [2.0, -0.5, 0.125, 1.0]),
    ([-2.0, 1.0, 4.0, 1.0], [3.0, -1.0, 2.0, 0.5], [0.5, 0.25, -4.0, 1.0]),
    ([4.0, -2.0, 1.0, 1.0], [-1.0, 2.0, 3.0, 0.5], [-0.75, 8.0, 0.5, 1.0]),
]
POSITION = [0.25, 0.5, 0.75, 1.0]


def compile_row(compiler, tmp, name, source, profile):
    src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
    src.write_text(source)
    run = subprocess.run([compiler, '-p', profile, '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=60)
    return run.returncode, (dst.read_bytes() if dst.exists() else b''), run.stdout + run.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    args = parser.parse_args()
    failures, total = [], 0
    with tempfile.TemporaryDirectory(prefix='struct-value-argument-') as tmp:
        for name, (source, expected) in FP_ROWS.items():
            total += 1
            rc, blob, text = compile_row(args.compiler, tmp, name, source, 'sce_fp_rsx')
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (text.strip().splitlines() or ['?'])[0]))
                continue
            for a, b, c in VECTORS:
                want = expected(a, b, c)
                try:
                    got = fp_eval.evaluate(blob, {'TEX0': a, 'TEX1': b, 'TEX2': c})
                except Exception as error:      # an unmodelled or unwritten read is a finding
                    failures.append('%s: evaluator: %s' % (name, error))
                    break
                if list(got) != want:
                    failures.append('%s: TEX0=%s TEX1=%s TEX2=%s got %s want %s' % (name, a, b, c, list(got), want))
                    break
        for name, (source, needle) in FP_REFUSED.items():
            total += 1
            rc, blob, text = compile_row(args.compiler, tmp, name, source, 'sce_fp_rsx')
            if rc != 1 or blob:
                failures.append('%s: must be refused with exit 1 and no container (exit %s, %d bytes)'
                                % (name, rc, len(blob)))
            elif needle not in text:
                failures.append('%s: refused, but not with "%s": %s'
                                % (name, needle, (text.strip().splitlines() or ['?'])[0]))
        for name, (source, expected) in VP_ROWS.items():
            total += 1
            rc, blob, text = compile_row(args.compiler, tmp, name, source, 'sce_vp_rsx')
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (text.strip().splitlines() or ['?'])[0]))
                continue
            for a, b, _ in VECTORS:
                outputs = vp_eval.evaluate(blob, {}, inputs={0: POSITION, 8: a, 9: b}, binary32=True)
                got = outputs[7][:3] if 7 in outputs else None
                if outputs.get(0) != POSITION or got != expected(a, b):
                    failures.append('%s: TEXCOORD0=%s TEXCOORD1=%s position %s got %s want %s'
                                    % (name, a, b, outputs.get(0), got, expected(a, b)))
                    break
    for failure in failures:
        print('FAIL:', failure)
    print('struct value argument: tests=%d pass=%d fail=%d; %d vectors per value row'
          % (total, total - len(failures), len(failures), len(VECTORS)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
