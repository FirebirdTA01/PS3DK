"""Vector conditions in ?:, judged by value (t_3b3a3e1e).

Measured against sce-cgc 475: a bool or numeric vector condition of width N is
accepted when the arms' common type is an N-wide vector (scalar arms widen);
two scalar arms, a width mismatch and a vector `if` condition are refused.
The reference selects per lane with predication (a comparison writes the
condition code, a MOV commits where it holds); every reference container for
the accepted rows matched the values below on 400 inputs with fp_eval's
condition-code model.  This test pins ours against values computed here from
Cg semantics, so it needs no reference output in the repository.

Fragment only.  The reference also accepts these shapes on sce_vp_rsx, where
ours refuses: the VP path has no predicated lowering, and its arithmetic blend
is not a conditional move.  Those rows are pinned as a named gap.
"""
import argparse
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

M = 'float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : COLOR'
V = ('void main(float4 p : POSITION, float4 a : TEXCOORD0, float4 b : TEXCOORD1, '
     'out float4 pos : POSITION, out float4 colour : COLOR) { pos = p; ')


def sel(c, x, y):
    return x if c else y


def per_lane(n, f, tail=()):
    return lambda a, b: [f(a[i], b[i], i) for i in range(n)] + list(tail)


CONST_ARMS = [(1.0, 0.0), (0.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
ACCEPT = {
    'bool4': (M + ' { return (a > b) ? a : b; }',
              per_lane(4, lambda x, y, i: sel(x > y, x, y))),
    'float4_cond': (M + ' { return a ? a : b; }',
                    per_lane(4, lambda x, y, i: sel(x != 0, x, y))),
    'bool4_const_arms': (M + ' { bool4 m = a > b; return m ? float4(1,0,0,1) : float4(0,0,1,1); }',
                         per_lane(4, lambda x, y, i: sel(x > y, *CONST_ARMS[i]))),
    'bool3': (M + ' { return float4((a.xyz > 0.5) ? a.xyz : b.xyz, 1); }',
              per_lane(3, lambda x, y, i: sel(x > 0.5, x, y), (1.0,))),
    'bool2': (M + ' { return float4((a.xy > b.xy) ? a.xy : b.xy, 0, 1); }',
              per_lane(2, lambda x, y, i: sel(x > y, x, y), (0.0, 1.0))),
    'scalar_cond': (M + ' { return (a.x > b.x) ? a : b; }',
                    lambda a, b: list(a) if a[0] > b[0] else list(b)),
    'and_cond': (M + ' { return ((a > b) && (a > 0.5)) ? a : b; }',
                 per_lane(4, lambda x, y, i: sel(x > y and x > 0.5, x, y))),
    'bool_arms': (M + ' { bool4 m = a > b; return m ? (a > 0.5) : (b > 0.5); }',
                  per_lane(4, lambda x, y, i: float(sel(x > y, x > 0.5, y > 0.5)))),
    'scalar_else': (M + ' { return (a > b) ? a : 0.0; }',
                    per_lane(4, lambda x, y, i: sel(x > y, x, 0.0))),
    'scalar_then': (M + ' { return (a > b) ? 1.0 : b; }',
                    per_lane(4, lambda x, y, i: sel(x > y, 1.0, y))),
    'nested': (M + ' { return (a > b) ? a : ((a < 0) ? -a : b); }',
               per_lane(4, lambda x, y, i: sel(x > y, x, sel(x < 0, -x, y)))),
    'half_arms': (M + ' { half4 h = a; return (a > b) ? h : (half4)b; }',
                  per_lane(4, lambda x, y, i: sel(x > y, x, y))),
    'int4_cond': (M + ' { int4 k = (int4)(a*4); return k ? a : b; }',
                  per_lane(4, lambda x, y, i: sel(int(x * 4) != 0, x, y))),
    # Finite constant arms an arithmetic blend gets wrong: (1 - 2^30) + 2^30
    # is 0 in binary32 (codex, review of c0b6589e).  The scalar form was
    # already wrong before vector conditions existed.
    'big_const_arms': (M + ' { return (a > b) ? float4(1.0) : float4(1073741824.0); }',
                       per_lane(4, lambda x, y, i: sel(x > y, 1.0, 2.0 ** 30))),
    'big_const_arms_scalar': (M + ' { return (a.x > b.x) ? float4(1.0) : float4(1073741824.0); }',
                              lambda a, b: [1.0 if a[0] > b[0] else 2.0 ** 30] * 4),
}
REFUSE_FP = {
    'scalar_arms': M + ' { return (a > b) ? 1.0 : 0.0; }',
    'cond2_arms4': M + ' { return (a.xy > b.xy) ? a : b; }',
    'cond4_arms3': M + ' { return float4((a > b) ? a.xyz : b.xyz, 1); }',
    'cond3_arms4': M + ' { return float4((a.xyz > b.xyz) ? a : b); }',
    'if_bool4': M + ' { float4 c = b; if (a > b) c = a; return c; }',
}
# The reference accepts the first two; ours refuses them (named gap).
REFUSE_VP = {
    'vp_bool4': (V + 'colour = (a > b) ? a : b; }', 'vertex select with a vector condition'),
    'vp_float4_cond': (V + 'colour = a ? a : b; }', 'vertex select with a vector condition'),
    'vp_scalar_arms': (V + 'colour = (a > b) ? 1.0 : 0.0; }', 'needs 4-component operands'),
}
GRID = [-1.5, -1.0, -0.5, 0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5]
# Rows judged on the grid only: half rounding/subnormal flushing and int
# overflow of the rounding inputs are separate questions from selection.
GRID_ONLY = {'half_arms', 'int4_cond'}
# binary32 values where an arithmetic blend (a - b) + b would not return a
ROUNDING = [([2.0 ** -30, 3.0, -2.0 ** -28, 1.0], [1.0, 2.0, -1.0, 1e30]),
            ([1e-20, -1e-20, 0.0, 7.0], [-1e20, 1e20, 5.0, 7.0])]


def vectors(count=200, seed=20261001):
    rng = random.Random(seed)
    out = []
    for _ in range(count):
        a = [rng.choice(GRID) for _ in range(4)]
        b = [rng.choice(GRID) for _ in range(4)]
        if rng.random() < 0.3:
            b = [a[i] if rng.random() < 0.5 else b[i] for i in range(4)]
        out.append((a, b))
    return out + ROUNDING


def compile_one(compiler, work, name, text, profile):
    src, dst = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(text)
    run = subprocess.run([compiler, '-p', profile, '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=60)
    return run.returncode, (dst.read_bytes() if dst.exists() else b''), run.stderr


def flip_predicate(blob):
    """Mutation control: turn the first NE-predicated instruction into EQ."""
    out = bytearray(blob)
    usize, uoff = struct.unpack_from('>8I', out, 0)[6:8]
    for off in range(uoff, uoff + usize, 16):
        raw = struct.unpack_from('>I', out, off + 4)[0]
        w1 = ((raw >> 16) | (raw << 16)) & 0xffffffff
        if (w1 >> 18) & 7 == 5:
            w1 = (w1 & ~(7 << 18)) | (2 << 18)
            struct.pack_into('>I', out, off + 4, ((w1 >> 16) | (w1 << 16)) & 0xffffffff)
            return bytes(out)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    args = ap.parse_args()
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test (encoded controls) failed')
    vecs = vectors()
    with tempfile.TemporaryDirectory(prefix='vector-select-') as tmp:
        work = Path(tmp)
        blobs = {}
        for name, (text, expect) in ACCEPT.items():
            rc, blob, err = compile_one(args.compiler, work, name, text, 'sce_fp_rsx')
            if rc != 0 or not blob:
                lines = [l for l in err.splitlines() if 'error' in l or 'refusing' in l]
                failures.append('%s: refused (%s)' % (name, lines[0] if lines else rc))
                print('  %-20s REFUSED' % name)
                continue
            blobs[name] = blob
            bad = 0
            rows = vecs[:-len(ROUNDING)] if name in GRID_ONLY else vecs
            for a, b in rows:
                try:
                    got = fp_eval.evaluate(blob, {'TEX0': a, 'TEX1': b})
                except fp_eval.Unmodelled as e:
                    failures.append('%s: unjudged (%s)' % (name, e))
                    bad = -1
                    break
                if got != expect(a, b):
                    bad += 1
                    if bad == 1:
                        failures.append('%s: a=%s b=%s got %s want %s' % (name, a, b, got, expect(a, b)))
            print('  %-20s %s' % (name, 'values ok (%d inputs)' % len(rows) if bad == 0
                                  else 'UNJUDGED' if bad < 0 else 'WRONG on %d inputs' % bad))
        for name, text in REFUSE_FP.items():
            rc, blob, err = compile_one(args.compiler, work, name, text, 'sce_fp_rsx')
            ok = rc == 1 and not blob
            print('  %-20s %s' % (name, 'refused like the reference' if ok else 'NOT refused (rc %d)' % rc))
            if not ok:
                failures.append('%s: expected exit 1 and no container, got rc %d' % (name, rc))
        for name, (text, why) in REFUSE_VP.items():
            rc, blob, err = compile_one(args.compiler, work, name, text, 'sce_vp_rsx')
            ok = rc == 1 and not blob and why in err
            print('  %-20s %s' % (name, 'refused (%s)' % why if ok else 'NOT refused as expected (rc %d)' % rc))
            if not ok:
                failures.append('%s: expected exit 1, no container and "%s", got rc %d' % (name, why, rc))
        # the evaluator must see a predicate that tests the wrong condition
        if 'bool4' in blobs:
            mutant = flip_predicate(blobs['bool4'])
            caught = mutant is not None and any(
                fp_eval.evaluate(mutant, {'TEX0': a, 'TEX1': b}) != ACCEPT['bool4'][1](a, b) for a, b in vecs)
            print('  %-20s %s' % ('control: NE->EQ', 'caught' if caught else 'NOT CAUGHT'))
            if not caught:
                failures.append('mutation control: an inverted predicate was not detected')
    for f in failures:
        print('FAIL:', f)
    print('vector-select: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
