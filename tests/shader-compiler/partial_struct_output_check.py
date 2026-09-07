"""Partial struct outputs must preserve the same lane writes as a local vector."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from vp_words import decode


def compile_shader(compiler, work, name, profile, text):
    src, out = work / (name + '.cg'), work / (name + '.bin')
    src.write_text(text)
    p = subprocess.run([compiler, '-p', profile, '--dump-ir', '--emit-container',
                        str(out), str(src)], capture_output=True, timeout=20)
    assert p.returncode == 0 and out.is_file(), (name, p.returncode, p.stderr)
    blob = out.read_bytes()
    header = struct.unpack_from('>8I', blob)
    return blob, blob[header[7]:header[7] + header[6]], (p.stdout + p.stderr).decode(errors='replace')


def main():
    cases = {
        'xy_wz': 'V.xy=t.xy; V.wz=t.wz;',
        'zw_yx': 'V.zw=t.zw; V.yx=t.yx;',
        'reverse': 'V.w=t.w; V.z=t.z; V.y=t.y; V.x=t.x;',
        'rgba': 'V.rgb=t.rgb; V.a=t.a;',
        'rg_ab': 'V.rg=t.rg; V.ab=t.ab;',
        'overlap': 'V.xy=t.xy; V.zw=t.zw; V.x=t.y;',
        'cached_rhs': 'float a=t.x*0.25; V.xy=t.yz; V.zw=t.zw; V.x=a;',
        'whole_last': 'V.xy=t.xy; V.zw=t.zw; V=t.wzyx;',
        'branch': 'V.xy=t.xy; V.zw=t.zw; if(t.x>0.5) V.x=t.y;',
        'branch_first': 'if(t.x>0.5) {V.xy=t.xy; V.zw=t.zw;} else {V.wz=t.xy; V.yx=t.zw;}',
        'scalar_broadcast': 'V.rgb=t.x*0.25; V.a=t.y;',
        'self_read': 'V.xy=t.xy; V.zw=t.zw; V.yx=V.xy;',
        'partial': 'V.xy=t.xy;',
    }
    with tempfile.TemporaryDirectory(prefix='ps3dk-partial-struct-') as temp:
        work = Path(temp)
        for profile, semantic, input_sem in [('sce_vp_rsx', 'POSITION', 'POSITION'),
                                              ('sce_fp_rsx', 'COLOR', 'TEXCOORD0')]:
            for name, body in cases.items():
                keep = 'float4 keep:TEXCOORD0;' if profile == 'sce_vp_rsx' else ''
                setup = 'R r; r.keep=t;' if keep else 'R r;'
                start = 'struct R {float4 p:'+semantic+';'+keep+'}; R main(float4 t:'+input_sem+') {'
                actual = start + setup + body.replace('V', 'r.p') + 'return r;}'
                twin = start + setup + 'float4 v;' + body.replace('V', 'v') + 'r.p=v; return r;}'
                blob, code, ir = compile_shader(sys.argv[1], work, profile+name, profile, actual)
                _, expected, _ = compile_shader(sys.argv[1], work, profile+name+'-flat', profile, twin)
                assert code == expected, (profile, name, 'partial field differs from local-vector twin')
                assert re.search(r'stout|storeoutput|store_output', ir, re.I), (name, 'missing StoreOutput')
                if profile == 'sce_vp_rsx':
                    lines, error = decode(blob)
                    assert not error, error
                    masks = [re.search(r'mask=([xyzw]+)', line).group(1)
                             for line in lines if 'dst=o0 ' in line]
                    lanes = set(''.join(masks))
                    assert lanes == set('xy' if name == 'partial' else 'xyzw'), (name, lines)
                print('PASS:', profile, name, flush=True)
        # Distinct real member names must never be treated as swizzle aliases.
        text = ('struct R {float4 rgb:COLOR; float4 xyz:COLOR1;}; '
                'R main(float4 t:TEXCOORD0){R r; r.rgb.xy=t.xy; r.rgb.zw=t.zw; '
                'r.xyz.xy=t.yx; r.xyz.zw=t.wz; return r;}')
        _, code, _ = compile_shader(sys.argv[1], work, 'real-fields', 'sce_fp_rsx', text)
        _, other, _ = compile_shader(sys.argv[1], work, 'real-fields-control', 'sce_fp_rsx',
                                    text.replace('t.yx', 't.xy').replace('t.wz', 't.zw'))
        assert code != other, 'distinct struct fields were merged'
        print('PASS: real rgb/xyz fields remain distinct', flush=True)
        for profile, sem, input_sem, unused_sem in [
                ('sce_vp_rsx', 'POSITION', 'POSITION', 'TEXCOORD0'),
                ('sce_fp_rsx', 'COLOR', 'TEXCOORD0', 'COLOR1')]:
            text = ('struct R {float4 p:'+sem+'; float4 unused:'+unused_sem+';}; '
                    'R main(float4 t:'+input_sem+'){R r; r.p=t; return r;}')
            _, code, _ = compile_shader(sys.argv[1], work, profile+'-unused', profile, text)
            _, expected, _ = compile_shader(sys.argv[1], work, profile+'-unused-control', profile,
                                            text.replace('float4 unused:'+unused_sem+';', ''))
            assert code == expected, (profile, 'untouched field acquired an output store')
            print('PASS:', profile, 'untouched field emits no store', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print('FAIL:', exc, file=sys.stderr)
        sys.exit(1)
