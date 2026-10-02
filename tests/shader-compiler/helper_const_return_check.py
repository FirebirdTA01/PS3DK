"""A helper whose returns sit under COMPILE-TIME-CONSTANT conditions (t_a290c3c8).

libretro's gamma-management.h `decode_input` returns from inside
`if (linearize_input) { if (assume_opaque_alpha) ...; else ...; } else ...`
over `static const bool`s, and 121 community shaders that include it were
refused ("a return inside control flow").  The reference accepts every row
below.  An if whose condition folds to a constant now inlines only its taken
branch, so each of the four constant combinations is judged by value.

A return under a RUN-TIME condition (including a parameter that shadows the
file-scope constant - the control that the fold never reads the shadowed
global) is lowered by running each arm with the rest of the body and joining
the results; those rows are judged by value too.
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
REFUSE = {}
# A return under a RUN-TIME condition runs each arm with the rest of the body
# and joins the results with Select.  Expected values are the Cg expression;
# the reference agrees where its program is evaluable here (the first three
# rows, measured on every grid input); the others are predicated programs the
# evaluator does not model.
RUNTIME = {
    'runtime_condition': ("""float4 decode(const float4 c) { if (c.x > 0.5) { return c * 2.0; } else { return c; } }
float4 main(float4 t : TEXCOORD0) : COLOR { return decode(t); }
""", lambda c: [v * 2.0 for v in c] if c[0] > 0.5 else list(c)),
    'shadowed_constant': ("""static const bool lin = true;
float4 decode(const float4 c, bool lin) { if (lin) { return c * 2.0; } else { return c; } }
float4 main(float4 t : TEXCOORD0) : COLOR { return decode(t, t.y > 0.5); }
""", lambda c: [v * 2.0 for v in c] if c[1] > 0.5 else list(c)),
    'nonstatic_const': ("""const bool lin = true;
float4 decode(const float4 c) { if (lin) { return c * 2.0; } else { return c; } }
float4 main(float4 t : TEXCOORD0) : COLOR { return decode(t); }
""", lambda c: [v * 2.0 for v in c]),
    'early_then_tail': ("""float4 f(float4 c) { if (c.x > 0.25) return c * 2.0; float4 d = c + 1.0; return d * 3.0; }
float4 main(float4 t : TEXCOORD0) : COLOR { return f(t); }
""", lambda c: [v * 2.0 for v in c] if c[0] > 0.25 else [(v + 1.0) * 3.0 for v in c]),
    'nested_returns': ("""float4 f(float4 c) { if (c.x > 0.0) { if (c.y > 0.0) return c * 2.0; c = c + 1.0; } return c * 3.0; }
float4 main(float4 t : TEXCOORD0) : COLOR { return f(t); }
""", lambda c: ([v * 2.0 for v in c] if c[1] > 0.0 else [(v + 1.0) * 3.0 for v in c]) if c[0] > 0.0
              else [v * 3.0 for v in c]),
    'sequential': ("""float4 f(float4 c) { if (c.x > 0.5) return c; if (c.y > 0.5) return c * 2.0; if (c.z > 0.5) return c * 4.0; return c * 8.0; }
float4 main(float4 t : TEXCOORD0) : COLOR { return f(t); }
""", lambda c: [v * (1.0 if c[0] > 0.5 else 2.0 if c[1] > 0.5 else 4.0 if c[2] > 0.5 else 8.0) for v in c]),
    'scoped_arm_local': ("""float4 f(float4 c) { float4 k = c * 3.0; if (c.x > 0.0) { float4 k = c * 5.0; if (c.y > 0.0) return k; } return k; }
float4 main(float4 t : TEXCOORD0) : COLOR { return f(t); }
""", lambda c: [v * (5.0 if c[0] > 0.0 and c[1] > 0.0 else 3.0) for v in c]),
    'void_out_early': ("""void g(float4 c, out float4 o) { o = c; if (c.x > 0.0) { o = c * 2.0; return; } o = o * 3.0; }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 r; g(t, r); return r; }
""", lambda c: [v * (2.0 if c[0] > 0.0 else 3.0) for v in c]),
    'global_write': ("""static float4 acc = float4(0, 0, 0, 0);
