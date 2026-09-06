#!/usr/bin/env bash
# t_0970e943: depth fetches need an RGB decode, including a patchable
# default parameter. A plain tex2D fetch cannot satisfy these assertions.
# The companion pixel rows decode packed RGB bytes in the rig's A8R8G8B8
# textures. They do not exercise a native depth-format fetch or a depth
# buffer write/read round trip.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:?compiler required}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
for name in normal precise; do
    for form in intrinsic control; do
        timeout 20s "$compiler" -p sce_fp_rsx --emit-container "$work/$name-$form.fpo" \
            "$root/tools/rsx-cg-compiler/tests/shaders/fp_texdepth_${name}_${form}_f.cg" >"$work/$name-$form.log" 2>&1 || {
            cat "$work/$name-$form.log" >&2
            echo "FAIL: $name/$form did not compile" >&2; exit 1;
        }
    done
done
cat > "$work/plain.cg" <<'CG'
float4 main(float2 uv : TEXCOORD0, uniform sampler2D image) : COLOR
{ return tex2D(image, uv); }
CG
cat > "$work/user.cg" <<'CG'
float texDepth2D(sampler2D image, float2 uv) { return 0.25; }
float4 main(float2 uv : TEXCOORD0, uniform sampler2D image) : COLOR
{ float d = texDepth2D(image, uv); return float4(d, d, d, 1.0); }
CG
for name in plain user; do
    timeout 20s "$compiler" -p sce_fp_rsx --emit-container "$work/$name.fpo" "$work/$name.cg" >"$work/$name.log" 2>&1 || {
        cat "$work/$name.log" >&2
        echo "FAIL: $name control did not compile" >&2; exit 1;
    }
done
python3 - "$work" <<'PY'
import pathlib, struct, sys
root = pathlib.Path(sys.argv[1])
def canonical_decode(blob, label):
    # The explicit tex2D control fetches an unused alpha lane. The intrinsic
    # needs only RGB. Normalize only that unused write and unused W source
    # selectors on DP3 / XYZ MAD; all live selectors, instructions, constants
    # and parameter records must remain byte-identical.
    b = bytearray(blob)
    def u32(p): return struct.unpack_from('>I', b, p)[0]
    def swap(v): return ((v << 16) | (v >> 16)) & 0xffffffff
    p, end = u32(28), u32(28) + u32(24)
    textures = dots = 0
    while p < end:
        words = [swap(u32(p+4*i)) for i in range(4)]
        opcode, mask = (words[0] >> 24) & 63, (words[0] >> 9) & 15
        inline = any((w & 3) == 2 for w in words[1:])
        if opcode == 0x17:
            textures += 1
            if mask & 7 != 7:
                raise SystemExit('FAIL: %s depth fetch did not write XYZ' % label)
            words[0] &= ~(8 << 9)
        if opcode == 5:
            dots += 1
            words[1] &= ~(3 << 15)
            words[2] &= ~(3 << 15)
        if opcode == 4 and mask == 7:
            for i in range(1, 4): words[i] &= ~(3 << 15)
        for i, w in enumerate(words): struct.pack_into('>I', b, p+4*i, swap(w))
        p += 32 if inline else 16
    if p != end or textures != 1 or dots != 1:
        raise SystemExit('FAIL: %s must contain one XYZ texture fetch and one DP3' % label)
    return b
for case, name, expected in (
    ('normal', '_depth_factor', [0x3f7f0000, 0x3b7f0000, 0x377f0000]),
    ('precise', '_depth_factor_precise', [0x3b800001, 0x37800001, 0x33800001]),
):
    for form in ('intrinsic', 'control'):
        b = (root / (case + '-' + form + '.fpo')).read_bytes()
        def u32(p): return struct.unpack_from('>I', b, p)[0]
        def unswap(v): return ((v << 16) | (v >> 16)) & 0xffffffff
        def cstr(p): return b[p:b.index(b'\0', p)].decode('ascii') if p else ''
        found = []
        for i in range(u32(12)):
            p = u32(16) + 48*i
            if cstr(u32(p+16)) != name:
                continue
            emb = u32(p+24)
            if not emb or not u32(emb):
                raise SystemExit('FAIL: %s/%s decode parameter has no patch offsets' % (case, form))
            for j in range(u32(emb)):
                off = u32(28) + u32(emb+4+4*j)
                bits = [unswap(u32(off+4*k)) for k in range(3)]
                if bits != expected:
                    raise SystemExit('FAIL: %s/%s wrong depth defaults: %s' % (case, form, bits))
            found.append(p)
        if len(found) != 1:
            raise SystemExit('FAIL: %s/%s missing or duplicated patchable %s' % (case, form, name))
    actual = canonical_decode((root/(case+'-intrinsic.fpo')).read_bytes(), case+'/intrinsic')
    control = canonical_decode((root/(case+'-control.fpo')).read_bytes(), case+'/control')
    if actual != control:
        raise SystemExit('FAIL: %s intrinsic differs from independently expanded decode' % case)
for name in ('plain', 'user'):
    b = (root/(name+'.fpo')).read_bytes()
    if b'_depth_factor' in b:
        raise SystemExit('FAIL: %s control acquired an intrinsic decode parameter' % name)
PY
cat > "$work/collision.cg" <<'CG'
uniform float3 _depth_factor;
float4 main(float2 uv : TEXCOORD0, uniform sampler2D image) : COLOR
{ float d = texDepth2D(image, uv); return float4(d, d, d, 1.0); }
CG
rc=0
timeout 20s "$compiler" -p sce_fp_rsx --emit-container "$work/collision.fpo" "$work/collision.cg" >"$work/collision.log" 2>&1 || rc=$?
if [[ "$rc" != 1 || -e "$work/collision.fpo" ]] ||
   ! grep -Fq "depth decode parameter '_depth_factor' conflicts with a source declaration" "$work/collision.log"; then
    cat "$work/collision.log" >&2
    echo "FAIL: collision must refuse by its binding diagnostic, exit exactly 1 and no container (got $rc)" >&2
    exit 1
fi
printf 'PASS: texdepth-decode-test (both decode forms, patchable defaults, ordinary/user controls and named collision refusal)\n'
