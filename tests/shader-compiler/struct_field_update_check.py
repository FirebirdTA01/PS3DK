"""A struct field's swizzle updates its whole value (t_2c400883).

Compare independent flat-vector spellings of each operation. Decoding only
ucode excludes parameter names and struct layout from the comparison.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

from fp_sources import instructions, source, ucode_words


def compile_shader(compiler, work, name, text):
    src, out = work / (name + '.cg'), work / (name + '.fpo')
    src.write_text(text)
    p = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container',
                        str(out), str(src)], capture_output=True, timeout=20)
    assert p.returncode == 0 and out.is_file(), (name, p.returncode, p.stderr)
    return ucode_words(out.read_bytes())


cases = {
    'rgb': 'V.rgb = p.zyx * 0.25;',
    'alpha': 'V.a = p.z * 0.25;',
    'permuted': 'V.zy = p.wx;',
    'compound': 'V.rgb *= p.zyx;',
    'self_read': 'V.rgb = V.bgr * p.xyz;',
    'repeated': 'V.xyz = p.zyx; V.y = p.w;',
    'scalar_broadcast': 'V.rgb = p.z;',
    'overwrite': 'V.rgb = p.zyx; V = p.wzyx;',
}
with tempfile.TemporaryDirectory(prefix='ps3dk-struct-field-update-') as temp:
    work = Path(temp)
    for name, body in cases.items():
        struct = ('struct Result { float4 color : COLOR; }; '
                  'Result main(float4 p : TEXCOORD0) { Result r; r.color=p; '
                  + body.replace('V', 'r.color') + ' return r; }')
        flat = ('float4 main(float4 p : TEXCOORD0) : COLOR { float4 v=p; '
                + body.replace('V', 'v') + ' return v; }')
        got = compile_shader(sys.argv[1], work, name, struct)
        expected = compile_shader(sys.argv[1], work, name+'_flat', flat)
        assert got == expected, (name, 'struct-field update differs from flat vector')
        print('PASS:', name, flush=True)
    for label, statement, expected_expr in [
        ('scalar_result', 'float a=(r.color.w=p.z); return a;', 'p.z'),
        ('vector_result', 'float3 a=(r.color.zyx=p.xyz); return float4(a,p.w);', 'float4(p.xyz,p.w)'),
    ]:
        got = compile_shader(sys.argv[1], work, label,
            'struct R { float4 color; }; float4 main(float4 p:TEXCOORD0):COLOR { R r; r.color=p; '
            + statement + ' }')
        expected = compile_shader(sys.argv[1], work, label+'_expected',
            'float4 main(float4 p:TEXCOORD0):COLOR { return '+expected_expr+'; }')
        assert got == expected, (label, 'assignment expression must return its RHS')
        print('PASS:', label, flush=True)
    face = compile_shader(sys.argv[1], work, 'face',
        'float4 main(float face : FACE) : COLOR { return face; }')
    words = list(instructions(face))
    assert len(words) == 1, ('FACE export', words)
    operand = source(words[0][0], 1)
    assert operand['type'] == 1 and operand['name'] == 'FACING' and operand['swizzle'] == 0, operand
    print('PASS: FACE broadcasts the facing input', flush=True)
    clamped = compile_shader(sys.argv[1], work, 'clamped_pow',
        'float4 main(float4 p : TEXCOORD0) : COLOR { return pow(saturate(p.x*2-1),3); }')
    unclamped = compile_shader(sys.argv[1], work, 'unclamped_pow',
        'float4 main(float4 p : TEXCOORD0) : COLOR { return pow(p.x*2-1,3); }')
    assert clamped != unclamped, 'pow discarded saturation of its base'
    decoded = [w for w, _ in instructions(clamped)]
    log_index = next(i for i,w in enumerate(decoded) if ((w[0] >> 24) & 63) == 29)
    assert any(w[0] & (1 << 31) for w in decoded[:log_index]), 'pow has no clamp before LG2'
    print('PASS: pow preserves saturation before taking the logarithm', flush=True)
    # Out-struct parameters currently emit no StoreOutput on either parent
    # or candidate. Keep that gap an exact refusal, never a stale first store.
    out_struct = work / 'out_struct.cg'
    refused = work / 'out_struct.fpo'
    out_struct.write_text('struct R { float4 color:COLOR; }; '
        'void main(float4 p:TEXCOORD0,out R o) { '
        'o.color=p; o.color.xyz=p.zyx*0.25; }')
    result = subprocess.run([sys.argv[1], '-p', 'sce_fp_rsx', '--emit-container',
                             str(refused), str(out_struct)], capture_output=True, timeout=20)
    assert result.returncode == 1, ('out struct must refuse', result.returncode)
    assert not refused.exists(), 'refused out struct left an artifact'
    assert b'no instructions emitted' in result.stderr, result.stderr
    print('PASS: unsupported out-struct parameter refuses without an artifact', flush=True)