float4 f(float4 c) { acc = c; if (c.x > 0.0) return c * 2.0; acc = c * 5.0; return c; }
float4 main(float4 t : TEXCOORD0) : COLOR { float4 r = f(t); return r + acc; }
""", lambda c: [v * (3.0 if c[0] > 0.0 else 6.0) for v in c]),
}
RUNTIME_GRID = [[0.5, -0.25, 1.0, 0.75], [-1.0, 0.75, 0.25, 2.0], [0.75, 0.75, 0.0, 1.0],
                [0.0, 0.0, 0.75, 0.5], [0.3, 0.6, 0.9, 0.1], [0.125, 0.25, 0.375, 0.5]]
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
# Precision edges (review: codex), measured on the reference.  These fold
# and must pick the reference's arm:
PREC = """static const float F = 0.1;
static const float G = 16777217.0;
static const half H = 0.1;
float4 pick(const float4 c)
{{
    if ({cond}) {{ return c * 2.0; }}
    else {{ return c * 3.0; }}
}}
float4 main(float4 t : TEXCOORD0) : COLOR {{ return pick(t); }}
"""
PREC_ROWS = [('F == 0.1', True), ('F == float(0.1)', True), ('F != 0.1', False),
             ('G == 16777216.0', True), ('G == 16777217.0', True), ('F * 10.0 == 1.0', True),
             ('F + 0.2 == 0.3', True)]
# ...and these are not folded (refused by name): the reference keeps an int
# against a float unrounded (G == 16777217 is FALSE there) and compares a
# half constant above half precision (H == 0.1 is TRUE there).
PREC_REFUSED = ['G == 16777217', 'H == 0.1', 'H != 0.25']
# A static const that ALIASES one of those shapes through its initialiser is
# refused too (review: codex), as is an int initialiser in a float const,
# which the reference keeps unrounded (Z == 16777217.0 is FALSE there).
ALIAS = """static const float G = 16777217.0;
static const half H = 0.1;
{decl}
float4 pick(const float4 c)
{{
    if ({cond}) {{ return c * 2.0; }}
    else {{ return c * 3.0; }}
}}
float4 main(float4 t : TEXCOORD0) : COLOR {{ return pick(t); }}
"""
ALIAS_REFUSED = [('static const bool B = (G == 16777217);', 'B'),
                 ('static const bool B = (H == 0.1);', 'B'),
                 ('static const float X = H;', 'X == 0.1'),
                 ('static const float Z = 16777217;', 'Z == 16777217.0'),
                 ('static const bool A = (G == 16777217); static const bool B = A;', 'B')]
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
        for i, (cond, first) in enumerate(PREC_ROWS):
            name = 'prec_%d' % i
            rc, blob, err = compile_one(args.compiler, work, name, PREC.format(cond=cond))
            if rc != 0 or not blob:
                failures.append('%s (%s) refused: %s' % (name, cond, (err.strip().splitlines() or ['?'])[-1]))
                continue
            k = 2.0 if first else 3.0
            ok = fp_eval.evaluate(blob, {'TEX0': [1.0, 0.5, 0.25, 1.0]}) == [k, 0.5 * k, 0.25 * k, k]
            print('  %-24s %s  %s' % (name, 'values ok' if ok else 'WRONG ARM', cond))
            if not ok:
                failures.append('%s (%s) picked the wrong return' % (name, cond))
        for i, cond in enumerate(PREC_REFUSED):
            name = 'prec_refused_%d' % i
            rc, blob, err = compile_one(args.compiler, work, name, PREC.format(cond=cond))
            ok = rc == 1 and not blob and 'a return inside control flow' in err
            print('  %-24s %s  %s' % (name, 'refused by name' if ok else 'NOT refused (rc %d)' % rc, cond))
            if not ok:
                failures.append('%s (%s): expected the named refusal, got rc %d' % (name, cond, rc))
        for i, (decl, cond) in enumerate(ALIAS_REFUSED):
            name = 'alias_refused_%d' % i
            rc, blob, err = compile_one(args.compiler, work, name, ALIAS.format(decl=decl, cond=cond))
            ok = rc == 1 and not blob and 'a return inside control flow' in err
            print('  %-24s %s  %s / %s' % (name, 'refused by name' if ok else 'NOT refused (rc %d)' % rc, decl, cond))
            if not ok:
                failures.append('%s (%s / %s): expected the named refusal, got rc %d' % (name, decl, cond, rc))
        for name, (text, want) in RUNTIME.items():
            rc, blob, err = compile_one(args.compiler, work, name, text)
            if rc != 0 or not blob:
                failures.append('%s refused: %s' % (name, (err.strip().splitlines() or ['?'])[-1]))
                print('  %-24s REFUSED' % name)
                continue
            bad = [c for c in RUNTIME_GRID if fp_eval.evaluate(blob, {'TEX0': c}) != want(c)]
            print('  %-24s %s' % (name, 'values ok' if not bad else 'WRONG on %d inputs' % len(bad)))
            if bad:
                failures.append('%s: got %s for %s, want %s' % (
                    name, fp_eval.evaluate(blob, {'TEX0': bad[0]}), bad[0], want(bad[0])))
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
