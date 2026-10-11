#!/usr/bin/env bash
# `: TEXUNITn` on a struct-typed entry parameter (libretro's mdapt passes:
# `prev PASSPREV4 : TEXUNIT1`).  Measured on the reference, both profiles:
# every member record carries the struct's semantic string; a sampler member
# binds to unit n (two samplers in one struct BOTH take unit n); a numeric
# member of a varying struct stays an ordinary varying - unreferenced with
# no resource when unread, the lowest free TEXCOORD when read; a UNIFORM
# struct with a numeric member is refused (C5118), a sampler-only one is
# accepted.  TEXCOORDn on a struct and nested structs stay named refusals.
#
# Usage: tests/shader-compiler/struct-param-semantic-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "struct-param-semantic: FAIL: compiler required"; exit 1; }
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
python3 - "$compiler" "$work" <<'PY'
import struct, subprocess, sys
from pathlib import Path
compiler, work = sys.argv[1], Path(sys.argv[2])
fails = []

def compile_one(name, profile, source):
    src, out = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(source)
    if out.exists(): out.unlink()
    p = subprocess.run([compiler, '-p', profile, '-e', 'main', '--emit-container', str(out), str(src)],
                       capture_output=True, text=True, timeout=60)
    ok = p.returncode == 0 and out.is_file() and out.stat().st_size > 0
    return p, (out.read_bytes() if ok else None)

def records(blob):
    count, table = struct.unpack_from('>2I', blob, 12); out = {}
    for i in range(count):
        w = struct.unpack_from('>12I', blob, table + 48 * i)
        name = blob[w[4]:blob.index(0, w[4])].decode()
        sem = blob[w[7]:blob.index(0, w[7])].decode() if w[7] else ''
        out[name] = (w[0], w[1], w[2], sem, w[10])   # type, res, var, semantic, referenced
    return out

VARYING, UNIFORM = 4101, 4102
S1 = 'struct S1 { float2 a; sampler2D s; };\n'
S3 = 'struct S3 { sampler2D s1; sampler2D s2; };\n'
S4 = 'struct S4 { float2 a; float4 b; };\n'
FP = 'float4 main(%s p : TEXUNIT2, float2 uv : TEXCOORD0) : COLOR { %s }\n'
VP = 'float4 main(%s p : TEXUNIT2, float2 uv : TEXCOORD0) : POSITION { %s }\n'
ACCEPT = [
    # name, profile, source, {record: (type, res, var, sem, ref)}
    ('fp_sampler_only_read', 'sce_fp_rsx', S1 + FP % ('S1', 'return tex2D(p.s, uv);'),
     {'p.a': (1046, 0xcb8, VARYING, 'TEXUNIT2', 0), 'p.s': (1066, 0x802, UNIFORM, 'TEXUNIT2', 1)}),
    ('fp_numeric_read', 'sce_fp_rsx', S1 + FP % ('S1', 'return tex2D(p.s, uv + p.a);'),
     {'p.a': (1046, 0xc95, VARYING, 'TEXUNIT2', 1), 'p.s': (1066, 0x802, UNIFORM, 'TEXUNIT2', 1)}),
    ('fp_two_samplers_one_unit', 'sce_fp_rsx', S3 + FP % ('S3', 'return tex2D(p.s1, uv) + tex2D(p.s2, uv);'),
     {'p.s1': (1066, 0x802, UNIFORM, 'TEXUNIT2', 1), 'p.s2': (1066, 0x802, UNIFORM, 'TEXUNIT2', 1)}),
    ('fp_no_sampler', 'sce_fp_rsx', S4 + FP % ('S4', 'return float4(p.a, uv) + p.b;'),
     {'p.a': (1046, 0xc95, VARYING, 'TEXUNIT2', 1), 'p.b': (1048, 0xc96, VARYING, 'TEXUNIT2', 1)}),
    ('fp_uniform_sampler_only', 'sce_fp_rsx',
     'struct S6 { sampler2D s; };\n' + FP % ('uniform S6', 'return tex2D(p.s, uv);'),
     {'p.s': (1066, 0x802, UNIFORM, 'TEXUNIT2', 1)}),
    ('vp_sampler_only_read', 'sce_vp_rsx', S1 + VP % ('S1', 'return tex2Dlod(p.s, float4(uv, 0, 0));'),
     {'p.a': (1046, 0xcb8, VARYING, 'TEXUNIT2', 0), 'p.s': (1066, 0x802, UNIFORM, 'TEXUNIT2', 1)}),
]
for name, profile, source, want in ACCEPT:
    p, blob = compile_one(name, profile, source)
    if blob is None:
        fails.append('%s: refused (rc %d): %s' % (name, p.returncode, (p.stderr + p.stdout).strip()[-160:])); continue
    got = {k: v for k, v in records(blob).items() if k in want}
    print('  %-26s %s' % (name, 'records ok' if got == want else 'RECORDS %s' % got))
    if got != want: fails.append('%s: records %s, want %s' % (name, got, want))

