"""Float matrix conversion: scalar [0][0], or a leading 1D vector.

Independently authored reference probes cover the shape/context matrix;
nonzero output padding avoids a reference zero-padding unwritten-register
idiom outside the strict FP evaluator. No register defaults are assumed.
General 2D matrices do not implicitly flatten into vectors. Reduced
precision and integer matrix conversions remain outside this slice.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp

INPUTS = [[.25,-.5,2.,4.],[-2.,1.,.5,-.25],[4.,2.,-1.,.5]]
SHAPES = [(1,1),(1,2),(1,3),(1,4),(2,1),(3,1),(4,1),(2,2),(2,3),(3,2),(4,4)]

def shape_cases():
    for rows,cols in SHAPES:
        init=f'float{rows}x{cols} m=float{rows}x{cols}('+','.join('t.'+'xyzw'[k%4] for k in range(rows*cols))+');'
        for width in range(1,5):
            ty='float' if width==1 else f'float{width}'
            result='float4(v'+',1'*(4-width)+')' if width<4 else 'v'
            body=init+f'{ty} v=m;return {result};'
            permitted=width==1 or (min(rows,cols)==1 and width<=max(rows,cols))
            control='return float4('+','.join(['t.'+lane for lane in 'xyzw'[:width]]+['1']*(4-width))+');'
            yield f'{rows}x{cols}_to_{width}',body,control,lambda t,w=width:t[:w]+[1.]*(4-w),permitted

def context_cases():
    for name,prefix,body,want,control in [
        ('store','','float1x3 m=float1x3(t.xyz);float3 v;v=m;return float4(v,1);',lambda t:t[:3]+[1.],'return float4(t.xyz,1);'),
        ('return','float3 f(float1x3 m){return m;}','return float4(f(float1x3(t.xyz)),1);',lambda t:t[:3]+[1.],'return float4(t.xyz,1);'),
        ('argument','float4 f(float3 v){return float4(v,1);}','return f(float1x3(t.xyz));',lambda t:t[:3]+[1.],'return float4(t.xyz,1);'),
        ('explicit','','return float4(float3(float1x3(t.xyz)),1);',lambda t:t[:3]+[1.],'return float4(t.xyz,1);'),
        ('product','','float1x2 a=float1x2(t.xy);float2x3 m=float2x3(1,2,4,8,16,32);float3 v=mul(a,m);return float4(v,1);',lambda t:[t[0]+8*t[1],2*t[0]+16*t[1],4*t[0]+32*t[1],1.],'return float4(t.x+8*t.y,2*t.x+16*t.y,4*t.x+32*t.y,1);'),
        ('once','','float s=t.x;float3 v=float1x3(s++,t.y,t.z);return float4(v,s);',lambda t:t[:3]+[t[0]+1.],'return float4(t.xyz,t.x+1);'),
        ('cast','','return float4((float3)float1x3(t.xyz),1);',lambda t:t[:3]+[1.],'return float4(t.xyz,1);'),
        ('snapshot','','float1x3 m=float1x3(t.xyz);float3 v=m;m[0]=t.wzy;return float4(v,1);',lambda t:t[:3]+[1.],'return float4(t.xyz,1);'),
        ('exact_overload','float4 f(float1x3 m){return float4(m[0],1);}float4 f(float3 v){return float4(9,8,7,6);}','return f(float1x3(t.xyz));',lambda t:t[:3]+[1.],'return float4(t.xyz,1);'),
        ('entry_return','','return float4x1(t);',lambda t:t,'return t;'),
    ]:
        yield name,prefix,body,want,control

def source(stage,body,prefix=''):
    return prefix+'float4 main(float4 t:%s):%s{%s}\n'%('TEXCOORD0' if stage=='fp' else 'POSITION','COLOR' if stage=='fp' else 'POSITION',body)

def check(compiler,work):
    assert fp_eval.self_test()
    vp.predication_selftest()
    errors=[];values=0;pairs=0;refusals=0
    for stage in ['fp','vp']:
        cases=[(name,'',body,want,control,accepted) for name,body,control,want,accepted in shape_cases()]
        cases += [(name,prefix,body,want,control,True) for name,prefix,body,want,control in context_cases()]
        for name,prefix,body in [
            ('half_matrix','','half1x3 m=half1x3(t.xyz);float3 v=m;return float4(v,1);'),
            ('int_matrix','','int1x3 m=int1x3(1,2,3);float3 v=m;return float4(v,1);'),
            ('array_conversion','float4 f(float3 a[1]){return float4(a[0],1);}','float1x3 a[1];a[0]=float1x3(t.xyz);return f(a);'),
            ('out_conversion','void f(out float3 v){v=float3(1,2,3);}','float1x3 m=float1x3(t.xyz);f(m);return float4(m[0],1);'),
            ('inout_conversion','void f(inout float3 v){v*=2;}','float1x3 m=float1x3(t.xyz);f(m);return float4(m[0],1);'),
        ]:cases.append((name,prefix,body,None,'',False))
        for name,prefix,body,want,control,accepted in cases:
            ok=0
            for mode,text in [('conversion',source(stage,body,prefix))]+([('control',source(stage,control))] if accepted else []):
                src=work/f'{stage}_{name}_{mode}.cg';dst=src.with_suffix('.bin')
                src.write_text(text);dst.unlink(missing_ok=True)
                proc=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                src.with_suffix('.log').write_text(proc.stdout+proc.stderr)
                if not accepted:
                    if proc.returncode==1 and not dst.exists() and '--extension=' not in proc.stderr:refusals+=1
                    else:errors.append(f'{stage}/{name}: invalid shape did not refuse cleanly, rc={proc.returncode}')
                    continue
                if proc.returncode or not dst.exists():
                    errors.append(f'{stage}/{name}/{mode}: refused rc={proc.returncode}: {proc.stderr.strip()}');continue
                blob=dst.read_bytes();ok+=1
                for t in INPUTS:
                    try:
                        got=fp_eval.evaluate(blob,{'TEX0':t}) if stage=='fp' else vp.evaluate(blob,{},inputs={0:t},binary32=True,predication=True).get(0)
                        values+=1
                        if got!=want(t):errors.append(f'{stage}/{name}/{mode}: {t}: {got} != {want(t)}')
                    except Exception as exc:errors.append(f'{stage}/{name}/{mode}: UNMODELLED: {exc}')
            if accepted and ok==2:pairs+=1
    for error in errors:print('FAIL',error)
    print(f'matrix vector conversion: {pairs} pairs, {values} values, {refusals} refusals, {len(errors)} failures')
    return not errors and pairs==66 and values==396 and refusals==52

def main():
    parser=argparse.ArgumentParser();parser.add_argument('compiler');parser.add_argument('--work',type=Path)
    args=parser.parse_args()
    if args.work:
        args.work.mkdir(parents=True,exist_ok=True)
        return 0 if check(args.compiler,args.work) else 1
    with tempfile.TemporaryDirectory(prefix='matrix-vector-conversion-') as root:
        return 0 if check(args.compiler,Path(root)) else 1

if __name__=='__main__':raise SystemExit(main())
