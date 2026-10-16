#!/usr/bin/env bash
# A vertex input with no semantic - a bare parameter or a varying struct
# member - takes the lowest ATTR<N> no explicit input semantic claims, in
# declaration order across parameters and members, and only when the
# program reads it; an unread one has no resource and takes no index
# (measured on the reference, every row below).  Ours bound a member to
# its position in its struct, so a member beside a COLOR or NORMAL input
# read that input's attribute, with exit 0.  Judged by the records and by
# value: each input gets its own random vector and a distinct weight, and
# the program's result must be the weighted sum read through the bindings
# the records declare.  An input read only under a constant-false
# condition, `false` or `0 > 1`, counts as unread.
#
# Usage: tests/shader-compiler/vp-implicit-attribute-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "vp-implicit-attribute: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import math, random, struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import vp_pow_vector_check as ev
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
ATTR0, NONE = 0x841, 0xcb8
NL = chr(10)
# (name, source, weighted terms, bindings measured on the reference)
ROWS = [
 ('member_gap', 'struct S { float4 a; float4 b; float4 c; float4 d; };' + NL +
  'float4 main(S s) : POSITION { return s.a + s.c * 2; }', [('s.a',1),('s.c',2)]),
 ('member_vs_color', 'struct S { float4 a; float4 b; float4 c; float4 d; float4 e; float4 f; };' + NL +
  'float4 main(S s, float4 col : COLOR) : POSITION { return s.a + s.b * 2 + s.c * 3 + s.d * 4 + s.e * 5 + s.f * 6 + col * 7; }',
  [('s.a',1),('s.b',2),('s.c',3),('s.d',4),('s.e',5),('s.f',6),('col',7)]),
 ('bare_unread', 'float4 main(float4 x, float4 y, float4 z) : POSITION { return x + z * 2; }', [('x',1),('z',2)]),
 ('two_structs', 'struct S { float4 a; float4 b; };' + NL + 'struct T { float4 c; float4 d; };' + NL +
  'float4 main(S s, T t) : POSITION { return s.b + t.c * 2 + t.d * 3; }', [('s.b',1),('t.c',2),('t.d',3)]),
 ('member_vs_normal', 'struct S { float4 a; float4 b : NORMAL; float4 c; };' + NL +
  'float4 main(S s) : POSITION { return s.a + s.b * 2 + s.c * 3; }', [('s.a',1),('s.b',2),('s.c',3)]),
 ('bare_struct_attr', 'struct S { float4 a; float4 b; };' + NL +
  'float4 main(float4 x, S s, float4 n : ATTR1) : POSITION { return x + s.a * 2 + s.b * 3 + n * 4; }',
  [('x',1),('s.a',2),('s.b',3),('n',4)]),
 ('same_struct_twice', 'struct S { float4 a; float4 b; };' + NL +
  'float4 main(S p, S q) : POSITION { return p.b + q.a * 2; }', [('p.b',1),('q.a',2)]),
 ('nested_member', 'struct Inner { float4 b; float4 c; };' + NL + 'struct In { float4 a; Inner i; };' + NL +
  'float4 main(In v, float4 col : COLOR) : POSITION { return v.a + v.i.c * 2 + col * 3; }', [('v.a',1),('v.i.c',2),('col',3)]),
 ('bare_vs_texcoord', 'float4 main(float4 a : ATTR0, float4 x, float4 y : TEXCOORD0) : POSITION { return a + x * 2 + y * 3; }',
  [('a',1),('x',2),('y',3)]),
 ('dead_compare', 'struct S { float4 a; float4 b; float4 c; };' + NL +
  'float4 main(S s) : POSITION { float4 r = s.b; if (0 > 1) r += s.a; return r + s.c * 2; }', [('s.b',1),('s.c',2)]),
 ('dead_false', 'struct S { float4 a; float4 b; float4 c; };' + NL +
  'float4 main(S s) : POSITION { float4 r = s.b; if (false) r += s.a; return r + s.c * 2; }', [('s.b',1),('s.c',2)]),
 ('member_then_bare', 'struct S { float4 a; float4 b; };' + NL +
  'float4 main(S s, float4 z, uniform float4 k) : POSITION { return z + s.b * 2 + k; }', [('z',1),('s.b',2)]),

]
EXPECT = {
    'member_gap': [('s.a',0),('s.b',None),('s.c',1),('s.d',None)],
    'member_vs_color': [('s.a',0),('s.b',1),('s.c',2),('s.d',4),('s.e',5),('s.f',6),('col',3)],
    'bare_unread': [('x',0),('y',None),('z',1)],
    'two_structs': [('s.a',None),('s.b',0),('t.c',1),('t.d',2)],
    'member_vs_normal': [('s.a',0),('s.b',2),('s.c',1)],
    'bare_struct_attr': [('x',0),('s.a',2),('s.b',3),('n',1)],
    'same_struct_twice': [('p.a',None),('p.b',0),('q.a',1),('q.b',None)],
    'nested_member': [('v.a',0),('v.i.b',None),('v.i.c',1),('col',3)],
    'bare_vs_texcoord': [('a',0),('x',1),('y',8)],
    'dead_compare': [('s.a',None),('s.b',0),('s.c',1)],
    'dead_false': [('s.a',None),('s.b',0),('s.c',1)],
    'member_then_bare': [('s.a',None),('s.b',0),('z',1)],
}

