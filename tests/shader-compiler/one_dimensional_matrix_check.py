"""One-dimensional FLOAT matrices retain shape and multiply row-major.

Authored FP/VP probes and scalar/vector twins were measured before implementation.
Use exact dyadic inputs and independent arithmetic; compilation alone never passes.
"""
import argparse
import math
from pathlib import Path
import subprocess
import tempfile
import struct

from fp_eval import evaluate
from vp_binding_check import evaluate_bindings
from uniform_container_check import Container, check_container
from uniform_struct_param_check import with_uniforms

SAMPLES = [([1,-2,3,-4], [2,1,-1,3]),
           ([-.5,.25,2,-1], [1,-2,.5,4]), ([0,1,0,-1], [-1,0,2,.5])]
M = [[1,2,3,4], [5,6,7,8], [9,10,11,12], [13,14,15,17]]
MDECL = 'float4x4 M=float4x4(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,17);'
ROW = 'float1x4 r=float1x4(q.x,q.y,q.z,q.w);'
COL = 'float4x1 c=float4x1(p.x,p.y,p.z,p.w);'
REFUSALS = {
    'shape_mismatch': 'float1x4 a=float1x4(p);float4x1 b=a;return float4(b[0],b[1],b[2],b[3]);',
    'row_bounds': 'float1x4 m=float1x4(p);return m[1];',
    'column_bounds': 'float3x1 m=float3x1(p.x,p.y,p.z);return float4(m[0][1],0,0,1);',
    'row_runtime': 'float4x1 m=float4x1(p);return float4(m[int(q.x)],0,0,1);',
    'ambiguous_scalar_mul': 'float4x1 m=float4x1(p);return mul(m,q.x);',
    # Measured reference accepts; these conversions remain explicit debts.
    'debt_implicit_scalar': 'float1x1 m=float1x1(p.x);float x=m;return float4(x,p.y,p.z,1);',
    'debt_vector_assignment': 'float4x1 m=p;return float4(m[0],m[1],m[2],m[3]);',
    'debt_column_assignment': 'float4x1 m=float4x1(p);float4 x=m;return x;',
}


def check_uniform_matrix(blob, stage):
    assert not check_container(blob)['issues'], 'uniform container has structural issues'
    container = Container(blob)
    records = {r['name']: r for r in container.records}
    assert records['r']['type'] == 1052 and records['c']['type'] == 1061
    assert records['r[0]']['type'] == 1048
    assert 'r[1]' not in records and 'c[4]' not in records
    # A one-column matrix row is CG_FLOAT1, distinct from scalar CG_FLOAT.
    assert all(records[f'c[{i}]']['type'] == 1091 for i in range(4)), records
    for r,c in [([2,1,-1,3], [1,-2,3,-4]), ([-1,2,4,-3], [.5,1,-2,4])]:
        values = {'r[0]':r, **{f'c[{i}]':[c[i],0,0,0] for i in range(4)}}
        expected = [sum(a*b for a,b in zip(r,c)),r[0],c[2],1]
        if stage == 'fp':
            assert all(records[name]['offsets'] for name in values)
            actual = evaluate(with_uniforms(blob, container, values), {})
        else:
            registers = {records[name]['register']:lanes for name,lanes in values.items()}
            assert len(registers) == 5, 'matrix row bindings overlap'
            actual = evaluate_bindings(blob, registers, {})[0]
        assert actual == expected, (actual,expected)
    return 2


def mv(p):
    return [sum(x*y for x,y in zip(row,p)) for row in M]


