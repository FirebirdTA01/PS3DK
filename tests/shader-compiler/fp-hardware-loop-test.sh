#!/usr/bin/env bash
# FP-LOOP slice 1: a fragment `for` the static expansion cannot prove (a
# run-time bound) compiles to a hardware loop - LOOP {255, 0, 1}, the source
# condition tested at the head of each iteration with a BRK where it fails,
# the shape the reference emits for a run-time bound (measured).  Judged on
# VALUES by fp_eval's LOOP/BRK model against the source semantics, with the
# 255-trip cap the measured shape implies (bounds 254, 255, 256 and 300 are
# the boundary controls) and zero-trip loops.
#
# Carried variables keep their value across iterations and after the loop;
# the latch is a parallel assignment (the swap row); a variable the body
# writes before reading is local to an iteration; loop invariants stay live.
# An if / else (and else-if chains) in the body is built flat - both arms,
# then a select per variable an arm assigned - since the region has no
# branches; an arm with an effect (discard, return) is refused, and a
# variable only one arm assigns that had no value before the loop is refused
# (the register holds the previous iteration's value, not the zero a
# straight-line program reads).  Outside the admitted shape the program is
# refused by name: a break or a nested unexpanded loop in the body, a variable
# read in the body before it has a value, an iteration-local variable read
# after the loop, and writes to an out parameter or a file-scope variable.
#
# Usage: tests/shader-compiler/fp-hardware-loop-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "fp-hardware-loop: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import fp_eval
from fp_sources import instructions, ucode_words
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
H = 'float4 main(float4 t : TEXCOORD0) : COLOR { '

def run_loop(start, bound, body, carries, cmp='<', step=1.0, cap=255):
    i = f32(start); trips = 0
    while trips < cap:
        if not {'<': i < bound, '<=': i <= bound, '>': i > bound, '>=': i >= bound}[cmp]:
            break
        carries = body(i, carries); i = f32(i + step); trips += 1
    return i, carries

def sum_(t):
    i, (s,) = run_loop(0.0, t[0], lambda i, c: (f32(c[0] + t[1]),), (0.0,))
    return [s, i, 0.0, 1.0]
def swap(t):
    i, (a, b) = run_loop(0.0, t[2], lambda i, c: (c[1], c[0]), (t[0], t[1]))
    return [a, b, i, 1.0]
def invariant(t):
    k = f32(t[3] * 2.0)
    i, (s,) = run_loop(-1.0, t[0], lambda i, c: (f32(c[0] + f32(k * i)),), (0.0,))
    return [s, k, i, 1.0]
def vector_carry(t):
    def body(i, c):
        v = c[0]
        return ([f32(v[0] + t[1]), f32(v[1] + f32(i * t[2])), f32(v[2] + 1.0)],)
    i, (v,) = run_loop(0.0, t[0], body, ([0.0, 0.0, 0.0],))
    return [v[0], v[1], v[2], 1.0]
def iteration_local(t):
    def body(i, c):
        x = f32(t[1] - i)
        return (f32(c[0] + f32(x * x)),)
    i, (s,) = run_loop(0.0, t[0], body, (0.0,))
    return [s, 0.0, 0.0, 1.0]
def decrement(t):
    i, (s,) = run_loop(t[0], 0.0, lambda i, c: (f32(c[0] + i),), (0.0,), cmp='>', step=-1.0)
    return [s, i, 0.0, 1.0]
def less_equal_step(t):
    i, (s,) = run_loop(0.0, t[0], lambda i, c: (f32(c[0] + 1.0),), (0.0,), cmp='<=', step=2.0)
    return [s, i, 0.0, 1.0]
def if_count(t):
    i, (s,) = run_loop(0.0, t[0], lambda i, c: (f32(c[0] + 1.0) if i > 2 else c[0],), (0.0,))
    return [s, 0.0, 0.0, 1.0]
def if_else(t):
    def body(i, c):
        a, b = c
        if i < t[1]: a = f32(a + i)
        else: b = f32(b + f32(i * 2.0))
        return (a, b)
    i, (a, b) = run_loop(0.0, t[0], body, (0.0, 0.0))
    return [a, b, i, 1.0]
def else_if(t):
    def body(i, c):
        s, = c
        if i < 1: s = f32(s + 1.0)
        elif i < 3: s = f32(s + 10.0)
        else: s = f32(s + 100.0)
        return (s,)
    i, (s,) = run_loop(0.0, t[0], body, (0.0,))
    return [s, 0.0, 0.0, 1.0]
def swap_if(t):
    def body(i, c):
        a, b = c
        if i > t[2]: a, b = b, a
        return (a, b)
    i, (a, b) = run_loop(0.0, t[0], body, (t[1], t[3]))
    return [a, b, 0.0, 1.0]
def sequential(t):
    _, (a,) = run_loop(0.0, t[0], lambda i, c: (f32(c[0] + t[1]),), (0.0,))
    _, (b,) = run_loop(0.0, t[2], lambda i, c: (f32(c[0] + t[3]),), (0.0,))
    return [a, b, 0.0, 1.0]
