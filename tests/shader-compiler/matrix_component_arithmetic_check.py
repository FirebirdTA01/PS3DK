"""Component-wise float matrix arithmetic, independent values and row controls.

Reference probes accept matrix/scalar and matching matrix/matrix +,-,*,/ in
both profiles. '*' here is component-wise; the mul() builtin is separate.
Dyadic samples avoid introducing a precision allowance. Half/fixed matrix
precision and implicit integer-local conversion remain separate debts.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp

INPUTS = [[.5, 1., 2., 2.], [-.5, 2., 4., .5], [2., -.5, 1., -2.]]
OPS = {'add': ('+', lambda a,b:a+b), 'sub': ('-', lambda a,b:a-b),
       'mul': ('*', lambda a,b:a*b), 'div': ('/', lambda a,b:a/b)}


def cases():
    for rows,cols in [(1,1),(1,3),(3,1),(2,2),(2,3),(3,2),(4,4)]:
        ty=f'float{rows}x{cols}'
        numbers=list(range(1,rows*cols+1))
        init=f'{ty} m={ty}('+','.join(map(str,numbers))+');'
        indices=[0,len(numbers)-1,len(numbers)//2,0]
        # A one-column matrix row has scalar type: do not subscript it again.
        result='float4('+','.join(f'm[{i//cols}]' if cols==1 else
                                 f'm[{i//cols}][{i%cols}]' for i in indices)+')'
        for name,(op,fn) in OPS.items():
            for mode in ['compound','expression','scalar_left','matrix_rhs','literal']:
                if mode=='compound': statement=f'm {op}= t.w;'
                elif mode=='expression': statement=f'm=m{op}t.w;'
                elif mode=='scalar_left': statement=f'm=t.w{op}m;'
                elif mode=='literal': statement=f'm {op}= 2.0;'
                else: statement=f'm {op}= {ty}('+','.join('t.w' for _ in numbers)+');'
                control=''
                for r in range(rows):
                    lhs,rhs=(f't.w',f'm[{r}]') if mode=='scalar_left' else (f'm[{r}]','2.0' if mode=='literal' else 't.w')
                    control+=f'm[{r}]={lhs}{op}{rhs};'
                def expected(t,mode=mode,fn=fn,numbers=numbers,indices=indices):
                    return [fn(t[3],numbers[i]) if mode=='scalar_left' else
                            fn(numbers[i],2. if mode=='literal' else t[3]) for i in indices]
                # Scalar/matrix division with non-power-of-two matrix entries
                # has real hardware rounding; use power-of-two entries below.
                if name=='div' and mode=='scalar_left':
                    continue
                yield f'{rows}x{cols}_{name}_{mode}',init+statement+'return '+result+';',init+control+'return '+result+';',expected
    init='float2x2 m=float2x2(1,2,4,8);'
    yield 'scalar_left_div',init+'m=t.w/m;return float4(m[0],m[1]);',init+'m[0]=t.w/m[0];m[1]=t.w/m[1];return float4(m[0],m[1]);',lambda t:[t[3]/v for v in [1.,2.,4.,8.]]
    for name,(op,fn) in OPS.items():
        yield 'once_'+name,init+f'float s=t.w;m{op}=s++;return float4(m[0],s,m[1][1]);',init+f'float s=t.w;float old=s++;m[0]=m[0]{op}old;m[1]=m[1]{op}old;return float4(m[0],s,m[1][1]);',lambda t,fn=fn:[fn(1.,t[3]),fn(2.,t[3]),t[3]+1.,fn(8.,t[3])]
        yield 'int_constant_'+name,init+f'm{op}=2;return float4(m[0],m[1]);',init+f'm[0]=m[0]{op}2.0;m[1]=m[1]{op}2.0;return float4(m[0],m[1]);',lambda t,fn=fn:[fn(v,2.) for v in [1.,2.,4.,8.]]
    yield 'negative_divisor',init+'m/=-t.w;return float4(m[0],m[1]);',init+'m[0]=m[0]/(-t.w);m[1]=m[1]/(-t.w);return float4(m[0],m[1]);',lambda t:[v/-t[3] for v in [1.,2.,4.,8.]]
    # Matrix array brace initialization is a separate unsupported path.
    # Whole-element stores isolate the already supported tracked lvalue.
    yield 'array_selector_once', 'float2x2 a[2];a[0]=float2x2(2,4,8,16);a[1]=float2x2(3,6,12,24);int i=0;float s=t.w;a[i++]/=s++;return float4(a[0][0][0],a[0][1][1],i,s);', 'float s=t.w;float old=s++;return float4(2.0/old,16.0/old,1,s);',lambda t:[2./t[3],16./t[3],1.,t[3]+1.]


def source(body,stage):
    return 'float4 main(float4 t:%s):%s{%s}\n' % ('TEXCOORD0' if stage=='fp' else 'POSITION','COLOR' if stage=='fp' else 'POSITION',body)


def check(compiler,work):
    assert fp_eval.self_test()
    vp.predication_selftest()
    failures=[];values=0;pairs=0;refusals=0
    for name,body,control,want in cases():
        for stage in ['fp','vp']:
            good=0
            for mode,text in [('matrix',body),('rows',control)]:
                src=work/f'{name}-{stage}-{mode}.cg';dst=src.with_suffix('.bin')
                src.write_text(source(text,stage));dst.unlink(missing_ok=True)
                p=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                src.with_suffix('.log').write_text(p.stdout+p.stderr)
                if p.returncode or not dst.exists():
                    failures.append(f'{name}/{stage}/{mode}: refused {p.returncode}: {p.stderr.strip()}');continue
                good+=1;blob=dst.read_bytes()
                for t in INPUTS:
                    got=fp_eval.evaluate(blob,{'TEX0':t}) if stage=='fp' else vp.evaluate(blob,{},inputs={0:t},binary32=True,predication=True).get(0)
                    values+=1
                    if got!=want(t):failures.append(f'{name}/{stage}/{mode}: {t}: {got} != {want(t)}')
            if good==2:pairs+=1
    for stage in ['fp','vp']:
        for name,body in [
            ('shape','float2x2 m=float2x2(1,2,3,4);m/=float3(1,2,4);return float4(m[0],m[1]);'),
            ('const','const float2x2 m=float2x2(1,2,3,4);m/=t.w;return float4(m[0],m[1]);'),
            ('implicit_int','float2x2 m=float2x2(1,2,3,4);int s=t.x;m+=s;return float4(m[0],m[1]);'),
            ('half_scalar','float2x2 m=float2x2(1,2,3,4);half s=t.x;m=m+s;return float4(m[0],m[1]);')]:
            src=work/f'refuse-{stage}-{name}.cg';dst=src.with_suffix('.bin')
            src.write_text(source(body,stage));dst.unlink(missing_ok=True)
            p=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
            src.with_suffix('.log').write_text(p.stdout+p.stderr)
            if p.returncode!=1 or dst.exists() or '--extension=' in p.stderr:
                failures.append(f'{name}/{stage}: expected refusal1 without artifact or extension hint, got {p.returncode}')
            else:refusals+=1
    for failure in failures[:20]:print('FAIL:',failure)
    print(f'matrix-component-arithmetic: {"FAIL" if failures else "PASS"} pairs={pairs} values={values} refusals={refusals} failures={len(failures)}')
    return int(bool(failures))


if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--work',type=Path);a=ap.parse_args()
    if a.work:
        a.work.mkdir(parents=True,exist_ok=True);raise SystemExit(check(a.compiler,a.work))
    with tempfile.TemporaryDirectory(prefix='matrix-component-arithmetic-') as tmp:raise SystemExit(check(a.compiler,Path(tmp)))
