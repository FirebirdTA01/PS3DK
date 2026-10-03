"""Two-column matvec reductions preserve row results, swizzles, and live inputs.

Independent arithmetic expectations were measured on both native profiles.
FP and the wider-column/vecmat controls protect paths outside this VP slice.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from fp_eval import evaluate
from vp_binding_check import evaluate_bindings

INPUTS = [[0.5, -1., 2., -3.], [-2., 0.25, -0.5, 4.], [0., 1.5, -4., 0.25]]


def cases():
    result = []
    for rows in (2, 3, 4):
        constructor = ','.join(str(v) for v in range(1, 2 * rows + 1))
        packed = 'r,0,1' if rows == 2 else 'r,1' if rows == 3 else 'r'
        result.append((f'constant_{rows}x2',
                       f'float{rows}x2 m=float{rows}x2({constructor});float{rows} r=mul(m,p.xy);return float4({packed});',
                       lambda p, n=rows: [p[0]*(2*r+1)+p[1]*(2*r+2) for r in range(n)] + ([0.,1.] if n==2 else [1.] if n==3 else [])))
    result += [
        ('runtime_rows', 'float2x2 m=float2x2(p.xy,p.zw);float2 r=mul(m,p.yx);return float4(r,p.xy);',
         lambda p: [2*p[0]*p[1],p[2]*p[1]+p[3]*p[0],p[0],p[1]]),
        ('selected_negated', 'float3x2 m=float3x2(-p.yw,p.zx,-p.wz);float3 r=mul(m,-p.wy);return float4(r,1);',
         lambda p: [2*p[1]*p[3],-p[2]*p[3]-p[0]*p[1],p[3]*p[3]+p[2]*p[1],1.]),
        ('both_pairs_repeat', 'float2x2 m=float2x2(p.xx,p.yy);float2 r=mul(m,p.zz);return float4(r,p.xy);',
         lambda p: [2*p[0]*p[2],2*p[1]*p[2],p[0],p[1]]),
        ('one_pair_repeats', 'float2x2 m=float2x2(p.xy,p.zz);float2 r=mul(m,p.wx);return float4(r,p.zw);',
         lambda p: [p[0]*p[3]+p[1]*p[0],p[2]*(p[3]+p[0]),p[2],p[3]]),
        ('snapshot', 'float2 v=p.xy;float2x2 m=float2x2(p.zw,p.yx);float2 first=mul(m,v);float2 second=mul(m,v.yx);return float4(first+second,v);',
         lambda p: [(p[2]+p[3])*(p[0]+p[1]),(p[0]+p[1])**2,p[0],p[1]]),
        ('vecmat_control', 'float2x2 m=float2x2(1,2,3,4);float2 r=mul(p.xy,m);return float4(r,0,1);',
         lambda p: [p[0]+3*p[1],2*p[0]+4*p[1],0.,1.]),
        ('three_columns_control', 'float2x3 m=float2x3(1,2,3,4,5,6);float2 r=mul(m,p.xyz);return float4(r,0,1);',
         lambda p: [p[0]+2*p[1]+3*p[2],4*p[0]+5*p[1]+6*p[2],0.,1.]),
        ('four_columns_control', 'float2x4 m=float2x4(1,2,3,4,5,6,7,8);float2 r=mul(m,p);return float4(r,0,1);',
         lambda p: [sum((i+1)*v for i,v in enumerate(p)),sum((i+5)*v for i,v in enumerate(p)),0.,1.]),
    ]
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    parser.add_argument('--keep', type=Path)
    args = parser.parse_args()
    failures = []
    checked = 0
    with tempfile.TemporaryDirectory(prefix='vp-matvec2-') as temporary:
        work = args.keep or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        for stage in ('fp', 'vp'):
            for name, body, expected in cases():
                label = name + '_' + stage
                src, dst = work / (label + '.cg'), work / (label + '.bin')
                if dst.exists():
                    raise RuntimeError('output already exists: ' + str(dst))
                output = 'COLOR' if stage == 'fp' else 'POSITION'
                src.write_text('float4 selected(float4 p:TEXCOORD0):' + output + '{' + body + '}\n', encoding='utf-8')
                run = subprocess.run([args.compiler, '-p', 'sce_' + stage + '_rsx', '-e', 'selected',
                                      '--emit-container', str(dst), str(src)],
                                     capture_output=True, text=True, timeout=30)
                src.with_suffix('.stdout').write_text(run.stdout, encoding='utf-8')
                src.with_suffix('.stderr').write_text(run.stderr, encoding='utf-8')
                try:
                    assert run.returncode == 0 and dst.is_file(), run.stderr
                    blob = dst.read_bytes()
                    for p in INPUTS:
                        value = (evaluate(blob, {'TEX0': p}) if stage == 'fp'
                                 else evaluate_bindings(blob, {}, {'IN8': p})[0])
                        want = expected(p)
                        assert len(value) == 4 and all(abs(a-b) <= 1e-5*max(1., abs(b)) for a,b in zip(value,want)), (p,value,want)
                        checked += 1
                    print('PASS', label)
                except (AssertionError, ValueError, RuntimeError, KeyError, IndexError) as error:
                    failures.append(label)
                    print('FAIL', label, str(error))
    print('vp-matvec2:', len(failures), 'failures;', checked, 'numeric checks')
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
