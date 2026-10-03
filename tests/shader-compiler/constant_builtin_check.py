"""Measured finite float builtin initializers, with literal and binding controls.

The private oracle is never needed by this test. User-defined calls and runtime
dependencies stay named debts; they must not be mistaken for builtin constants.
"""
import argparse
from pathlib import Path
import subprocess
import struct
import tempfile

from fp_eval import evaluate
from vp_binding_check import evaluate_bindings
from uniform_container_check import Container, check_container
from uniform_struct_param_check import with_uniforms


PAIRS = [
    ('ceil_pos', 'float', 'ceil(1.25)', '2.0'),
    ('ceil_neg', 'float', 'ceil(-1.25)', '-1.0'),
    ('max', 'float', 'max(-0.75,-0.25)', '-0.25'),
    ('min', 'float', 'min(0.75,0.25)', '0.25'),
    ('clamp_inside', 'float', 'clamp(0.375,0.0,1.0)', '0.375'),
    ('clamp_low', 'float', 'clamp(-0.5,0.0,1.0)', '0.0'),
    ('clamp_high', 'float', 'clamp(1.5,0.0,1.0)', '1.0'),
    ('nested', 'float', 'max(abs(-0.125),0.0625)', '0.125'),
    # The oracle's ceil(-.25) is NEGATIVE zero in both profiles.
    ('ceil_vector', 'float4', 'ceil(float4(-1.25,-0.25,0.25,1.25))', 'float4(-1,-0.0,1,2)'),
    ('clamp_vector', 'float4', 'clamp(float4(-1,0.25,0.75,2),0.0,1.0)', 'float4(0,0.25,0.75,1)'),
    ('negative_zero', 'float', 'ceil(-0.0)', '-0.0'),
]
REFUSALS = {
    # Literal-only non-static const currently drops its uniform binding.
    # Keep NEW call-bearing forms refused, including nested traversal.
    'const_call_binding_debt': 'const float K=ceil(1.25);',
    'const_nested_binding_debt': 'const float K=float2(0.5+ceil(1.25),0).x;',
    'const_transitive_binding_debt': 'static const float A=ceil(1.25);static const float B=A+1.0;const float K=B;',
    'opposite_zero_min_debt': 'static const float K=min(-0.0,0.0);',
    'opposite_zero_max_debt': 'static const float K=max(0.0,-0.0);',
    'reversed_clamp_debt': 'static const float K=clamp(0.5,1.0,0.0);',
    'user_ceil': 'float ceil(float x){return x+4;}static const float K=ceil(0.25);',
    'uniform_dependency': 'uniform float U=0.25;static const float K=max(U,0.5);',
    'const_dependency': 'const float U=0.25;static const float K=max(U,0.5);',
    'static_dependency': 'static float U=0.25;static const float K=max(U,0.5);',
    'half_debt': 'static const half K=max(half(0.375),half(0.25));',
    'fixed_debt': 'static const fixed K=max(fixed(0.375),fixed(0.25));',
    'int_debt': 'static const int K=max(int(0.375),int(0.25));',
    'nan_debt': 'static const float K=max(0.0/0.0,1.0);',
    'infinity_debt': 'static const float K=ceil(1.0/0.0);',
}