REFUSE = [
    ('fp_uniform_numeric_C5118', 'struct S6 { float2 a; sampler2D s; };\n' +
     FP % ('uniform S6', 'return tex2D(p.s, uv + p.a);'), 'C5118'),
    ('fp_texcoord_on_struct', 'struct S5 { float2 a; float2 b; };\n'
     'float4 main(S5 p : TEXCOORD1) : COLOR { return float4(p.a, p.b); }\n', 'struct-typed parameters'),
    ('fp_nested_struct', 'struct I { float2 a; sampler2D s; };\nstruct O { float3 b; I inner; };\n' +
     FP % ('O', 'return tex2D(p.inner.s, uv);'), 'struct-typed parameters'),
    # The reference WRITES an out/inout struct's members to varying outputs;
    # not lowered here, so refused rather than compiled with them dropped.
    ('fp_out_struct', 'struct S { float4 c; };' + chr(10) + 'void main(float2 uv : TEXCOORD0, out S p : TEXUNIT2, '
     'out float4 col : COLOR) { p.c = float4(uv, 0, 1); col = float4(uv, 1, 1); }' + chr(10), 'out/inout struct'),
    ('fp_inout_struct', 'struct S { float4 c; };' + chr(10) + 'void main(float2 uv : TEXCOORD0, inout S p : TEXUNIT2, '
     'out float4 col : COLOR) { p.c = float4(uv, 0, 1); col = float4(uv, 1, 1); }' + chr(10), 'out/inout struct'),
]
VP_REFUSE = [
    ('vp_out_struct', 'struct S { float4 c; };' + chr(10) + 'void main(float4 pos : POSITION, out S p : TEXUNIT2, '
     'out float4 opos : POSITION) { p.c = pos * 2; opos = pos; }' + chr(10), 'out/inout struct'),
    ('vp_inout_struct', 'struct S { float4 c; };' + chr(10) + 'void main(float4 pos : POSITION, inout S p : TEXUNIT2, '
     'out float4 opos : POSITION) { p.c = pos * 2; opos = pos; }' + chr(10), 'out/inout struct'),
]
for name, source, text, profile in [r + ('sce_fp_rsx',) for r in REFUSE] + [r + ('sce_vp_rsx',) for r in VP_REFUSE]:
    p, blob = compile_one(name, profile, source)
    ok = p.returncode == 1 and blob is None and text in (p.stderr + p.stdout)
    print('  %-26s %s' % (name, 'refused' if ok else 'NOT a named refusal (rc %d)' % p.returncode))
    if not ok: fails.append('%s: expected exit 1 naming %r, got rc %d' % (name, text, p.returncode))

for f in fails: print('struct-param-semantic: FAIL', f)
print('struct-param-semantic: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
sys.exit(1 if fails else 0)
PY