def assign_invariant(t):
    _, (a,) = run_loop(0.0, t[0], lambda i, c: (t[1],), (7.0,))
    return [a, t[1], 0.0, 1.0]
def helper_body(t):
    i, (s,) = run_loop(0.0, t[0], lambda i, c: (f32(c[0] + f32(i * t[2])),), (0.0,))
    return [s, 0.0, 0.0, 1.0]

VALUE = [
    ('sum', H + 'float s = 0; float i; for (i = 0; i < t.x; i++) s += t.y; return float4(s, i, 0, 1); }', sum_),
    ('swap', H + 'float a = t.x, b = t.y; float i; for (i = 0; i < t.z; i++) { float tmp = a; a = b; b = tmp; } '
             'return float4(a, b, i, 1); }', swap),
    ('invariant', H + 'float k = t.w * 2.0; float s = 0; float i; for (i = -1.0; i < t.x; i++) s += k * i; '
                  'return float4(s, k, i, 1); }', invariant),
    ('vector_carry', H + 'float3 v = float3(0, 0, 0); for (float i = 0; i < t.x; i++) { v.x += t.y; v.y += i * t.z; '
                     'v.z += 1.0; } return float4(v, 1); }', vector_carry),
    ('iteration_local', H + 'float x; float s = 0; for (float i = 0; i < t.x; i++) { x = t.y - i; s += x * x; } '
                        'return float4(s, 0, 0, 1); }', iteration_local),
    ('decrement', H + 'float s = 0; float i; for (i = t.x; i > 0; i--) s += i; return float4(s, i, 0, 1); }', decrement),
    ('less_equal_step', H + 'float s = 0; float i; for (i = 0; i <= t.x; i += 2.0) s += 1.0; return float4(s, i, 0, 1); }',
     less_equal_step),
    # The loop inside an inlined helper (the community witnesses' shape).
    ('in_helper', 'float g(float i, float k) { return i * k; }' + chr(10) +
                  'float acc(float4 t) { float s = 0; float i; for (i = 0; i < t.x; i++) s += g(i, t.z); return s; }' + chr(10) +
                  H + 'return float4(acc(t), 0, 0, 1); }', helper_body),
    ('if_count', H + 'float s = 0; for (float i = 0; i < t.x; i++) { if (i > 2) s += 1; } return float4(s, 0, 0, 1); }',
     if_count),
    ('if_else', H + 'float a = 0, b = 0; float i; for (i = 0; i < t.x; i++) { if (i < t.y) a += i; else b += i * 2.0; } '
                'return float4(a, b, i, 1); }', if_else),
    ('else_if', H + 'float s = 0; for (float i = 0; i < t.x; i++) { if (i < 1) s += 1; else if (i < 3) s += 10; '
                'else s += 100; } return float4(s, 0, 0, 1); }', else_if),
    # Two loops in one block: each keeps its own carries (review: codex, LOOP2).
    ('sequential', H + 'float a = 0, b = 0; for (float i = 0; i < t.x; i++) a += t.y; '
                   'for (float j = 0; j < t.z; j++) b += t.w; return float4(a, b, 0, 1); }', sequential),
    # A carried variable takes an unchanged one's value: the latch reads the
    # unchanged value, not a dropped carry (review: codex, LOOP3).
    ('assign_invariant', H + 'float a = 7, b = t.y; for (float i = 0; i < t.x; i++) a = b; '
                         'return float4(a, b, 0, 1); }', assign_invariant),
    # A swap under a condition: the select join, then the parallel latch.
    ('swap_if', H + 'float a = t.y, b = t.w; for (float i = 0; i < t.x; i++) { if (i > t.z) { float k = a; a = b; b = k; } } '
                'return float4(a, b, 0, 1); }', swap_if),
]
REFUSE = [
    ('local_read_after_loop', H + 'float x; float s = 0; for (float i = 0; i < t.x; i++) { x = t.y - i; s += x; } '
                              'return float4(s, x, 0, 1); }', 'not carried'),
    ('one_arm_without_value', H + 'float x; float s = 0; for (float i = 0; i < t.x; i++) { if (i > 1) x = i; s += x; } '
                              'return float4(s, 0, 0, 1); }', 'no initialiser'),
    ('discard_in_arm', H + 'float s = 0; for (float i = 0; i < t.x; i++) { if (i > 2) discard; s += 1; } '
                       'return float4(s, 0, 0, 1); }', "the loop body contains 'discard'"),
    ('read_before_value', H + 'float s; for (float i = 0; i < t.x; i++) { s = s + 1; } return float4(s, 0, 0, 1); }',
     'no initialiser'),
    ('nested_runtime_loop', H + 'float s = 0; for (float i = 0; i < t.x; i++) { for (float j = 0; j < t.y; j++) s += 1; } '
                            'return float4(s, 0, 0, 1); }', 'error'),
    # The exit test runs before the latch: a condition that assigns would
    # lose that write at the exit (review: codex, LOOP1).
    ('condition_assigns', H + 'float n = t.x; float s = 0; for (float i = 0; i < (n = n - 1); i++) s += 1; '
                          'return float4(s, n, 0, 1); }', 'the loop condition assigns a variable'),
    # A constant trip count over 255 needs the nested loop (not implemented):
    # LOOP {255} would cut it short.
    ('constant_over_255', H + 'float s = 0; for (float i = 0; i < 300; i += 0.5) s += t.y; return float4(s, 0, 0, 1); }',
     'more than 255 iterations'),
    # A constant bound the guard must evaluate, not a literal (review:
    # codex LOOP4): abs(-300.0) with step 0.5 is 600 iterations.
    ('constant_expression_over_255', H + 'float s = 0; for (float i = 0; i < abs(-300.0); i += 0.5) s += t.y; '
                                     'return float4(s, 0, 0, 1); }', 'more than 255 iterations'),
    # A constant bound behind a 40-level shared DAG (k[n] = min(k[n-1], k[n-1]))
    # is classified once per value, not once per path (review: codex,
    # LOOP5-DAG): a named refusal well inside the subprocess timeout.
    ('constant_dag_over_255', H.replace(' { ', ' { float k0 = abs(-300.0); ' +
                              ''.join('float k%d = min(k%d, k%d); ' % (n, n - 1, n - 1) for n in range(1, 41))) +
                              'float s = 0; for (float i = 0; i < k40; i += 0.5) s += t.y; return float4(s, 0, 0, 1); }',
     'more than 255 iterations'),
    ('constant_bound_induction_write', H + 'float s = 0; for (float i = 0.0; i < 4.0; i += 0.5) { s += t.y; i += 0.5; } '
                                       'return float4(s, 0, 0, 1); }', 'writes its induction variable'),
    ('out_parameter', 'void f(float4 t, out float r) { r = 0; for (float i = 0; i < t.x; i++) r += 1; }' + chr(10) +
                      H + 'float r; f(t, r); return float4(r, 0, 0, 1); }', 'out parameter'),
    ('file_scope_variable', 'static float g = 0;' + chr(10) +
                            H + 'for (float i = 0; i < t.x; i++) g += 1; return float4(g, 0, 0, 1); }', 'file-scope variable'),
]
TS = [[3.0, 0.5, 2.0, 0.25], [0.0, 1.5, 1.0, 1.0], [-2.0, 0.25, 3.0, -1.0], [2.5, -1.0, 0.5, 3.0],
      [1.0, 2.0, 4.0, 0.5], [7.0, 0.125, 5.0, 2.0], [254.0, 1.0, 1.0, 1.0], [255.0, 1.0, 1.0, 1.0],
      [256.0, 1.0, 1.0, 1.0], [300.0, 1.0, 1.0, 1.0]]