def run(compiler, work):
    failures, checks = [], 0
    for stage in ('fp', 'vp'):
        si, so = ('TEXCOORD0', 'COLOR') if stage == 'fp' else ('POSITION', 'POSITION')

        def compile_one(name, declarations, result='p+K', refusal=False):
            src = work / (name+'_'+stage+'.cg')
            dst = src.with_suffix('.bin')
            dst.unlink(missing_ok=True)
            src.write_text(declarations+'\nfloat4 main(float4 p:'+si+'):'+so+'{return '+result+';}\n')
            proc = subprocess.run([str(compiler), '-p', 'sce_'+stage+'_rsx', '-e', 'main',
                                   '--emit-container', str(dst), str(src)],
                                  capture_output=True, text=True, timeout=30)
            src.with_suffix('.log').write_text(proc.stdout+proc.stderr)
            if refusal:
                assert proc.returncode == 1 and not dst.exists(), (name, proc.returncode, proc.stderr)
                assert 'cannot evaluate' in proc.stderr+proc.stdout, (name, proc.stderr)
                return
            assert proc.returncode == 0 and dst.is_file(), (name, proc.returncode, proc.stderr)
            blob = dst.read_bytes()
            assert blob and not check_container(blob)['issues'], name
            return blob

        def check(name, function):
            nonlocal checks
            try:
                function()
                checks += 1
            except (AssertionError, ValueError, KeyError, subprocess.TimeoutExpired) as exc:
                failures.append((stage, name, str(exc)))

        for name, ty, expression, literal in PAIRS:
            def twin(name=name, ty=ty, expression=expression, literal=literal):
                a = compile_one(name, f'static const {ty} K={expression};')
                b = compile_one(name+'_literal', f'static const {ty} K={literal};')
                assert a == b, name+' differs from its measured literal twin'
            check(name, twin)
        def dependency():
            a = compile_one('dependency', 'static const float2 V=float2(-0.125,1.25);'
                            'static const float A=ceil(V.y);'
                            'static const float K=clamp(max(abs(V.x),0.0625)*A,0.0,1.0);')
            b = compile_one('dependency_literal', 'static const float K=0.25;')
            assert a == b
        check('dependency', dependency)
        def repeated_dependencies():
            declarations = 'static const float A0=1.0;'
            for i in range(1,29):
                declarations += f'static const float A{i}=A{i-1}+A{i-1};'
            # A plain const's policy validation must visit each declaration
            # once, not expand this small DAG into 2**28 initializer visits.
            a = compile_one('repeated_dependencies', declarations+'const float K=A28;')
            b = compile_one('repeated_literal', 'const float K=268435456.0;')
            assert a == b
        check('repeated_dependencies', repeated_dependencies)
        for storage in ('uniform',):
            def uniform(storage=storage):
                a = compile_one(storage+'_default', storage+' float K=ceil(1.25);')
                b = compile_one(storage+'_literal', storage+' float K=2.0;')
                assert a == b
                c = Container(a)
                records = [r for r in c.records if r['name'] == 'K']
                assert len(records) == 1
                rec = records[0]
                # Patch TWO non-default values through the emitted resource.
                for value in (-0.5, 3.0):
                    p = [.125,.25,.5,1]
                    if stage == 'fp':
                        assert rec['offsets']
                        got = evaluate(with_uniforms(a, c, {'K':[value]*4}), {'TEX0':p})
                    else:
                        got = evaluate_bindings(a, {rec['register']:[value]*4}, {'IN0':p})[0]
                    assert got == [x+value for x in p], (storage, value, got)
            check(storage+'_binding', uniform)
        def signed_zero_default():
            blob = compile_one('signed_zero_default', 'uniform float4 K=ceil(float4(-1.25,-0.25,0.25,1.25));')
            rec = next(r for r in Container(blob).records if r['name'] == 'K')
            assert rec['default']
            assert struct.unpack_from('>4I', blob, rec['default']) == (0xbf800000,0x80000000,0x3f800000,0x40000000)
        check('signed_zero_default', signed_zero_default)
        for name, source in REFUSALS.items():
            for dormant in (False, True):
                check(name+str(dormant), lambda name=name, source=source, dormant=dormant:
                      compile_one(name+str(dormant), source, 'p' if dormant else 'p+K', True))
        if stage == 'fp':
            def helper_condition(shadow):
                declarations = ('static const float U=0.25;' if shadow else '')
                declarations += 'float f(float U){if(ceil('+('U' if shadow else '0.25')+')>0.0)return U;else return -U;}'
                blob = compile_one('helper_condition_'+str(shadow), declarations, 'float4(f(p.x),p.y,p.z,p.w)')
                for x in (-1.25,-0.25,0.25,1.25):
                    got = evaluate(blob, {'TEX0':[x,0.25,0.5,1]})
                    expected = x if (not shadow or x>0) else -x
                    assert got == [expected,0.25,0.5,1], (shadow,x,got)
            check('constant_helper_condition', lambda: helper_condition(False))
            check('shadowed_helper_condition', lambda: helper_condition(True))
    for failure in failures:
        print('FAIL', *failure)
    print(f'constant-builtin: {checks} checks passed, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--keep', type=Path)
    args = parser.parse_args()
    if args.keep:
        args.keep.mkdir(parents=True, exist_ok=True)
        raise SystemExit(run(args.compiler.resolve(), args.keep))
    with tempfile.TemporaryDirectory(prefix='constant-builtin-') as directory:
        raise SystemExit(run(args.compiler.resolve(), Path(directory)))
