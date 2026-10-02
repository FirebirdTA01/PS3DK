"""A helper whose returns sit under COMPILE-TIME-CONSTANT conditions (t_a290c3c8).

libretro's gamma-management.h `decode_input` returns from inside
`if (linearize_input) { if (assume_opaque_alpha) ...; else ...; } else ...`
over `static const bool`s, and 121 community shaders that include it were
refused ("a return inside control flow").  The reference accepts every row
below.  An if whose condition folds to a constant now inlines only its taken
branch, so each of the four constant combinations is judged by value.

Still a NAMED GAP, refused by name: a return under a run-time condition, and
one whose condition names a parameter that shadows the file-scope constant -
the second is also the control that the fold never reads the shadowed global.
"""
import argparse
import itertools
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

DECODE = """static const bool lin = {lin};
static const bool opaque = {opaque};
float4 decode(const float4 c)
{{
    if (lin)
    {{
        if (opaque) {{ return float4(c.rgb * 2.0, 1.0); }}
        else        {{ return float4(c.rgb * 2.0, c.a); }}
    }}
    else {{ return c; }}
}}
float4 main(float4 t : TEXCOORD0) : COLOR {{ return decode(t); }}
"""
REFUSE = {
    'runtime_condition': """float4 decode(const float4 c) { if (c.x > 0.5) { return c * 2.0; } else { return c; } }
float4 main(float4 t : TEXCOORD0) : COLOR { return decode(t); }
""",
    # A file-scope const WITHOUT static is a uniform with a default in the
    # reference, so its condition is run-time here, never folded.
    'nonstatic_const': """const bool lin = true;
float4 decode(const float4 c) { if (lin) { return c * 2.0; } else { return c; } }
float4 main(float4 t : TEXCOORD0) : COLOR { return decode(t); }
""",
    'shadowed_constant': """static const bool lin = true;
float4 decode(const float4 c, bool lin) { if (lin) { return c * 2.0; } else { return c; } }
float4 main(float4 t : TEXCOORD0) : COLOR { return decode(t, t.y > 0.5); }
""",
}
# A taken constant branch is still a block: its own local ends with it, so
# the return after it reads the OUTER k (t * 3), never the inner one (t * 5).
SCOPED = """static const bool on = true;
float4 scale(const float4 c)
{
    float4 k = c * 3.0;
    if (on) { float4 k = c * 5.0; k = k + 1.0; }
    return k;
}
float4 main(float4 t : TEXCOORD0) : COLOR { return scale(t); }
"""
# Conditions fold by TYPED value (review: codex - 1 == 2 once folded true
# as "both truthy").  COND picks the helper's first return (c * 2) when true,
# else the second (c * 3); every expected arm was measured on the reference.
PICK = """static const int K = 3;
static const int L = 4;
static const float F = 0.5;
float4 pick(const float4 c)
{{
    if ({cond}) {{ return c * 2.0; }}
    else {{ return c * 3.0; }}
}}
float4 main(float4 t : TEXCOORD0) : COLOR {{ return pick(t); }}
"""
PICK_ROWS = [('1 == 2', False), ('1 != 2', True), ('2 == 2', True), ('K == L', False),
             ('K != L', True), ('!(K != L)', False), ('K < L', True), ('(K < L) == false', False),
             ('F == 0.5', True), ('F > 0.75 || K == 3', True), ('K == 3 && L == 3', False),
             ('K * 2 == 6', True), ('-K == 3', False)]
GRID = [-1.0, -0.25, 0.0, 0.5, 0.75, 1.0]


def compile_one(compiler, work, name, text):
    src, dst = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(text)
    run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=60)
    return run.returncode, (dst.read_bytes() if dst.exists() else b''), run.stderr


def expected(lin, opaque, t):
    if not lin:
        return list(t)
    return [t[0] * 2.0, t[1] * 2.0, t[2] * 2.0, 1.0 if opaque else t[3]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    args = ap.parse_args()
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    rng_inputs = [list(v) for v in itertools.product(GRID, repeat=2)]
    with tempfile.TemporaryDirectory(prefix='helper-const-return-') as tmp:
        work = Path(tmp)
        for lin, opaque in itertools.product((True, False), repeat=2):
            name = 'decode_%s_%s' % (lin, opaque)
            text = DECODE.format(lin=str(lin).lower(), opaque=str(opaque).lower())
            rc, blob, err = compile_one(args.compiler, work, name, text)
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                print('  %-24s REFUSED' % name)
                continue
            bad = 0
            for a, b in rng_inputs:
                t = [a, b, a * 0.5, b * 0.5]
                if fp_eval.evaluate(blob, {'TEX0': t}) != expected(lin, opaque, t):
                    bad += 1
            print('  %-24s %s' % (name, 'values ok' if not bad else 'WRONG on %d' % bad))
            if bad:
                failures.append('%s values wrong on %d inputs' % (name, bad))
        rc, blob, err = compile_one(args.compiler, work, 'scoped_local', SCOPED)
        if rc != 0 or not blob:
            failures.append('scoped_local refused: %s' % (err.strip().splitlines() or ['?'])[-1])
            print('  %-24s REFUSED' % 'scoped_local')
        else:
            bad = sum(1 for a, b in rng_inputs
                      if fp_eval.evaluate(blob, {'TEX0': [a, b, b, a]}) != [a * 3.0, b * 3.0, b * 3.0, a * 3.0])
            print('  %-24s %s' % ('scoped_local', 'outer k returned' if not bad else 'WRONG on %d' % bad))
            if bad:
                failures.append('scoped_local: inner block local leaked on %d inputs' % bad)
        for i, (cond, first) in enumerate(PICK_ROWS):
            name = 'pick_%d' % i
            rc, blob, err = compile_one(args.compiler, work, name, PICK.format(cond=cond))
            if rc != 0 or not blob:
                failures.append('%s (%s) refused: %s' % (name, cond, (err.strip().splitlines() or ['?'])[-1]))
                print('  %-24s REFUSED  %s' % (name, cond))
                continue
            k = 2.0 if first else 3.0
            bad = sum(1 for a, b in rng_inputs
                      if fp_eval.evaluate(blob, {'TEX0': [a, b, b, a]}) != [a * k, b * k, b * k, a * k])
            print('  %-24s %s  %s' % (name, 'values ok' if not bad else 'WRONG on %d' % bad, cond))
            if bad:
                failures.append('%s (%s) picked the wrong return on %d inputs' % (name, cond, bad))
        for name, text in REFUSE.items():
            rc, blob, err = compile_one(args.compiler, work, name, text)
            ok = rc == 1 and not blob and 'a return inside control flow' in err
            print('  %-24s %s' % (name, 'refused by name' if ok else 'NOT refused by name (rc %d)' % rc))
            if not ok:
                failures.append('%s: expected the named early-return refusal, got rc %d' % (name, rc))
    for f in failures:
        print('FAIL:', f)
    print('helper-const-return: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
