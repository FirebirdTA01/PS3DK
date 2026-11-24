"""break and continue in a statically expanded loop (static-loop-early-exit).

A `for` whose trip sequence is proven is expanded iteration by iteration, and
an expansion has no loop to leave: a body with `break` or `continue` was
refused (in an inlined helper by name, in the entry as a back-edge).  The
established compiler accepts these loops.  Each exit now sets a flag, and
the statements it skips - the rest of its block, and for `break` every later
iteration - are built under that flag.

Every row is judged by VALUE with the evaluators against a plain Python
rendering of the same loop, on inputs chosen so each result is exact in
binary32.  TEXCOORD0.x is the value accumulated, TEXCOORD1.x the run-time
limit the exit tests.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp_eval

HEAD = 'float4 main(float3 p : TEXCOORD0, float3 q : TEXCOORD1) : COLOR {\n float n = q.x;\n'
TAIL = ' return float4(s, t, 0, 1);\n}\n'
FBM = ('float fbm(float3 p, float n) {\n float s = 0; float a = 0.5;\n'
       ' for (int o = 0; o < 6; o++) { if (float(o) >= n) break; s += a * p.x; a *= 0.5; }\n'
       ' return s;\n}\n')


def fbm(x, n):
    s, a = 0.0, 0.5
    for o in range(6):
        if o >= n:
            break
        s += a * x
        a *= 0.5
    return s


def r_break_first(x, n):
    return [fbm(x, n), 0.0]


def r_break_middle(x, n):
    s = t = 0.0
    for o in range(5):
        s += x
        if s > n:
            break
        t += 1.0
    return [s, t]


def r_continue(x, n):
    s = t = 0.0
    for o in range(5):
        t += 1.0
        if o == n:
            continue
        s += x * (o + 1)
    return [s, t]


def r_both(x, n):
    s = t = 0.0
    for o in range(6):
        if o == 1:
            continue
        if o > n:
            break
        s += x
        t += o
    return [s, t]


def r_nested_if(x, n):
    s = t = 0.0
    for o in range(5):
        if x > 0.0:
            if o >= n:
                break
            s += x
        else:
            t += 1.0
    return [s, t]


def r_unconditional(x, n):
    return [x, 0.0]


def r_inner_only(x, n):
    s = t = 0.0
    for o in range(3):
        for i in range(4):
            if i >= n:
                break
            s += x
        t += 1.0
    return [s, t]


def r_gt(x, stop):
    a = 0.0
    for i in range(4):
        if x > i:
            if stop:
                break
            continue
        a += x
    return a


def r_twice(x, n):
    return [fbm(x, n) + fbm(x, n + 2.0), 0.0]


# name -> (source, expected(TEX0.x, TEX1.x) -> [COLOR.x, COLOR.y])
FP_ROWS = {
    'break_first': (HEAD + ' float s = 0; float t = 0; float a = 0.5;\n'
                    ' for (int o = 0; o < 6; o++) { if (float(o) >= n) break; s += a * p.x; a *= 0.5; }\n'
                    + TAIL, r_break_first),
    'break_middle': (HEAD + ' float s = 0; float t = 0;\n'
                     ' for (int o = 0; o < 5; o++) { s += p.x; if (s > n) break; t += 1; }\n' + TAIL, r_break_middle),
    'continue_only': (HEAD + ' float s = 0; float t = 0;\n'
                      ' for (int o = 0; o < 5; o++) { t += 1; if (float(o) == n) continue; s += p.x * (o + 1); }\n'
                      + TAIL, r_continue),
    'break_and_continue': (HEAD + ' float s = 0; float t = 0;\n'
                           ' for (int o = 0; o < 6; o++) { if (o == 1) continue; if (float(o) > n) break;'
                           ' s += p.x; t += o; }\n' + TAIL, r_both),
    'nested_if': (HEAD + ' float s = 0; float t = 0;\n'
                  ' for (int o = 0; o < 5; o++) { if (p.x > 0) { if (float(o) >= n) break; s += p.x; }'
                  ' else { t += 1; } }\n' + TAIL, r_nested_if),
    'unconditional': (HEAD + ' float s = 0; float t = 0;\n'
                      ' for (int o = 0; o < 4; o++) { s += p.x; break; }\n' + TAIL, r_unconditional),
    # a break of the inner loop leaves the outer one running
    'inner_only': (HEAD + ' float s = 0; float t = 0;\n'
                   ' for (int o = 0; o < 3; o++) { for (int i = 0; i < 4; i++) { if (float(i) >= n) break;'
                   ' s += p.x; } t += 1; }\n' + TAIL, r_inner_only),
    # the same loop in a helper, and expanded twice from one tree
    'helper': (FBM + HEAD + ' float s = fbm(p, n); float t = 0;\n' + TAIL, r_break_first),
    # the two helper rows that were named refusals in helper-static-loop-test
    'helper_break_gt': ('float f(float x){float a=0;for(int i=0;i<4;i++){if(x>i)break;a+=x;}return a;}\n'
                        + HEAD + ' float s = f(n); float t = 0;\n' + TAIL, lambda x, n: [r_gt(n, True), 0.0]),
    'helper_continue_gt': ('float f(float x){float a=0;for(int i=0;i<4;i++){if(x>i)continue;a+=x;}return a;}\n'
                           + HEAD + ' float s = f(n); float t = 0;\n' + TAIL, lambda x, n: [r_gt(n, False), 0.0]),
    'helper_twice': (FBM + HEAD + ' float s = fbm(p, n) + fbm(p, n + 2); float t = 0;\n' + TAIL, r_twice),
}
VP_ROWS = {
    'vp_break_first': ('void main(float4 pos : POSITION, float3 p : TEXCOORD0, float3 q : TEXCOORD1,\n'
                       '          out float4 op : POSITION, out float3 o3 : TEXCOORD0) {\n'
                       ' float n = q.x; float s = 0; float a = 0.5;\n'
                       ' for (int o = 0; o < 6; o++) { if (float(o) >= n) break; s += a * p.x; a *= 0.5; }\n'
                       ' op = pos; o3 = float3(s, 0, 0);\n}\n', r_break_first),
    'vp_helper_continue_gt': ('float f(float x){float a=0;for(int i=0;i<4;i++){if(x>i)continue;a+=x;}return a;}\n'
                              'void main(float4 pos : POSITION, float3 p : TEXCOORD0, float3 q : TEXCOORD1,\n'
                              '          out float4 op : POSITION, out float3 o3 : TEXCOORD0) {\n'
                              ' op = pos; o3 = float3(f(q.x), 0, 0);\n}\n', lambda x, n: [r_gt(n, False), 0.0]),
}
VECTORS = [(2.0, 0.0), (2.0, 1.0), (4.0, 2.5), (-8.0, 3.0), (2.0, 6.0), (0.5, 9.0), (2.0, -1.0)]
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
    with tempfile.TemporaryDirectory(prefix='static-loop-early-exit-') as tmp:
        for rows, profile in ((FP_ROWS, 'sce_fp_rsx'), (VP_ROWS, 'sce_vp_rsx')):
            for name, (source, expected) in rows.items():
                total += 1
                rc, blob, text = compile_row(args.compiler, tmp, name, source, profile)
                if rc != 0 or not blob:
                    lines = [l for l in text.splitlines() if 'error' in l or 'refus' in l]
                    failures.append('%s refused: %s' % (name, (lines or text.strip().splitlines() or ['?'])[0]))
                    continue
                for x, n in VECTORS:
                    a, b = [x, 0.0, 0.0, 1.0], [n, 0.0, 0.0, 1.0]
                    want = expected(x, n)
                    try:
                        if profile == 'sce_fp_rsx':
                            got = list(fp_eval.evaluate(blob, {'TEX0': a, 'TEX1': b}))[:2]
                        else:
                            outputs = vp_eval.evaluate(blob, {}, inputs={0: POSITION, 8: a, 9: b}, binary32=True,
                                                       predication=True, branches=True)
                            got, want = [outputs[7][0]], want[:1]
                    except Exception as error:      # an unmodelled or unwritten read is a finding
                        failures.append('%s: evaluator: %s' % (name, error))
                        break
                    if got != want:
                        failures.append('%s: x=%s n=%s got %s want %s' % (name, x, n, got, want))
                        break
    for failure in failures:
        print('FAIL:', failure)
    print('static loop early exit: tests=%d pass=%d fail=%d; %d vectors per row'
          % (total, total - len(failures), len(failures), len(VECTORS)))
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