fails = []
def compile_one(name, src):
    s, o = work / (name + '.cg'), work / (name + '.bin')
    s.write_text(src + chr(10))
    try:
        p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                           capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        p = subprocess.CompletedProcess([], 124, '', 'TIMEOUT after 60 s')
    return p, o
for name, src, want_fn in VALUE:
    p, o = compile_one(name, src)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d)' % (name, p.returncode)); print('  %-22s REFUSED' % name); continue
    blob = o.read_bytes()
    # The shape: one LOOP {255, 0, 1} and one BRK per source loop.
    loops = brks = 0
    for w, _ in instructions(ucode_words(blob)):
        if (w[2] >> 31) & 1:
            op = (w[0] >> 24) & 0x3F
            if op == 0x03 and (w[2] >> 2) & 0xFF == 255 and (w[2] >> 10) & 0xFF == 0 and (w[2] >> 19) & 0xFF == 1:
                loops += 1
            elif op == 0x00:
                brks += 1
    want_loops = 2 if name == 'sequential' else 1
    if (loops, brks) != (want_loops, want_loops):
        fails.append('%s: %d LOOP {255,0,1} and %d BRK, want %d of each' % (name, loops, brks, want_loops))
    wrong = 0
    for t in TS:
        t = [f32(x) for x in t]
        try:
            got = fp_eval.evaluate(blob, {'TEX0': t})
        except fp_eval.Unmodelled as e:
            fails.append('%s at t=%s: unjudged: %s' % (name, t, e)); wrong += 1; continue
        want = [f32(x) for x in want_fn(t)]
        if any(not (a == b or abs(a - b) <= 1e-5 * max(1.0, abs(b))) for a, b in zip(got, want)):
            fails.append('%s at t=%s: got %s want %s' % (name, t, got, want)); wrong += 1
    print('  %-22s %s' % (name, 'ok' if not wrong else '%d WRONG' % wrong))
for name, src, text in REFUSE:
    p, o = compile_one(name, src)
    ok = p.returncode == 1 and not o.exists() and text in (p.stdout + p.stderr)
    print('  %-22s %s' % (name, 'refused' if ok else 'NOT refused as expected (rc %d)' % p.returncode))
    if not ok: fails.append('%s: expected exit 1 naming %r and no container' % (name, text))
for f in fails: print('fp-hardware-loop: FAIL', f)
print('fp-hardware-loop: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
