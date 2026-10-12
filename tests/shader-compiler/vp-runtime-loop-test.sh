#!/usr/bin/env bash
# A vertex program loop whose trip count is a run-time value (PSGL's
# vpFixedPipeline: `for (int i = 0; i < nLights; ++i)`).  Measured on the
# reference: real branches - the test, BRA to the exit where it fails, the
# body (relative constant reads through ARL), BRA back.  Ours lowers the
# loop region the fragment hardware loop uses to the same two branches.
# Judged by value against independent float arithmetic for trip counts 0,
# 1, several, fractional, negative and the full array; the carry swap, a
# predicated (CC-writing) body, reads before / inside / after the loop and
# two arrays are rows.  A flipped back-edge in our own container must be
# caught (instrument control).  Shapes outside the slice keep a named
# refusal and write no container.
#
# Usage: tests/shader-compiler/vp-runtime-loop-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "vp-runtime-loop: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import random, struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import vp_pow_vector_check as ev
# The instrument first: its branch and address-register controls.
ev.branches_selftest()
ev.indexed_constants_selftest()
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
NL = chr(10)
H = 'uniform float n; uniform float4 k[8]; uniform float4 j[8]; uniform float4 u[4]; uniform float m; uniform float4 f;' + NL
M = 'float4 main(float4 p : POSITION) : POSITION { '
add = lambda a, b: [x + y for x, y in zip(a, b)]
sub = lambda a, b: [x - y for x, y in zip(a, b)]
mul = lambda a, b: [x * y for x, y in zip(a, b)]
sc = lambda a, s: [x * s for x in a]

def trips(n):
    i = 0
    while i < n and i < 8:
        yield i
        i += 1

def e_sum(n, p, U):
    s = p
    for i in trips(n): s = add(s, U['k'][i])
    return s
def e_swap(n, p, U):
    a, b = p, p[1:] + p[:1]
    for i in trips(n): a, b = add(b, U['k'][i]), a
    return add(sc(a, 2.0), b)
def e_pred(n, p, U):
    s = p
    for i in trips(n):
        k = U['k'][i]
        s = add(s, k) if k[0] > 0 else sub(s, k[1:] + k[:1])
    return s
def e_post(n, p, U):
    m = int(U['m'][0]); s = U['u'][m]; last = p
    for i in trips(n):
        s = add(s, mul(U['k'][i], U['u'][m])); last = U['k'][i]
    return add(add(s, last), U['u'][m + 1])
def e_two(n, p, U):
    s = p
    for i in trips(n): s = add(s, mul(U['k'][i], U['j'][7 - i]))
    return s
def e_in_if(n, p, U):
    if U['f'][0] != 0:
        return sc(e_sum(n, p, U), 2.0)
    return [x - 1 for x in p]
def e_float(n, p, U):
    s = p
    for i in trips(n): s = add(sc(s, 0.5), U['k'][i])
    return s

ROWS = [
    ('sum', M + 'float4 s = p; for (int i = 0; i < n; ++i) s += k[i]; return s; }', e_sum),
    ('carry_swap', M + 'float4 a = p; float4 b = p.yzwx; for (int i = 0; i < n; ++i) '
                   '{ float4 t = a; a = b + k[i]; b = t; } return a * 2 + b; }', e_swap),
    ('predicated_body', M + 'float4 s = p; for (int i = 0; i < n; ++i) '
                        '{ if (k[i].x > 0) s += k[i]; else s -= k[i].yzwx; } return s; }', e_pred),
    ('reads_around_loop', M + 'float4 s = u[m]; float4 last = p; for (int i = 0; i < n; ++i) '
                          '{ s += k[i] * u[m]; last = k[i]; } return s + last + u[m + 1]; }', e_post),
    ('two_arrays', M + 'float4 s = p; for (int i = 0; i < n; ++i) s += k[i] * j[7 - i]; return s; }', e_two),
    # Inside a flattened if: the loop runs, the join selects (both arms judged).
    ('loop_in_if', M + 'float4 s = p; if (f.x != 0) { for (int i = 0; i < n; ++i) s += k[i]; s = s * 2; } '
                   'else { s = s - 1; } return s; }', e_in_if),
    ('float_induction', M + 'float4 s = p; for (float i = 0; i < n; i += 1) s = s * 0.5 + k[int(i)]; return s; }', e_float),
]
NS = [0, 1, 2, 2.5, 3, 5, 7.9, 8, -1]
fails = []

