"""Float determinant values: independent permutation sum and scalar expansion.

The reference accepts rectangular matrices by narrowing to their leading
min(rows, cols) square, and scalar/vector inputs by taking the first scalar.
Half/fixed and integer matrix arithmetic remain explicitly recorded debt.
"""
import argparse
import itertools
import subprocess
import tempfile
from pathlib import Path
import fp_eval
import vp_pow_vector_check as vp

INPUTS=[[.5,1.,-1.,2.],[-2.,.5,1.,-1.],[1.,-1.,2.,.5]]

def determinant(m):
    n=len(m);answer=0.
    for p in itertools.permutations(range(n)):
        term=1.
        for r,c in enumerate(p):term*=m[r][c]
        inversions=sum(p[i]>p[j] for i in range(n) for j in range(i+1,n))
        answer+=(-term if inversions%2 else term)
    return answer

def expansion(n,access):
    terms=[]
    for p in itertools.permutations(range(n)):
        inversions=sum(p[i]>p[j] for i in range(n) for j in range(i+1,n))
        product='*'.join(access(r,c) for r,c in enumerate(p))
        terms.append(('-' if inversions%2 else '+')+'('+product+')')
    return ''.join(terms).lstrip('+')

def cases():
    for rows in range(1,5):
        for cols in range(1,5):
            n=min(rows,cols);ty=f'float{rows}x{cols}'
            def element(r,c):
                return f'(t.{"xyzw"[(r+2*c)%4]}*{[-1,1,2][(r+c)%3]}.0+{(r*3+c*2)%5-2}.0)'
            init=f'{ty} m={ty}('+','.join(element(r,c) for r in range(rows) for c in range(cols))+');'
            def access(r,c):return f'm[{r}]' if cols==1 else f'm[{r}][{c}]'
            control=expansion(n,access)
            def want(t,rows=rows,cols=cols,n=n):
                m=[[t[(r+2*c)%4]*[-1,1,2][(r+c)%3]+((r*3+c*2)%5-2) for c in range(n)] for r in range(n)]
                return [determinant(m),1.,2.,3.]
            yield f'{rows}x{cols}','',init+'return float4(determinant(m),1,2,3);',init+f'return float4({control},1,2,3);',want
    for width in range(1,5):
        arg='t.x' if width==1 else 't.'+'xyzw'[:width]
        yield f'width{width}','','return float4(determinant('+arg+'),1,2,3);','return float4(t.x,1,2,3);',lambda t:[t[0],1.,2.,3.]
    yield 'once','','float x=t.x;float d=determinant(float2x2(x++,t.y,t.z,t.w));return float4(d,x,2,3);','float x=t.x;float a=x++;return float4(a*t.w-t.y*t.z,x,2,3);',lambda t:[t[0]*t[3]-t[1]*t[2],t[0]+1,2.,3.]
    yield 'helper','float f(float2x2 m){return determinant(m);}','return float4(f(float2x2(t)),1,2,3);','return float4(t.x*t.w-t.y*t.z,1,2,3);',lambda t:[t[0]*t[3]-t[1]*t[2],1.,2.,3.]
    yield 'singular','','return float4(determinant(float2x2(t.xy,t.xy)),1,2,3);','return float4(0,1,2,3);',lambda t:[0.,1.,2.,3.]
    yield 'source_override','float determinant(float2x2 m){return m[1][0]+7;}','return float4(determinant(float2x2(t)),1,2,3);','return float4(t.z+7,1,2,3);',lambda t:[t[2]+7,1.,2.,3.]

def source(stage,body,prefix=''):
    return prefix+'float4 main(float4 t:%s):%s{%s}\n'%('TEXCOORD0' if stage=='fp' else 'POSITION','COLOR' if stage=='fp' else 'POSITION',body)

def check(compiler,work):
    assert fp_eval.self_test();vp.predication_selftest()
    errors=[];pairs=values=refusals=0
    for name,prefix,body,control,want in cases():
        for stage in ['fp','vp']:
            good=0
            for mode,text in [('builtin',source(stage,body,prefix)),('control',source(stage,control))]:
                src=work/f'{name}-{stage}-{mode}.cg';dst=src.with_suffix('.bin')
                src.write_text(text);dst.unlink(missing_ok=True)
                p=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                src.with_suffix('.log').write_text(p.stdout+p.stderr)
                if p.returncode or not dst.exists():errors.append(f'{name}/{stage}/{mode}: refused rc={p.returncode}: {p.stderr.strip()}');continue
                good+=1;blob=dst.read_bytes()
                for t in INPUTS:
                    try:
                        got=fp_eval.evaluate(blob,{'TEX0':t}) if stage=='fp' else vp.evaluate(blob,{},inputs={0:t},binary32=True,predication=True).get(0)
                        values+=1
                        if got!=want(t):errors.append(f'{name}/{stage}/{mode}: {got} != {want(t)}')
                    except Exception as exc:errors.append(f'{name}/{stage}/{mode}: UNMODELLED {exc}')
            if good==2:pairs+=1
    for stage in ['fp','vp']:
        for name,body in [('half_debt','half2x2 m=half2x2(t);return float4(determinant(m),1,2,3);'),
                          ('fixed_matrix_debt','fixed2x2 m=fixed2x2(t);return float4(determinant(m),1,2,3);'),
                          ('fixed_vector_debt','fixed2 m=fixed2(t.xy);return float4(determinant(m),1,2,3);'),
                          ('fixed_scalar_debt','fixed m=t.x;return float4(determinant(m),1,2,3);'),
                          ('int_debt','int2x2 m=int2x2(t);return float4(determinant(m),1,2,3);'),
                          ('array','float2x2 a[2];a[0]=float2x2(t);return float4(determinant(a),1,2,3);'),
                          ('arity','return float4(determinant(t.x,t.y),1,2,3);')]:
            src=work/f'{stage}-{name}.cg';dst=src.with_suffix('.bin');src.write_text(source(stage,body));dst.unlink(missing_ok=True)
            p=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
            src.with_suffix('.log').write_text(p.stdout+p.stderr)
            if p.returncode==1 and not dst.exists() and '--extension=' not in p.stderr:refusals+=1
            else:errors.append(f'{name}/{stage}: expected clean refusal1, got {p.returncode}')
    for error in errors[:24]:print('FAIL',error)
    print(f'determinant: pairs={pairs} values={values} refusals={refusals} failures={len(errors)}')
    return int(bool(errors) or pairs!=48 or values!=288 or refusals!=14)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--work',type=Path);a=ap.parse_args()
    if a.work:a.work.mkdir(parents=True,exist_ok=True);raise SystemExit(check(a.compiler,a.work))
    with tempfile.TemporaryDirectory(prefix='determinant-') as tmp:raise SystemExit(check(a.compiler,Path(tmp)))
