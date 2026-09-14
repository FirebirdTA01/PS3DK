#!/usr/bin/env python3
"""t_a290c3c8: typed, invariant static loops agree with explicit expansions.

The 64-iteration limit is OUR resource bound. Reference unroll decisions depend
on body and induction type (t_bc4fa4e5). The VP integer-eight arithmetic control
is a named instruction-shape divergence: reference BRA, our explicit expansion.
No reference byte-identity claim is made for that form. VP texture samplelod is
a separate gap, t_6346513f; arithmetic twins exercise this change in both profiles.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    repo = Path(__file__).resolve().parents[2]
    scratch = repo / '.local' / 'tmp'
    scratch.mkdir(parents=True, exist_ok=True)
    count = 0
    refusals = 0
    with tempfile.TemporaryDirectory(prefix='static-loop-', dir=scratch) as tmp:
        work = Path(tmp)

        def compile_one(name, body, profile, refusal=None, prefix=''):
            src, dst = work / (name + '.cg'), work / (name + '.bin')
            if profile == 'sce_fp_rsx':
                source = 'float4 main(float4 t:TEXCOORD0):COLOR{float4 a=0;' + body + 'return a;}'
            else:
                source = ('void main(float4 p:POSITION,float4 t:TEXCOORD0,out float4 op:POSITION,'
                          'out float4 o:TEXCOORD0){op=p;float4 a=0;' + body + 'o=a;}')
            src.write_text(prefix + source)
            assert not dst.exists(), name + ': stale destination'
            result = subprocess.run([compiler, '-p', profile, '--emit-container', str(dst), str(src)],
                                    capture_output=True, text=True, timeout=30)
            log = result.stdout + result.stderr
            if refusal is not None:
                assert result.returncode == 1 and not dst.exists(), (
                    f'{name}: expected exit 1 / no container, got {result.returncode}\n{log}')
                assert refusal in log, f'{name}: expected diagnostic {refusal!r}\n{log}'
                return None
            assert result.returncode == 0 and dst.exists(), (
                f'{name}: expected acceptance, got {result.returncode}\n{log}')
            data = dst.read_bytes()
            assert len(data) >= 32, name + ': truncated container'
            header = struct.unpack_from('>8I', data)
            assert header[2] == len(data) and header[6] > 0 and header[6] % 16 == 0, name + ': invalid container'
            assert header[7] + header[6] <= len(data), name + ': invalid ucode extent'
            return data

        def twin(label, body, explicit, profile, prefix=''):
            nonlocal count
            name = profile + '-' + label
            actual = compile_one(name, body, profile, prefix=prefix)
            expected = compile_one(name + '-explicit', explicit, profile, prefix=prefix)
            assert actual == expected, name + ': strict explicit-expansion twin differs'
            count += 1

        for profile in ('sce_fp_rsx', 'sce_vp_rsx'):
            arithmetic = 'a=a.yzwx*.75+float4(t.xy,i*.03125,1);'
            for trips in (1, 2, 4, 8):
                explicit = ''.join('{float i=' + str(i) + '.0;' + arithmetic + '}' for i in range(trips))
                twin(f'float-{trips}', f'for(float i=0.0;i<{trips};i+=1.0){{{arithmetic}}}', explicit, profile)
            # Two separate mechanisms: float induction above, bound identifier here.
            explicit = ''.join('{int i=' + str(i) + ';' + arithmetic + '}' for i in (-4, -2, 0, 2, 4))
            twin('local-bound', 'int bound=4;int i;for(i=-bound;i<=bound;i+=2){' + arithmetic + '}', explicit, profile)
            for label, header, values in (
                ('negative-up', 'float i=-4.0;i<0.0;i+=1.0', (-4,-3,-2,-1)),
                ('positive-down', 'float i=4.0;i>0.0;i-=1.0', (4,3,2,1)),
                ('negative-down', 'float i=-1.0;i>=-4.0;i-=1.0', (-1,-2,-3,-4)),
                ('zero-trip', 'float i=4.0;i<0.0;i+=1.0', ())):
                explicit = ''.join('{float i=' + str(i) + '.0;' + arithmetic + '}' for i in values)
                twin(label, 'for(' + header + '){' + arithmetic + '}', explicit, profile)
            for label, body, explicit in (
                ('external-final', 'float i;for(i=-4.0;i<0.0;i+=1.0){a+=t*(i+5.0);}a+=i;',
                 'float i=0;a=t*1.0;a+=t*2.0;a+=t*3.0;a+=t*4.0;a+=i;'),
                ('declared-shadow', 'float i=.75;for(float i=0.0;i<4.0;i+=1.0){a+=t*(i+1.0);}a+=i;',
                 'float i=.75;a=t*1.0;a+=t*2.0;a+=t*3.0;a+=t*4.0;a+=i;'),
                ('inner-shadow', 'for(float i=0.0;i<4.0;i+=1.0){{float i=10.0;a+=t*i;}}',
                 'a=t*10.0;a+=t*10.0;a+=t*10.0;a+=t*10.0;')):
                twin(label, body, explicit, profile)
            # Existing integer path remains accepted, including reference-looping VP8.
            explicit = ''.join('{int i=' + str(i) + ';' + arithmetic + '}' for i in range(8))
            twin('int8-shape-divergence', 'for(int i=0;i<8;i+=1){' + arithmetic + '}', explicit, profile)
            uint_body = 'a=a*t.x+i;'
            twin('uint-ascending', 'for(unsigned int i=0;i<3;i++){' + uint_body + '}',
                 'for(int i=0;i<3;i++){' + uint_body + '}', profile)
            uint_locals = 'unsigned int start=0;unsigned int bound=3;unsigned int step=1;'
            twin('uint-local-bounds', uint_locals + 'for(unsigned int i=start;i<bound;i+=step){' + uint_body + '}',
                 uint_locals + ''.join('{int i=' + str(i) + ';' + uint_body + '}' for i in range(3)), profile)
            twin('uint-high-boundary', 'for(unsigned int i=4294967293;i<4294967295;i++){a+=t*(i-4294967292);}',
                 'a=t;a+=t*2.0;', profile)
            for label, body, diagnostic in (
                ('induction-write', 'for(float i=0.0;i<4.0;i+=1.0){a+=t*(i+1.0);i+=1.0;}', 'back-edge'),
                ('bound-write', 'int bound=4;for(float i=0.0;i<bound;i+=1.0){a+=t*(i+1.0);bound=2;}', 'back-edge'),
                ('step-write', 'int step=2;for(float i=0.0;i<6.0;i+=step){a+=t*(i+1.0);step=1;}', 'back-edge'),
                ('nonintegral-gap', 'for(float i=0.0;i<2.0;i+=.5){a+=t*(i+1.0);}', 'back-edge'),
                ('unsigned-bound-gap', 'unsigned int start=1;for(float i=-start;i<2;i+=1.0){a+=t;}', 'back-edge'),
                ('uint-negated-gap', 'unsigned int start=1;for(unsigned int i=-start;i<2;i++){a+=t;}', 'back-edge'),
                ('uint-wrapping-gap', 'for(unsigned int i=4294967295;i>=4294967295;i++){a+=t;}', 'back-edge'),
                ('negative-zero-gap', 'for(float i=-0.0;i<1;i+=1.0){a=t/i;}', 'back-edge'),
                ('negative-zero-local-gap', 'float start=-0.0;for(float i=start;i<1;i+=1.0){a=t/i;}', 'back-edge'),
                ('resource-cap', 'for(float i=0.0;i<65.0;i+=1.0){a+=t;}', 'hardware-loop'),
            ):
                # These reference-accepted forms remain named gaps, not reference refusals.
                compile_one(profile + '-' + label, body, profile, refusal=diagnostic)
                refusals += 1
        # FP texture path: actual sampled work, rather than a constant-only loop.
        fetch = 'a+=tex2D(tex,t.xy+float2(i*.01,0));'
        for ty in ('float', 'int'):
            values = (-4,-2,0,2,4)
            body = f'{ty} size=4;{ty} i;for(i=-size;i<=size;i+=2){{{fetch}}}'
            explicit = ''.join('{' + ty + ' i=' + str(i) + ('.0;' if ty == 'float' else ';') + fetch + '}' for i in values)
            twin('texture-local-' + ty, body, explicit, 'sce_fp_rsx', prefix='uniform sampler2D tex;')
    assert (count, refusals) == (34, 20), (count, refusals)
    print(f'static-loop-expansion: PASS ({count} strict twins, {refusals} named refusals)')


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print('FAIL:', exc, file=sys.stderr)
        sys.exit(1)
