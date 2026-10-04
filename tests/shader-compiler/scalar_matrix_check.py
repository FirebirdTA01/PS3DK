"""Scalar-to-float-matrix broadcast: shape, signs, and one evaluation."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from fp_eval import evaluate as evaluate_fp
from vp_pow_vector_check import evaluate as evaluate_vp
from uniform_container_check import check_container

INPUTS = [[0., .25, -.5, 1.5], [-.5, .25, -.5, 1.5], [1.5, -.25, .5, -1.], [-1.5, .5, .75, -1.]]

def cases():
    result = []
    for shape in ('2x2', '2x3', '3x2', '4x4'):
        rows, cols = map(int, shape.split('x'))
        observation = '+'.join(f'm[{r}][{c}]*{r*cols+c+1}.0' for r in range(rows) for c in range(cols))
        weight = sum(range(1, rows*cols+1))
        for form, statement in (
            ('init', f'float{shape} m=s;'),
            ('store', f'float{shape} m; m=s;'),
            ('constructor', f'float{shape} m=float{shape}(s);'),
            ('cast', f'float{shape} m=(float{shape})s;')):
            result.append((f'{form}-{shape}', 'float s=-p.x;'+statement+f'return float4({observation},s,0,1);',
                           lambda p,w=weight: [-p[0]*w,-p[0],0.,1.], ('fp','vp')))
    for name, scalar, value in [('zero','0',lambda p:0.), ('signed-constant','-2',lambda p:-2.)]:
        result.append((name, f'float2x2 m={scalar};return float4(m[0],m[1]);',
                       lambda p,f=value:[f(p)]*4, ('fp','vp')))
    result.append(('store-expression', 'float2x2 m;float2x2 n=(m=p.x);return float4(m[0],n[1]);',
                   lambda p:[p[0]]*4,('fp','vp')))
    for form, stmt in [('init','float2x2 m=s++;'),('store','float2x2 m;m=s++;'),('constructor','float2x2 m=float2x2(s++);'),('cast','float2x2 m=(float2x2)(s++);')]:
        result.append(('once-'+form, 'float s=p.x;'+stmt+'return float4(m[0][0],m[1][1],s,1);',
                       lambda p:[p[0],p[0],p[0]+1.,1.],('fp','vp')))
    result.append(('signed-variable','int s=int(p.x);float2x2 m=s;return float4(m[0],m[1]);',lambda p:[float(int(p[0]))]*4,('fp',)))
    result.append(('compound-add','float2x2 m=float2x2(1,2,3,4);m+=p.x;return float4(m[0],m[1]);',lambda p:[x+p[0] for x in [1.,2.,3.,4.]],('fp','vp')))
    return result

REFUSALS = {
    'vector-assignment':'float2x2 m=p;return float4(m[0],m[1]);',
    'half-target':'half2x2 m=p.x;return float4(m[0],m[1]);',
    'bool-target':'bool2x2 m=p.x;return float4(m[0],m[1]);',
    'int-target':'int2x2 m=p.x;return float4(m[0],m[1]);',
    'implicit-int-binding':'int s=p.x;float2x2 m=s;return float4(m[0],m[1]);',
    'implicit-int-arithmetic':'int s=p.x;int j=s+1;float2x2 m=j;return float4(m[0],m[1]);',
    'implicit-int-increment':'int s=p.x;float2x2 m=++s;return float4(m[0],m[1]);',
    'store-implicit-int-binding':'int s=p.x;float2x2 m;m=s;return float4(m[0],m[1]);',
    'store-implicit-int-arithmetic':'int s=p.x;int j=s+1;float2x2 m;m=j;return float4(m[0],m[1]);',
    'store-implicit-int-increment':'int s=p.x;float2x2 m;m=++s;return float4(m[0],m[1]);',
}
UNIT_REFUSALS = {
    'global-default': ('float2x2 g=1;', 'return float4(g[0],g[1]);'),
    'return-conversion': ('float2x2 f(float x){return x;}', 'float2x2 m=f(p.x);return float4(m[0],m[1]);'),
    'parameter-default': ('float4 f(float2x2 m=1){return float4(m[0],m[1]);}', 'return f();'),
    'argument-conversion': ('float4 f(float2x2 m){return float4(m[0],m[1]);}', 'return f(p.x);'),
    'parameter-store': ('float4 f(float2x2 m,float x){m=x;return float4(m[0],m[1]);}', 'return f(float2x2(1,2,3,4),p.x);'),
    'member-store': ('struct S{float2x2 m;};', 'S s;s.m=p.x;return float4(s.m[0],s.m[1]);'),
}

def run(compiler, root):
    root.mkdir(parents=True, exist_ok=True)
    failures=[]; reports=[]
    def compile_case(name, stage, body, expected, prefix=''):
        source=root/(name+'-'+stage+'.cg'); dest=source.with_suffix('.bin')
        source.write_text(prefix+'\nfloat4 main(float4 p:TEXCOORD0):'+('COLOR' if stage=='fp' else 'POSITION')+'{'+body+'}\n')
        if dest.exists(): dest.unlink()
        proc=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dest),str(source)],capture_output=True,text=True,timeout=20)
        source.with_suffix('.log').write_text(proc.stdout+proc.stderr)
        report={'name':name,'stage':stage,'rc':proc.returncode}
        try:
            if expected is None:
                assert proc.returncode==1 and not dest.exists(), 'expected refusal without container'
            else:
                assert proc.returncode==0 and dest.exists(), proc.stderr
                blob=dest.read_bytes()
                assert not check_container(blob)['issues'], 'container inconsistent'
                values=[evaluate_fp(blob,{'TEX0':p}) if stage=='fp' else evaluate_vp(blob,{},inputs={8:p},binary32=True)[0] for p in INPUTS]
                want=[expected(p) for p in INPUTS]
                assert values==want, f'values {values}, expected {want}'
                report['values']=values
        except Exception as error:
            failures.append(name+'-'+stage+': '+str(error))
        reports.append(report)
    for name,body,expected,stages in cases():
        for stage in stages: compile_case(name,stage,body,expected)
    for name,body in REFUSALS.items():
        for stage in ('fp','vp'): compile_case(name,stage,body,None)
    for name,(prefix,body) in UNIT_REFUSALS.items():
        for stage in ('fp','vp'): compile_case(name,stage,body,None,prefix)
    (root/'RESULT.json').write_text(json.dumps({'reports':reports,'failures':failures},indent=2)+'\n')
    for failure in failures: print('FAIL',failure)
    print(f'scalar-matrix: {len(reports)} programs, {len(failures)} failures')
    return int(bool(failures))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler'); parser.add_argument('--keep',type=Path)
    args=parser.parse_args()
    if args.keep: raise SystemExit(run(str(Path(args.compiler).resolve()),args.keep))
    with tempfile.TemporaryDirectory(prefix='scalar-matrix-') as temp:
        raise SystemExit(run(str(Path(args.compiler).resolve()),Path(temp)))
