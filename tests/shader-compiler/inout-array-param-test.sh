#!/usr/bin/env bash
# An INOUT ARRAY parameter of an inlined helper (2xbrz / 3xbrz's
# `void ScalePixel(const int4 blend, const float3 k[9], inout float3 dst[4])`):
# the caller's local array is copied in, and the helper's final elements are
# copied back - elements it never writes keep the caller's values.  Judged by
# value.  A swizzle write into an array element (`d[0].y = 7;`) reads the
# element, inserts the lanes and stores it back.  An OUT array, the same array
# passed to two inout array parameters, and a file-scope array argument stay
# refused by name.
#
# Usage: tests/shader-compiler/inout-array-param-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "inout-array-param: FAIL: compiler required"; exit 1; }
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
SEED = 'float3 dst[2]; dst[0] = t.xyz; dst[1] = t.yzw; '
def lerp3(a, b, w): return [f32(x + f32(w * f32(y - x))) for x, y in zip(a, b)]
VALUE = [
    ('write_one_read_other',
     'void sp(const float k[3], inout float3 dst[2]) { dst[1] = lerp(dst[1], float3(k[0], k[1], k[2]), 0.5); }' + NL +
     M + 'float k[3] = { t.x, t.y, t.z }; ' + SEED + 'sp(k, dst); return float4(dst[0].x, dst[1]); }',
     lambda t: [t[0]] + lerp3(t[1:4], t[0:3], 0.5)),
    ('untouched_elements',
     'void sp(inout float3 dst[2]) { dst[0].y = 7.0; }' + NL +
     M + SEED + 'sp(dst); return float4(dst[0].xy, dst[1].xz); }',
     lambda t: [t[0], 7.0, t[1], t[3]]),
    ('static_loop_over_elements',
     'void sc(inout float3 d[2], float k) { for (int j = 0; j < 2; j++) d[j] = d[j] * k; }' + NL +
     M + SEED + 'sc(dst, t.w); return float4(dst[0].x, dst[1].yz, dst[0].z); }',
     lambda t: [f32(t[0] * t[3]), f32(t[2] * t[3]), f32(t[3] * t[3]), f32(t[2] * t[3])]),
    ('passed_on',
     'void inner(inout float3 d[2]) { d[1].x = d[0].x + 1.0; }' + NL +
     'void outer(inout float3 d[2]) { inner(d); d[0].x = 3.0; }' + NL +
     M + SEED + 'outer(dst); return float4(dst[0].x, dst[1].x, dst[1].y, 1); }',
     lambda t: [3.0, f32(t[0] + 1.0), t[2], 1.0]),
    ('two_calls',
     'void bump(inout float3 d[2]) { d[0] = d[0] + d[1]; }' + NL +
     M + SEED + 'bump(dst); bump(dst); return float4(dst[0], 1); }',
     lambda t: [f32(f32(t[0] + t[1]) + t[1]), f32(f32(t[1] + t[2]) + t[2]), f32(f32(t[2] + t[3]) + t[3]), 1.0]),
    # A helper writing its own IN array parameter writes its copy: the
    # caller's array is unchanged and a second call sees the caller's
    # values again (measured on the reference: h(a) twice = 12, 12).
    ('in_array_param_written',
     'float h(float v[3]) { v[0] = 10.0; return v[0] + v[1]; }' + NL +
     M + 'float a[3] = { 1.0, 2.0, 3.0 }; float x = h(a); float y = h(a); return float4(x, y, a[0], t.x); }',
     lambda t: [12.0, 12.0, 1.0, t[0]]),
    # A swizzle write into an element of a local array (needed by the
    # helpers above, and on its own).
    ('element_lane_write',
     M + 'float3 a[2]; a[0] = t.xyz; a[1] = t.yzw; a[0].y = 7.0; return float4(a[0].xy, a[1].xz); }',
     lambda t: [t[0], 7.0, t[1], t[3]]),
    ('element_two_lane_write',
     M + 'float3 a[2]; a[0] = t.xyz; a[1] = t.yzw; a[1].xz = float2(t.x * 2.0, 5.0); return float4(a[1], a[0].z); }',
     lambda t: [f32(t[0] * 2.0), t[2], 5.0, t[2]]),
]
REFUSE = [
    ('out_array',
     'void sp(out float3 d[2]) { d[0] = float3(1, 2, 3); d[1] = float3(4, 5, 6); }' + NL +
     M + 'float3 dst[2]; sp(dst); return float4(dst[0], 1); }', 'out/inout parameter'),
    ('same_array_twice',
     'void sp(inout float3 a[2], inout float3 b[2]) { a[0] = b[1]; }' + NL +
     M + SEED + 'sp(dst, dst); return float4(dst[0], 1); }', 'two inout array parameters'),
    ('file_scope_array',
     'static float3 g[2] = { float3(1, 2, 3), float3(4, 5, 6) };' + NL +
     'void sp(inout float3 d[2]) { d[0] = d[1]; }' + NL +
     M + 'sp(g); return float4(g[0], 1); }', 'out/inout parameter'),
]
TS = [[0.25, -0.5, 1.5, 2.0], [-1.0, 0.75, 0.125, -3.0], [3.0, 0.0, -2.5, 0.5]]
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
        fails.append('%s: refused (rc %d)' % (name, p.returncode)); print('  %-26s REFUSED' % name); continue
    wrong = 0
    for t in TS:
        t = [f32(x) for x in t]
        got = fp_eval.evaluate(o.read_bytes(), {'TEX0': t}); want = [f32(x) for x in want_fn(t)]
        if got != want:
            fails.append('%s at t=%s: got %s want %s' % (name, t, got, want)); wrong += 1
    print('  %-26s %s' % (name, 'ok' if not wrong else '%d WRONG' % wrong))
for name, src, text in REFUSE:
    p, o = compile_one(name, src)
    ok = p.returncode == 1 and not o.exists() and text in (p.stdout + p.stderr)
    print('  %-26s %s' % (name, 'refused' if ok else 'NOT refused as expected (rc %d)' % p.returncode))
    if not ok: fails.append('%s: expected exit 1 naming %r and no container' % (name, text))
for f in fails: print('inout-array-param: FAIL', f)
print('inout-array-param: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
