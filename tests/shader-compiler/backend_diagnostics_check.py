"""Authored refusal diagnostics: exact exit 1, no output, named first cause.

Keep diagnostics useful to callers that display one line and to census tools
that bucket it. The closed VP two-column matrix gap has a value control.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from vp_pow_vector_check import evaluate


CASES = [
    # fmod is lowered now (a scalar operand splats), so these name two VP
    # gaps that remain: the construct still leads the first line.
    ('vp_atan2', 'vp', 'void main(float4 p:POSITION,out float4 o:POSITION,out float4 c:COLOR) {o=p;c=atan2(p,p.wzyx);}',
     r'^nv40-general: VP atan2 lowering deferred'),
    ('vp_derivative', 'vp', 'void main(float4 p:POSITION,out float4 o:POSITION,out float4 c:COLOR) {o=p;c=ddx(p);}',
     r'^nv40-general: screen-space derivatives are fragment-only'),
    # A semantic-less struct member now takes an implicit TEXCOORD (measured;
    # value-checked by implicit-varying); 'semantic' keeps this message's shape.
    ('semantic', 'fp', 'struct Input {float4 shade:TEXUNIT0;}; float4 main(Input data):COLOR {return data.shade;}',
     r"unsupported input semantic.*TEXUNIT0.*entry 'main'.*input 'data.shade'.*vec4"),
]


def valid(rc, has_output, log, pattern):
    lines = [line.strip() for line in log.splitlines() if line.strip()]
    return (rc == 1 and not has_output and bool(lines)
            and lines[0].startswith('nv40-general:')
            and re.search(pattern, lines[0]) is not None)


def main():
    compiler = sys.argv[1]
    # A crash, stale output or a name mentioned only after the generic banner
    # cannot stand in for a useful first diagnostic.
    good = 'nv40-general: unsupported IR op call @fmod'
    controls = [(124, False, good), (1, True, good),
                (1, False, 'nv40-general: refusing to emit - the program did not lower completely\n' + good)]
    for args in controls:
        assert not valid(*args, r'unsupported IR op call.*@fmod')
    assert valid(1, False, good, r'unsupported IR op call.*@fmod')
    passed = len(controls) + 1
    failed = 0
    with tempfile.TemporaryDirectory(prefix='rsxcg-diagnostics-') as tmp:
        # Preserve the original formerly-refused uniform case and judge it
        # with distinct rows so swapped/clobbered row results cannot pass.
        src=Path(tmp)/'vp_matvec_2col.cg'; dst=src.with_suffix('.bin')
        src.write_text('float4 main(float4 p:POSITION,uniform float2x2 m):POSITION {return float4(mul(m,p.xy),0,1);}')
        result=subprocess.run([compiler,'-p','sce_vp_rsx','--emit-container',str(dst),str(src)],
                              capture_output=True,text=True,timeout=30)
        try:
            assert result.returncode==0 and dst.is_file(),result.stderr
            blob=dst.read_bytes()
            for x,y in ((-.5,.25),(0.,-2.),(1.5,.5),(4.,-1.)):
                got=evaluate(blob,{'m[0]':[1.,2.,0.,0.],'m[1]':[3.,-4.,0.,0.]},
                             inputs={0:[x,y,7.,-3.]},binary32=True)[0]
                assert got==[x+2*y,3*x-4*y,0.,1.],(x,y,got)
            passed+=1
            print('PASS: vp_matvec_2col (4 exact values)')
        except (AssertionError,ValueError,RuntimeError,KeyError) as e:
            failed+=1
            print('FAIL: vp_matvec_2col',e)
        for name, profile, source, pattern in CASES:
            src = Path(tmp) / (name + '.cg')
            dst = Path(tmp) / (name + '.bin')
            src.write_text(source, encoding='utf-8')
            result = subprocess.run([compiler, '-p', 'sce_' + profile + '_rsx',
                                     '--emit-container', str(dst), str(src)],
                                    capture_output=True, text=True, timeout=30)
            # stderr is the diagnostic channel; stdout's IR summary is not it.
            ok = valid(result.returncode, dst.exists(), result.stderr, pattern)
            if ok:
                passed += 1
                print('PASS:', name)
            else:
                failed += 1
                print('FAIL:', name, 'exit=', result.returncode,
                      'output=', dst.exists(), '\n' + result.stderr)
    print(f'backend-diagnostics: {passed + failed} tests, {passed} pass, {failed} fail')
    return int(failed != 0)


if __name__ == '__main__':
    sys.exit(main())