def cases():
    result = [
        ('matrix_column', '', MDECL+COL+'float4x1 z=mul(M,c);return float4(z[0],z[1],z[2],z[3]);', lambda p,q: mv(p)),
        ('row_matrix', '', MDECL+ROW+'float1x4 z=mul(r,M);return z[0];', lambda p,q: [sum(q[i]*M[i][j] for i in range(4)) for j in range(4)]),
        ('outer_product', '', COL+ROW+'float4x4 z=mul(c,r);return float4(z[1][2],z[2][1],z[3][3],z[0][1]);', lambda p,q: [p[1]*q[2],p[2]*q[1],p[3]*q[3],p[0]*q[1]]),
        ('one_product', '', 'float1x1 a=float1x1(p.x);float1x1 b=float1x1(q.y);return float4(float(mul(a,b)),float(a),float(b),1);', lambda p,q: [p[0]*q[1],p[0],q[1],1]),
        ('cast_scalar', '', 'float1x1 a=float1x1(p.x);return float4((float)a,p.y,p.z,1);', lambda p,q: [p[0],p[1],p[2],1]),
        ('broadcast_once', '', 'float s=p.x;float4x1 m=float4x1(s++);return float4(m[0],m[3],s,1);', lambda p,q: [p[0],p[0],p[0]+1,1]),
        ('column_from_vector', '', 'float4x1 m=float4x1(p);return float4(m[0],m[1],m[2],m[3]);', lambda p,q: p),
    ]
    for name, expr in [('chain_right','mul(r,mul(M,c))'), ('chain_left','mul(mul(r,M),c)')]:
        result.append((name, '', MDECL+ROW+COL+'float1x1 z='+expr+';return float4(float(z),p.x,q.y,1);',
                       lambda p,q: [sum(a*b for a,b in zip(q,mv(p))),p[0],q[1],1]))
    result.append(('static_chain', 'static const '+MDECL.replace('M=','K='),
                   ROW+COL+'return float4(float(mul(r,mul(K,c))),p.x,q.y,1);',
                   lambda p,q: [sum(a*b for a,b in zip(q,mv(p))),p[0],q[1],1]))
    for rows, cols in [(1,1),(1,2),(1,3),(1,4),(2,1),(3,1),(4,1)]:
        n = rows*cols
        typ = f'float{rows}x{cols}'
        args = ','.join('p.'+'xyzw'[i] for i in range(n))
        outputs = [f'm[{i//cols}]'+(f'[{i%cols}]' if cols>1 else '') for i in range(n)]
        body = f'{typ} m={typ}({args});return float4('+','.join(outputs+['1']*(4-n))+');'
        result.append((typ, '', body, lambda p,q,n=n: p[:n]+[1]*(4-n)))
    # Existing rectangular and scalar controls catch inadvertent type/overload changes.
    result.extend([
        ('rectangular_control', '', 'float2x3 m=float2x3(1,2,3,4,5,6);return float4(mul(m,p.xyz),p.w,1);', lambda p,q: [p[0]+2*p[1]+3*p[2],4*p[0]+5*p[1]+6*p[2],p[3],1]),
        ('scalar_control', '', 'return p*q;', lambda p,q: [a*b for a,b in zip(p,q)]),
    ])
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('compiler')
    parser.add_argument('--keep', type=Path)
    args = parser.parse_args()
    failures, checked = [], 0
    with tempfile.TemporaryDirectory(prefix='one-dimensional-matrix-') as temporary:
        work = args.keep or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        for stage in ('fp','vp'):
            for name, prefix, body, expected in cases():
                label = name+'_'+stage
                src, dst = work/(label+'.cg'), work/(label+'.bin')
                if dst.exists():
                    raise RuntimeError('output already exists: '+str(dst))
                src.write_text(prefix+'\nfloat4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):'+
                               ('COLOR' if stage=='fp' else 'POSITION')+'{'+body+'}\n')
                run = subprocess.run([args.compiler,'-p','sce_'+stage+'_rsx','-e','main',
                                      '--emit-container',str(dst),str(src)], capture_output=True,
                                     text=True, timeout=30)
                src.with_suffix('.stdout').write_text(run.stdout)
                src.with_suffix('.stderr').write_text(run.stderr)
                try:
                    assert run.returncode == 0 and dst.is_file(), run.stderr
                    blob = dst.read_bytes()
                    for p,q in SAMPLES:
                        actual = (evaluate(blob, {'TEX0':p,'TEX1':q}) if stage=='fp' else
                                  evaluate_bindings(blob, {}, {'IN8':p,'IN9':q})[0])
                        want = expected(p,q)
                        assert len(actual)==len(want)==4 and all(math.isfinite(a) and a==b for a,b in zip(actual,want)), (p,q,actual,want)
                        checked += 1
                    print('PASS', label)
                except (AssertionError, ValueError, RuntimeError, KeyError, IndexError) as error:
                    failures.append(label)
                    print('FAIL', label, str(error))
            for form,prefix,parameters in (
                    ('global','uniform float1x4 r;uniform float4x1 c;',''),
                    ('entry','', 'uniform float1x4 r,uniform float4x1 c'),
                    ('default','uniform float1x4 r=float1x4(2,1,-1,3);uniform float4x1 c=float4x1(1,-2,3,-4);','')):
                label = 'uniform_rows_'+form+'_'+stage
                src, dst = work/(label+'.cg'), work/(label+'.bin')
                if dst.exists():
                    raise RuntimeError('output already exists: '+str(dst))
                src.write_text(prefix+'float4 main('+parameters+'):'+
                               ('COLOR' if stage=='fp' else 'POSITION')+
                               '{return float4(float(mul(r,c)),r[0].x,float(c[2]),1);}')
                run = subprocess.run([args.compiler,'-p','sce_'+stage+'_rsx','-e','main',
                                      '--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                src.with_suffix('.stderr').write_text(run.stderr)
                try:
                    assert run.returncode == 0 and dst.is_file(), run.stderr
                    blob = dst.read_bytes()
                    if form == 'default':
                        records = {r['name']:r for r in Container(blob).records}
                        assert not records['r']['default'] and not records['c']['default']
                        for name,values in {'r[0]':[2,1,-1,3], **{f'c[{i}]':[v,0,0,0] for i,v in enumerate([1,-2,3,-4])}}.items():
                            assert records[name]['default'] and list(struct.unpack_from('>4f',blob,records[name]['default'])) == values
                    checked += check_uniform_matrix(blob, stage)
                    print('PASS', label)
                except (AssertionError, ValueError, RuntimeError, KeyError, IndexError) as error:
                    failures.append(label)
                    print('FAIL', label, str(error))
            for name,body in REFUSALS.items():
                label = name+'_'+stage
                src, dst = work/(label+'.cg'), work/(label+'.bin')
                if dst.exists():
                    raise RuntimeError('output already exists: '+str(dst))
                src.write_text('float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1):'+
                               ('COLOR' if stage=='fp' else 'POSITION')+'{'+body+'}')
                run = subprocess.run([args.compiler,'-p','sce_'+stage+'_rsx','-e','main',
                                      '--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                src.with_suffix('.stderr').write_text(run.stderr)
                if run.returncode != 1 or dst.exists():
                    failures.append(label)
                    print('FAIL',label,'expected refusal without container')
                else:
                    print('PASS',label,'refusal')
    print('one-dimensional-matrix:',len(failures),'failures;',checked,'exact numerical checks')
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
