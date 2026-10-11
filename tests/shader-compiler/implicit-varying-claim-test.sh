#!/usr/bin/env bash
# An implicit (semantic-less) fragment input that flows into the RETURN
# VALUE of an inlined call claims its TEXCOORD even when that call's result
# is dead, and every later implicit input moves up one (t_8c4a982e).
# Measured on the reference: a user helper, and standard-library functions
# implemented as inlined source (sin, lerp, clamp, mul, ...), claim; the
# hardware primitives (dot, abs, pow, frac, saturate, tex2D, ...) do not,
# nor does dead code inside the entry itself, nor a helper that ignores the
# parameter.  A claimed input's record stays unreferenced.
#
# Usage: tests/shader-compiler/implicit-varying-claim-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "implicit-varying-claim: FAIL: compiler required"; exit 1; }
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
TEX0, TEX1, UNDEF = 0xc94, 0xc95, 0xcb8
CLAIM = {'b': (TEX0, 0), 'f': (TEX1, 1)}
NO_CLAIM = {'b': (UNDEF, 0), 'f': (TEX0, 1)}
MAIN = 'float4 main(float2 b, float f) : COLOR { %s return float4(f, 0, 0, 1); }\n'
H = 'float2 h(float2 x) { return x * 3.0; }\n'
ROWS = [
    ('helper_dead',        H + MAIN % 'float2 d = h(b);', CLAIM),
    ('helper_dead_store',  H + MAIN % 'float2 d = h(b); float2 e = d;', CLAIM),
    ('helper_nested',      'float2 g(float2 x) { return x + 1.0; }\n' + H + MAIN % 'float2 d = h(g(b));', CLAIM),
    ('helper_lane',        'float k(float x) { return x * 2.0; }\n' + MAIN % 'float d = k(b.x);', CLAIM),
    ('helper_ignores',     'float2 z(float2 x) { return float2(1, 2); }\n' + MAIN % 'float2 d = z(b);', NO_CLAIM),
    ('local_dead',         MAIN % 'float2 d = b * 2.0;', NO_CLAIM),
    ('struct_dead',        'struct S { float2 v; };\n' + MAIN % 'S s; s.v = b;', NO_CLAIM),
    ('sin_dead',           MAIN % 'float2 d = sin(b);', CLAIM),
    ('lerp_dead',          MAIN % 'float2 d = lerp(b, float2(1.0, 1.0), 0.5);', CLAIM),
    ('clamp_dead',         MAIN % 'float2 d = clamp(b, 0.0, 1.0);', CLAIM),
    ('normalize_dead',     MAIN % 'float2 d = normalize(b);', CLAIM),
    ('dot_dead',           MAIN % 'float d = dot(b, b);', NO_CLAIM),
    ('pow_dead',           MAIN % 'float2 d = pow(b, float2(2.0, 2.0));', NO_CLAIM),
    ('frac_dead',          MAIN % 'float2 d = frac(b);', NO_CLAIM),
    ('saturate_dead',      MAIN % 'float2 d = saturate(b);', NO_CLAIM),
    ('read_both',          MAIN % 'f = f + b.x;', {'b': (TEX0, 1), 'f': (TEX1, 1)}),
]
fails = []
for name, source, want in ROWS:
    src, out = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(source)
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '-e', 'main', '--emit-container', str(out), str(src)],
                       capture_output=True, text=True, timeout=60)
    if p.returncode != 0 or not out.is_file() or out.stat().st_size == 0:
        fails.append('%s: refused (rc %d)' % (name, p.returncode)); continue
    blob = out.read_bytes(); got = {}
    count, table = struct.unpack_from('>2I', blob, 12)
    for i in range(count):
        w = struct.unpack_from('>12I', blob, table + 48 * i)
        rec = blob[w[4]:blob.index(0, w[4])].decode()
        if rec in want: got[rec] = (w[1], w[10])
    print('  %-20s %s' % (name, 'ok' if got == want else 'GOT %s' % {k: (hex(v[0]), v[1]) for k, v in got.items()}))
    if got != want: fails.append('%s: %s, want %s' % (name, got, want))
for f in fails: print('implicit-varying-claim: FAIL', f)
print('implicit-varying-claim: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
