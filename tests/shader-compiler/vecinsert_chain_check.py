"""VecInsert compaction must keep lane values while avoiding redundant writes."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from partial_struct_output_check import compile_shader
from vp_words import decode


def mov_lane_values(blob):
    """Symbolically execute the MOV-only controls, reading before writing."""
    lines, error = decode(blob)
    assert not error, error
    values = {'IN0': list('xyzw')}
    for line in lines:
        match = re.fullmatch(r'\d+ MOV dst=(R\d+|o0) mask=([xyzw]+) '
                             r'src0=(IN0|R\d+)\.([xyzw]{4}) src1=\S+ src2=\S+', line)
        assert match, ('MOV-only control has an unexpected instruction', line)
        dst, mask, src, swz = match.groups()
        before = values.get(src, [None]*4)[:]
        result = values.setdefault(dst, [None]*4)
        for lane in mask:
            result['xyzw'.index(lane)] = before['xyzw'.index(swz['xyzw'.index(lane)])]
    return values.get('o0')


def main():
    failures = []
    with tempfile.TemporaryDirectory(prefix='ps3dk-vecinsert-') as temp:
        work = Path(temp)
        for profile, sem, inp in [('sce_vp_rsx', 'POSITION', 'POSITION'),
                                   ('sce_fp_rsx', 'COLOR', 'TEXCOORD0')]:
            start = 'struct R {float4 p:'+sem+';}; R main(float4 t:'+inp+'){R r;'
            cases = {
                'xyz_w': ('r.p.xyz=t.xyz; r.p.w=1;', None),
                'loop': ('r.p.w=1; for(int i=0;i<3;++i) r.p.xyz=t.xyz*i;',
                         'r.p.w=1; r.p.xyz=t.xyz*2;'),
                'reorder': ('r.p.zy=t.xy; r.p.xw=t.zw; r.p.y=t.w;',
                            'r.p.zy=t.xw; r.p.xw=t.zw;'),
                'read_modify': ('r.p=t; r.p.xy=r.p.yx; r.p.z=r.p.x;', None),
                'shared': ('r.p=t*2; float4 saved=r.p; r.p.xy=t.yx; r.p+=saved;', None),
                'accumulate': ('r.p=t; for(int i=0;i<3;++i) r.p.xyz+=t.zyx;', None),
            }
            for name, (body, control) in cases.items():
                blob, code, ir = compile_shader(sys.argv[1], work, profile+name, profile,
                                                start+body+'return r;}')
                if control:
                    _, other, _ = compile_shader(sys.argv[1], work, profile+name+'-control', profile,
                                                  start+control+'return r;}')
                    if code != other:
                        failures.append((profile, name, 'overwritten chain differs from final writes'))
                if profile == 'sce_vp_rsx' and name == 'xyz_w':
                    lines, error = decode(blob)
                    assert not error, error
                    if len(code) != 32:
                        failures.append((name, 'expected two instructions', len(code)//16, lines))
                # Shared and self-read values must agree with an ordinary local vector.
                local = 'float4 v;'+body.replace('r.p', 'v')+'r.p=v;'
                _, twin, _ = compile_shader(sys.argv[1], work, profile+name+'-local', profile,
                                            start+local+'return r;}')
                if code != twin:
                    failures.append((profile, name, 'local vector differs'))
                print('checked', profile, name, flush=True)
        # Unlike the local-vector twins, these expected values are independent
        # of both compiler paths. They catch deleting a shared base or merging
        # sequential self-reads into a simultaneous MOV.
        for name, body, expected in [
            ('swap', 'r.p=t; r.p.xy=r.p.yx;', 'yxzw'),
            ('read_after', 'r.p=t; r.p.x=r.p.y; r.p.z=r.p.x;', 'yyyw'),
            ('saved_base', 'r.p=t; float4 saved=r.p; r.p.xy=t.zw; r.p.zw=saved.yx;', 'zwyx'),
            ('overlap_reads', 'r.p=t; r.p.zy=r.p.xw; r.p.xw=r.p.yz;', 'wwxx'),
            ('saved_insert', 'r.p=t; r.p.x=t.y; float4 saved=r.p; r.p.x=t.z; r.p.zw=saved.xy;', 'zyyy'),
            ('shared_chain', 'r.p=t; r.p.x=t.y; r.p.y=t.z; float4 saved=r.p; r.p.x=t.w; r.p.z=saved.x;', 'wzyw'),
        ]:
            text = 'struct R {float4 p:POSITION;}; R main(float4 t:POSITION){R r;'+body+'return r;}'
            blob, _, _ = compile_shader(sys.argv[1], work, name, 'sce_vp_rsx', text)
            actual = mov_lane_values(blob)
            if actual != list(expected):
                failures.append((name, 'lane value changed', actual, expected))
            print('checked lane values', name, flush=True)
    assert not failures, failures


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print('FAIL:', exc, file=sys.stderr)
        sys.exit(1)
