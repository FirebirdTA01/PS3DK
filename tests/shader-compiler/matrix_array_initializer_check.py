"""Same-shape float matrix array elements, including contextual row braces.

Flattened array lists, one-wide nested row braces and reduced precision are
separate measured reference-accepted debts. Explicit element stores provide
controls; independent expected values cover every row of both array elements.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path
import fp_eval
import vp_pow_vector_check as vp

INPUTS = [[.25,-.5,2.,4.],[-2.,1.,.5,-.25],[4.,2.,-1.,.5]]
SHAPES = [(1,1),(1,3),(2,2),(3,2),(4,2),(4,4)]

def cases():
    for rows,cols in SHAPES:
        ty=f'float{rows}x{cols}'
        lanes=[[('xyzw' if e==0 else 'wzyx')[i%4] for i in range(rows*cols)] for e in range(2)]
        parts=[['t.'+x for x in ls] for ls in lanes]
        ctors=[ty+'('+','.join(p)+')' for p in parts]
        braces=['{'+','.join('{'+','.join(p[r*cols:(r+1)*cols])+'}' for r in range(rows))+'}' for p in parts]
        control=f'{ty} a[2];a[0]={ctors[0]};a[1]={ctors[1]};'
        for form,items in [('explicit',ctors)]+([('braces',braces)] if cols>1 else []):
            body=f'{ty} a[2]={{'+','.join(items)+'};'
            for e in range(2):
                for row in range(rows):
                    ret=f'return float4(a[{e}][{row}]'+',1'*(4-cols)+');'
                    selected=lanes[e][row*cols:(row+1)*cols]
                    want=lambda t,ls=selected,c=cols:[t['xyzw'.index(x)] for x in ls]+[1.]*(4-c)
                    yield f'{rows}x{cols}_{form}_{e}_{row}',body+ret,control+ret,want
    yield 'once','float s=t.x;float2x2 a[2]={{{s++,2},{3,4}},{{s++,6},{7,8}}};return float4(a[0][0][0],a[1][0][0],s,1);','return float4(t.x,t.x+1,t.x+2,1);',lambda t:[t[0],t[0]+1,t[0]+2,1.]
    yield 'snapshot','float2x2 m=float2x2(t);float2x2 a[2]={m,m};m[0]=t.wz;return float4(a[0][0],a[1][1]);','return t;',lambda t:t
    yield 'overwrite','float2x2 a[2]={float2x2(t),float2x2(t.wzyx)};a[0]=float2x2(2*t);return float4(a[0][0],a[1][1]);','return float4(2*t.xy,t.yx);',lambda t:[2*t[0],2*t[1],t[1],t[0]]

REFUSALS = {
    'short':'float2x2 a[2]={float2x2(t)};',
    'long':'float2x2 a[1]={float2x2(t),float2x2(t)};',
    'shape':'float2x2 a[1]={float3x3(1,2,3,4,5,6,7,8,9)};',
    'short_row':'float2x2 a[1]={{{t.x},{t.z,t.w}}};',
    'long_row':'float2x2 a[1]={{{t.x,t.y,t.z},{t.z,t.w}}};',
    'debt_half_conversion':'float2x2 a[1]={half2x2(t)};',
    'debt_fixed_conversion':'float2x2 a[1]={fixed2x2(t)};',
    'debt_half_array':'half2x2 a[1]={half2x2(t)};',
    'debt_fixed_array':'fixed2x2 a[1]={fixed2x2(t)};',
    'debt_flat':'float2x2 a[1]={t.x,t.y,t.z,t.w};',
    'debt_one_wide_braces':'float1x1 a[1]={{{t.x}}};',
}

def source(stage,body):
    return 'float4 main(float4 t:%s):%s{%s}\n'%('TEXCOORD0' if stage=='fp' else 'POSITION','COLOR' if stage=='fp' else 'POSITION',body)

def check(compiler,work):
    assert fp_eval.self_test()
    vp.predication_selftest()
    errors=[];pairs=values=refusals=0
    for stage in ('fp','vp'):
        for name,body,control,want in cases():
            ok=0
            for mode,text in [('init',body),('control',control)]:
                src=work/f'{stage}_{name}_{mode}.cg';dst=src.with_suffix('.bin')
                src.write_text(source(stage,text));dst.unlink(missing_ok=True)
                p=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
                src.with_suffix('.log').write_text(p.stdout+p.stderr)
                if p.returncode or not dst.exists():
                    errors.append(f'{stage}/{name}/{mode}: refused rc={p.returncode}: {p.stderr.strip()}');continue
                ok+=1;blob=dst.read_bytes()
                for t in INPUTS:
                    try:
                        got=fp_eval.evaluate(blob,{'TEX0':t}) if stage=='fp' else vp.evaluate(blob,{},inputs={0:t},binary32=True,predication=True).get(0)
                        values+=1
                        if got!=want(t):errors.append(f'{stage}/{name}/{mode}: {got} != {want(t)}')
                    except Exception as e:errors.append(f'{stage}/{name}/{mode}: UNMODELLED {e}')
            if ok==2:pairs+=1
        for name,body in REFUSALS.items():
            src=work/f'{stage}_{name}.cg';dst=src.with_suffix('.bin')
            src.write_text(source(stage,body+'return float4(a[0][0],1,1);' if 'one_wide' not in name else body+'return float4(a[0][0],1,1,1);'));dst.unlink(missing_ok=True)
            p=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
            src.with_suffix('.log').write_text(p.stdout+p.stderr)
            if p.returncode==1 and not dst.exists() and '--extension=' not in p.stderr:refusals+=1
            else:errors.append(f'{stage}/{name}: expected clean refusal, rc={p.returncode}')
    for e in errors:print('FAIL',e)
    print(f'matrix array initializer: {pairs} pairs, {values} values, {refusals} refusals, {len(errors)} failures')
    return not errors and pairs==122 and values==732 and refusals==22

def main():
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--work',type=Path);a=ap.parse_args()
    if a.work:
        a.work.mkdir(parents=True,exist_ok=True)
        return 0 if check(a.compiler,a.work) else 1
    with tempfile.TemporaryDirectory(prefix='matrix-array-initializer-') as tmp:
        return 0 if check(a.compiler,Path(tmp)) else 1

if __name__=='__main__':raise SystemExit(main())