def inputs_of(blob):
    count, offset = struct.unpack_from('>2I', blob, 12)
    seen = []
    for i in range(count):
        w = struct.unpack_from('>12I', blob, offset + 48 * i)
        name = blob[w[4]:blob.index(b'\0', w[4])].decode() if w[4] else ''
        if ATTR0 <= w[1] < ATTR0 + 16:
            seen.append((name, w[1] - ATTR0, w[10]))
        elif w[1] == NONE and name not in ('k',):
            seen.append((name, None, w[10]))
    return seen

def agrees(got, want):
    if got is None or len(got) != 4:
        return False
    if not all(isinstance(x, float) and math.isfinite(x) for x in got):
        return False
    return all(abs(x - y) <= 1e-5 * max(1.0, abs(y)) for x, y in zip(got, want))
assert not agrees([math.nan] * 4, [0.0] * 4) and not agrees([], [0.0] * 4), 'instrument control'

fails = []
rnd = random.Random(11)
for name, src, terms in ROWS:
    s, o = work / (name + '.cg'), work / (name + '.vpo')
    s.write_text(src + NL)
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=120)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d): %s' % (name, p.returncode, (p.stdout + p.stderr).strip().splitlines()[-1:]))
        print('  %-18s REFUSED' % name); continue
    blob = o.read_bytes()
    want = [(n, a, 0 if a is None else 1) for n, a in EXPECT[name]]
    got = inputs_of(blob)
    if got != want:
        fails.append('%s: input records %s, want %s' % (name, got, want))
        print('  %-18s RECORDS' % name); continue
    attr = {n: a for n, a in EXPECT[name]}
    wrong = 0
    for _ in range(12):
        vecs = {i: [f32(rnd.uniform(-2, 2)) for _ in range(4)] for i in range(16)}
        k = [f32(rnd.uniform(-2, 2)) for _ in range(4)]
        expect = [0.0] * 4
        for n, weight in terms:
            for j in range(4):
                expect[j] += weight * vecs[attr[n]][j]
        if name == 'member_then_bare':
            expect = [e + kj for e, kj in zip(expect, k)]
        res = ev.evaluate(blob, {'k': k}, inputs=vecs, binary32=True, predication=True,
                          branches=True).get(0)
        if not agrees(res, expect):
            wrong += 1
            if wrong == 1: fails.append('%s: got %s want %s' % (name, res, expect))
    print('  %-18s %s' % (name, 'ok' if wrong == 0 else '%d WRONG' % wrong))

