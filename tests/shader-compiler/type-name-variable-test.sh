#!/usr/bin/env bash
# A built-in type spelling used as a VARIABLE name (PSGL's vpFixedPipeline:
# `float3 half = float3(0,0,0); ... dot(normal, half)`).  Measured on the
# reference: every built-in type name except the reserved words float, int
# and bool may name a variable (half, fixed, the floatN / halfN / fixedN /
# intN / boolN vectors and matrices, the samplers), and from its declaration
# to the end of its scope (a for initialiser's: the whole for statement) the
# spelling is that variable, a later declarator of a list included.  Judged
# by value; the reserved words stay refused.
#
# Usage: tests/shader-compiler/type-name-variable-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "type-name-variable: FAIL: compiler required"; exit 1; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" "$here" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
import fp_eval
f32 = lambda v: struct.unpack('>f', struct.pack('>f', v))[0]
NL = chr(10)
M = 'float4 main(float4 t : TEXCOORD0) : COLOR { '
VALUE = [
    ('half_local', M + 'float3 half = t.xyz * 2.0; return float4(half, 1); }',
     lambda t: [f32(t[0] * 2.0), f32(t[1] * 2.0), f32(t[2] * 2.0), 1.0]),
    ('fixed_and_half2', M + 'float fixed = t.x; float2 half2 = t.zw; return float4(fixed, half2, 0); }',
     lambda t: [t[0], t[2], t[3], 0.0]),
    ('float2_local', M + 'float float2 = t.y + 1.0; return float4(float2, 0, 0, 1); }',
     lambda t: [f32(t[1] + 1.0), 0.0, 0.0, 1.0]),
    ('parameter', 'float g(float half) { return half * 3.0; }' + NL + M + 'return float4(g(t.x), 0, 0, 1); }',
     lambda t: [f32(t[0] * 3.0), 0.0, 0.0, 1.0]),
    # Only the declaring scope: after the block, half is the type again.
    ('block_scope', M + 'float s = 0; { float half = t.x; s = half; } half h = half(t.y); return float4(s, h, 0, 1); }',
     lambda t: [t[0], t[1], 0.0, 1.0]),
    # A for statement's initialiser: its scope is the whole statement, an
    # if/else body included; after the loop the spelling is the type again.
    ('for_init_if_else', M + 'float s = 0; for (float half = 0; half < 2; half += 1) '
                         'if (t.x > 0) { s += half; } else { s += 2 * half; } half q = half(t.y); return float4(s, q, 0, 1); }',
     lambda t: [1.0 if t[0] > 0 else 2.0, t[1], 0.0, 1.0]),
    # A later declarator of a declaration list.
    ('comma_declarator', M + 'float s = t.y, half = t.x; return float4(s, half, 0, 1); }',
     lambda t: [t[1], t[0], 0.0, 1.0]),
]
REFUSE = [
    ('float_reserved', M + 'float float = t.x; return float4(float, 0, 0, 1); }'),
    ('int_reserved', M + 'float int = t.x; return float4(int, 0, 0, 1); }'),
    ('bool_reserved', M + 'float bool = t.x; return float4(bool, 0, 0, 1); }'),
    # A shadowed vector spelling is the variable: calling it is an error.
    ('shadowed_constructor_call', M + 'float float4 = t.x; return float4(1, 0, 0, 1); }'),
]
TS = [[0.25, -0.5, 1.5, 2.0], [-1.0, 0.75, 0.125, -3.0]]
fails = []
def compile_one(name, src):
    s, o = work / (name + '.cg'), work / (name + '.bin')
    s.write_text(src + NL)
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '-e', 'main', '--emit-container', str(o), str(s)],
                       capture_output=True, text=True, timeout=60)
    return p, o
for name, src, want_fn in VALUE:
    p, o = compile_one(name, src)
    if p.returncode != 0 or not o.exists():
        fails.append('%s: refused (rc %d): %s' % (name, p.returncode, (p.stdout + p.stderr).strip().splitlines()[-1:]))
        print('  %-26s REFUSED' % name); continue
    wrong = 0
    for t in TS:
        t = [f32(x) for x in t]
        got = fp_eval.evaluate(o.read_bytes(), {'TEX0': t}); want = [f32(x) for x in want_fn(t)]
        if got != want:
            fails.append('%s at t=%s: got %s want %s' % (name, t, got, want)); wrong += 1
    print('  %-26s %s' % (name, 'ok' if not wrong else '%d WRONG' % wrong))
for name, src in REFUSE:
    p, o = compile_one(name, src)
    ok = p.returncode == 1 and not o.exists()   # a refusal exits 1; a crash (134, -6) is not one
    print('  %-26s %s' % (name, 'refused' if ok else 'NOT refused (rc %d)' % p.returncode))
    if not ok: fails.append(name + ': expected a refusal and no container')
for f in fails: print('type-name-variable: FAIL', f)
print('type-name-variable: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
