"""Component-wise !, && and || on vectors, judged by value (t_19e8402f).

Measured against sce-cgc 475, sce_fp_rsx: the reference compiles unary ! on
bool and numeric vectors, && and || on two vectors of one width (bool,
numeric or one of each, in either order), and libretro's float4(!bool4) blend; it refuses bool && bool4 (no
scalar broadcast for the logical operators).  Our containers were checked
against the reference's on 400 inputs per probe with fp_eval (all equal); this
test pins the same rows against values computed here from Cg semantics, so it
needs no reference output in the repository.

Inputs come from the fx12-exact domain (multiples of 1/4 in [-1.5, 1.5]),
where the low-precision comparisons the compiler may emit are exact, and
include ties (a == b, a == 0.5) for every comparison boundary.
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
F = lambda c: 1.0 if c else 0.0


def lanes(n, f):
    return lambda a, b: [f(a[i], b[i]) for i in range(n)] + [1.0] * (4 - n)


ACCEPT = {
    'and_bool4': (M + ' { return (float4)((a > b) && (a > 0.5)); }',
                  lanes(4, lambda x, y: F(x > y and x > 0.5))),
    'or_bool4': (M + ' { return (float4)((a > b) || (b > 0.5)); }',
                 lanes(4, lambda x, y: F(x > y or y > 0.5))),
    'and_bool3': (M + ' { return float4((float3)((a.xyz > b.xyz) && (a.xyz > 0.5)), 1); }',
                  lanes(3, lambda x, y: F(x > y and x > 0.5))),
    'and_float4': (M + ' { return (float4)(a && b); }',
                   lanes(4, lambda x, y: F(x != 0 and y != 0))),
    'not_float4': (M + ' { return (float4)(!a); }',
                   lanes(4, lambda x, y: F(x == 0))),
    'not_bool4_cast': (M + ' { bool4 m = a > b; return (float4)(!m); }',
                       lanes(4, lambda x, y: F(not x > y))),
    'libretro_not3': (M + ' { bool3 c = a.xyz <= 0.75; bool3 nc = !c; return float4((float3)nc, 1); }',
                      lanes(3, lambda x, y: F(not x <= 0.75))),
    'libretro_ctor_not4': (M + ' { const bool4 big = a > float4(0.5); return b * float4(big) + a * float4(!big); }',
                           lanes(4, lambda x, y: y if x > 0.5 else x)),
    'libretro_helper_not3': ('float3 f(float3 z) { const bool3 big = z > float3(0.5); return z * float3(!big); } '
                             + M + ' { return float4(f(a.xyz), 1); }',
                             lanes(3, lambda x, y: 0.0 if x > 0.5 else x)),
    # Mixed bool/numeric vector operands: the reference accepts both operand
    # orders for && and ||, the numeric side meaning != 0 per lane.
    'bool4_and_float4': (M + ' { bool4 m = a > b; return (float4)(m && a); }',
                         lanes(4, lambda x, y: F(x > y and x != 0))),
    'float4_and_bool4': (M + ' { bool4 m = a > b; return (float4)(a && m); }',
                         lanes(4, lambda x, y: F(x != 0 and x > y))),
    'bool4_or_float4': (M + ' { bool4 m = a > b; return (float4)(m || a); }',
                        lanes(4, lambda x, y: F(x > y or x != 0))),
    'float4_or_bool4': (M + ' { bool4 m = a > b; return (float4)(a || m); }',
                        lanes(4, lambda x, y: F(x != 0 or x > y))),
    'unreached_not4': ('float4 unused(float4 z) { const bool4 big = z > 0.5; return float4(!big); } '
                       + M + ' { return a; }',
                       lambda a, b: list(a)),
}
REFUSE = {
    'scalar_and_vector': M + ' { bool s = a.x > 0.5; return (float4)(s && (b > 0.5)); }',
}
VALUES = [-1.5, -1.0, -0.5, 0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5]


def vectors(count=200, seed=20261001):
    rng = random.Random(seed)
    out = []
    for _ in range(count):
        a = [rng.choice(VALUES) for _ in range(4)]
        b = [rng.choice(VALUES) for _ in range(4)]
        if rng.random() < 0.3:
            b = [a[i] if rng.random() < 0.5 else b[i] for i in range(4)]
        out.append((a, b))
    return out


def compile_fp(compiler, work, name, text):
    src, dst = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(text)
    run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                         capture_output=True, text=True, timeout=60)
    return run.returncode, (dst.read_bytes() if dst.exists() else b''), run.stderr


def flip_first_compare(blob):
    """Mutation control: swap the first SGT/SLT (or SGE/SLE) opcode."""
    out = bytearray(blob)
    usize, uoff = struct.unpack_from('>8I', out, 0)[6:8]
    swap = {0x0A: 0x0D, 0x0D: 0x0A, 0x0B: 0x0C, 0x0C: 0x0B}
    for off in range(uoff, uoff + usize, 16):
        raw = struct.unpack_from('>I', out, off)[0]
        logical = ((raw >> 16) | (raw << 16)) & 0xffffffff
        opc = (logical >> 24) & 0x3f
        if opc in swap:
            logical = (logical & ~(0x3f << 24)) | (swap[opc] << 24)
            struct.pack_into('>I', out, off, ((logical >> 16) | (logical << 16)) & 0xffffffff)
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
    with tempfile.TemporaryDirectory(prefix='bool-vector-logic-') as tmp:
        work = Path(tmp)
        blobs = {}
        for name, (text, expect) in ACCEPT.items():
            rc, blob, err = compile_fp(args.compiler, work, name, text)
            if rc != 0 or not blob:
                failures.append('%s: refused (%s)' % (name, err.strip().splitlines()[0] if err.strip() else rc))
                continue
            blobs[name] = blob
            bad = 0
            for a, b in vecs:
                try:
                    got = fp_eval.evaluate(blob, {'TEX0': a, 'TEX1': b})
                except fp_eval.Unmodelled as e:
                    failures.append('%s: unjudged (%s)' % (name, e))
                    break
                if got != expect(a, b):
                    bad += 1
                    if bad == 1:
                        failures.append('%s: a=%s b=%s got %s want %s' % (name, a, b, got, expect(a, b)))
            print('  %-22s %s' % (name, 'values ok (%d inputs)' % len(vecs) if not bad else 'WRONG on %d inputs' % bad))
        for name, text in REFUSE.items():
            rc, blob, err = compile_fp(args.compiler, work, name, text)
            ok = rc == 1 and not blob
            print('  %-22s %s' % (name, 'refused like the reference' if ok else 'NOT refused (rc %d)' % rc))
            if not ok:
                failures.append('%s: expected exit 1 and no container, got rc %d' % (name, rc))
        # the evaluator must see a wrong comparison
        if 'and_bool4' in blobs:
            mutant = flip_first_compare(blobs['and_bool4'])
            caught = mutant is not None and any(
                fp_eval.evaluate(mutant, {'TEX0': a, 'TEX1': b}) != ACCEPT['and_bool4'][1](a, b) for a, b in vecs)
            print('  %-22s %s' % ('control: flipped compare', 'caught' if caught else 'NOT CAUGHT'))
            if not caught:
                failures.append('mutation control: a flipped comparison was not detected')
    for f in failures:
        print('FAIL:', f)
    print('bool-vector-logic: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