# Varying matrices are a separate, measured gap. Even m[1] with ATTR0
# explicitly assigned currently reads ATTR0.y four times, instead of ATTR1.
# An unread matrix must also reserve its rows before any implicit input;
# until row loads/records are implemented these shapes must refuse.
MATRIX_REFUSALS = {
    'matrix_read': 'float4 main(float4x4 m:ATTR0):POSITION{return m[1];}',
    'matrix_explicit_neighbors': 'float4 main(float4x4 m:ATTR0,float4 x:ATTR4):POSITION{return m[1]+2*x;}',
    'matrix_implicit_neighbor': 'float4 main(float4x4 m:ATTR0,float4 x):POSITION{return x;}',
    'matrix_implicit': 'float4 main(float4x4 m,float4 x:ATTR4):POSITION{return x;}',
    'matrix_member': 'struct S{float4x4 m:ATTR0;float4 x;}; float4 main(S s,float4 y):POSITION{return s.m[1]+2*s.x+3*y;}',
    'matrix_member_unread': 'struct S{float4x4 m:ATTR0;float4 x;}; float4 main(S s,float4 y):POSITION{return 2*s.x+3*y;}',
    'matrix_nested': 'struct T{float4x4 m:ATTR0;}; struct S{T t;float4 x;}; float4 main(S s,float4 y):POSITION{return 2*s.x+3*y;}',
    'matrix_member_read_explicit': 'struct S{float4x4 m:ATTR0;float4 x:ATTR4;}; float4 main(S s):POSITION{return s.m[1]+s.x;}',
    'matrix_bare_array_neighbor': 'float4 main(float4x4 m:ATTR0,float4 x[2]):POSITION{return x[0];}',
}
for name, src in MATRIX_REFUSALS.items():
    s, o = work / (name + '.cg'), work / (name + '.vpo')
    s.write_text(src + NL)
    o.unlink(missing_ok=True)
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=120)
    ok = p.returncode == 1 and 'vp-varying-matrix-input' in p.stdout + p.stderr and not o.exists()
    if not ok: fails.append('%s: expected named refusal and no container; rc=%d, exists=%s' % (name, p.returncode, o.exists()))
    print('  %-28s %s' % (name, 'refused' if ok else 'FAIL'))

# Preserve the correct neighbour: an unread explicit matrix and only
# explicitly bound numeric inputs. Only x is read; its record and value
# must keep ATTR4. Also exercise DCE before the matrix-read guard.
for name, src in [
    ('matrix_unread_explicit', 'float4 main(float4x4 m:ATTR0,float4 x:ATTR4):POSITION{return x;}'),
    ('matrix_dead_explicit', 'float4 main(float4x4 m:ATTR0,float4 x:ATTR4):POSITION{float4 v=x;if(false)v=m[1];return v;}'),
    ('matrix_uniform_unread', 'float4 main(uniform float4x4 m,float4 x:ATTR4):POSITION{return x;}'),
]:
    s, o = work / (name + '.cg'), work / (name + '.vpo')
    s.write_text(src + NL)
    o.unlink(missing_ok=True)
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=120)
    ok = p.returncode == 0 and o.is_file() and o.stat().st_size > 0
    if ok:
        blob = o.read_bytes()
        ok = [r for r in inputs_of(blob) if r[0] == 'x'] == [('x', 4, 1)]
        vecs = {i: [float(10*i+j+1) for j in range(4)] for i in range(16)}
        ok = ok and agrees(ev.evaluate(blob, {}, inputs=vecs, binary32=True, predication=True,
                                       branches=True).get(0), vecs[4])
    if not ok: fails.append('%s: explicit unread matrix neighbour failed' % name)
    print('  %-28s %s' % (name, 'ok' if ok else 'FAIL'))

for f in fails: print('vp-implicit-attribute: FAIL', f)
print('vp-implicit-attribute: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