def compile_one(name, src):
    s, o = work / (name + '.cg'), work / (name + '.vpo')
    s.write_text(H + src + NL)
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=120)
    return p, o

def judge(blob, fn, seed):
    rnd = random.Random(seed); wrong = 0
    for n in NS:
        U = {'k': [[f32(rnd.uniform(-2, 2)) for _ in range(4)] for _ in range(8)],
             'j': [[f32(rnd.uniform(-2, 2)) for _ in range(4)] for _ in range(8)],
             'u': [[f32(rnd.uniform(-2, 2)) for _ in range(4)] for _ in range(4)],
             'm': [f32(rnd.choice([0, 1, 2]) + 0.25), 0, 0, 0],
             'f': [float(rnd.choice([0, 1])), 0, 0, 0]}
        p = [f32(rnd.uniform(-1, 1)) for _ in range(4)]
        uni = {'n': [n, 0, 0, 0], 'm': U['m'], 'f': U['f']}
        for a in ('k', 'j', 'u'):
            uni.update({'%s[%d]' % (a, i): v for i, v in enumerate(U[a])})
        got = ev.evaluate(blob, uni, inputs={0: p}, binary32=True, predication=True,
                          indexed_constants=True, branches=True).get(0)
        want = fn(n, p, U)
        if got is None or any(abs(a - b) > 1e-5 * max(1.0, abs(b)) for a, b in zip(got, want)):
            wrong += 1
            if wrong == 1: fails_detail.append('n=%s got %s want %s' % (n, got, want))
    return wrong

for name, src, fn in ROWS:
    fails_detail = []
    p, o = compile_one(name, src)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d): %s' % (name, p.returncode, (p.stdout + p.stderr).strip().splitlines()[-1:]))
        print('  %-22s REFUSED' % name); continue
    try:
        wrong = judge(o.read_bytes(), fn, 11)
    except AssertionError as e:
        fails.append('%s: instrument refused the program: %s' % (name, e)); print('  %-22s UNJUDGED' % name); continue
    if wrong: fails.append('%s: %d of %d wrong; first %s' % (name, wrong, len(NS), fails_detail[0]))
    print('  %-22s %s' % (name, 'ok' if not wrong else '%d WRONG' % wrong))

# Instrument control: our own sum container with the back-edge never taken
# must be judged wrong.
p, o = compile_one('control', ROWS[0][1])
if p.returncode == 0 and o.exists():
    b = bytearray(o.read_bytes()); off, size = struct.unpack_from('>2I', b, 24)[::-1]
    flipped = 0
    for pos in range(off, off + size, 16):
        w0, w1 = struct.unpack_from('>2I', b, pos)
        if (w1 >> 27) & 31 == 9 and (w0 >> 10) & 7 == 7:      # BRA TR: the back-edge
            struct.pack_into('>I', b, pos, w0 & ~(7 << 10)); flipped += 1
    fails_detail = []
    caught = flipped == 1 and judge(bytes(b), e_sum, 11) > 0
    print('  %-22s %s' % ('control_no_backedge', 'caught' if caught else 'NOT CAUGHT (%d flipped)' % flipped))
    if not caught: fails.append('instrument control: a never-taken back-edge was not caught')
else:
    fails.append('control: sum row refused')

REFUSE = [
    ('nested', M + 'float4 s = p; for (int i = 0; i < n; ++i) for (int q = 0; q < n; ++q) s += k[q]; return s; }'),
    ('induction_write', M + 'float4 s = p; for (int i = 0; i < n; ++i) { s += k[i]; i += 1; } return s; }'),
    ('return_in_body', M + 'float4 s = p; for (int i = 0; i < n; ++i) { s += k[i]; if (s.x > 3) return s; } return s; }'),
    ('break_in_body', M + 'float4 s = p; for (int i = 0; i < n; ++i) { s += k[i]; if (s.x > 3) break; } return s; }'),
    ('array_write', M + 'float4 a[2]; a[0] = p; a[1] = p; for (int i = 0; i < n; ++i) a[0] += k[i]; return a[0]; }'),
]
for name, src in REFUSE:
    p, o = compile_one(name, src)
    ok = p.returncode == 1 and not o.exists()
    print('  %-22s %s' % (name, 'refused' if ok else 'NOT refused (rc %d)' % p.returncode))
    if not ok: fails.append(name + ': expected a refusal (rc 1) and no container')

for f in fails: print('vp-runtime-loop: FAIL', f)
print('vp-runtime-loop: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
