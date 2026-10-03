"""Typed bool matrices: construction, conversion and static row consumption.

Reference probes accept these constructors and hqx's weighted bool rows in
both profiles. Whole-matrix logic/comparisons and dynamic rows are refused.
The FP values and the two VP controls use independent nonzero/row formulas;
no proprietary compiler or output is needed by this regression.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from fp_eval import evaluate as evaluate_fp, Unmodelled
from vp_pow_vector_check import evaluate as evaluate_vp
from uniform_container_check import check_container


INPUTS = [
    ([0.0, .25, -.5, 1.5], [.5, 0.0, -.25, -1.5]),
    ([-.25, -0.0, 1.0, -.5], [0.0, .125, 0.0, .5]),
    ([0.0]*4, [0.0]*4),
    ([-1.5, .5, .75, -1.0], [-1.5, -.5, .75, 1.0]),
]


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def truth(value):
    return float(value != 0)


def make_cases():
    cases = []
    def add(name, body, twin, expected, profiles=('sce_fp_rsx',)):
        for profile in profiles:
            cases.append(dict(name=name+'-'+profile, profile=profile, body=body,
                              twin=twin, expected=[expected(p, q) for p, q in INPUTS]))

    for rows in range(2, 5):
        for cols in range(2, 5):
            selectors = [('p' if (r+c)%2 == 0 else 'q', (r+2*c)%4)
                         for r in range(rows) for c in range(cols)]
            args = [f'({base}.{"xyzw"[lane]}!=0)' for base, lane in selectors]
            terms = [f'float(b[{r}][{c}])*{float(1 << (r*cols+c))}'
                     for r in range(rows) for c in range(cols)]
            direct = [f'float({arg})*{float(1 << i)}' for i, arg in enumerate(args)]
            body = f'bool{rows}x{cols} b=bool{rows}x{cols}('+','.join(args)+');return float4('+ '+'.join(terms)+',0,0,1);'
            twin = 'return float4('+ '+'.join(direct)+',0,0,1);'
            def expected(p, q, picks=selectors):
                return [sum((1 << i)*truth((p if base == 'p' else q)[lane])
                            for i, (base, lane) in enumerate(picks)), 0, 0, 1]
            add(f'scalars-{rows}x{cols}', body, twin, expected)

    both = ('sce_fp_rsx', 'sce_vp_rsx')
    numeric_twin = 'return float4(p.x!=0,p.y!=0,p.z!=0,p.w!=0);'
    add('numeric-scalars', 'bool2x2 b=bool2x2(p.x,p.y,p.z,p.w);return float4(b[0],b[1]);',
        numeric_twin, lambda p,q: list(map(truth,p)), both)
    add('numeric-rows', 'bool2x3 b=bool2x3(p.xyz,q.zyx);return float4(float3(b[0])+2*float3(b[1]),1);',
        'return float4(float3(p.xyz!=0)+2*float3(q.zyx!=0),1);',
        lambda p,q: [truth(p[i])+2*truth(q[2-i]) for i in range(3)]+[1])
    add('numeric-braces', 'bool2x2 b={{p.x,p.y},{p.z,p.w}};return float4(b[0],b[1]);',
        numeric_twin, lambda p,q: list(map(truth,p)))
    add('numeric-matrix-cast', 'float2x2 a=float2x2(p.xy,p.zw);bool2x2 b=bool2x2(a);return float4(b[0],b[1]);',
        numeric_twin, lambda p,q: list(map(truth,p)))
    add('numeric-splat', 'bool2x2 b=bool2x2(p.x);return float4(b[0],b[1]);',
        'return float4(p.x!=0,p.x!=0,p.x!=0,p.x!=0);', lambda p,q: [truth(p[0])]*4)
    add('numeric-constants', 'bool2x2 b=bool2x2(0.0,.25,-.5,1.5);return float4(b[0],b[1]);',
        'return float4(0,1,1,1);', lambda p,q: [0,1,1,1])
    add('row-write', 'bool2x2 b=bool2x2(p.x!=0,p.y!=0,p.z!=0,p.w!=0);b[1]=q.xy;return float4(b[0],b[1]);',
        'return float4(p.x!=0,p.y!=0,q.x!=0,q.y!=0);',
        lambda p,q: [truth(p[0]),truth(p[1]),truth(q[0]),truth(q[1])])
    add('lane-write', 'bool2x2 b=bool2x2(p.x!=0,p.y!=0,p.z!=0,p.w!=0);b[0][1]=q.x;return float4(b[0],b[1]);',
        'return float4(p.x!=0,q.x!=0,p.z!=0,p.w!=0);',
        lambda p,q: [truth(p[0]),truth(q[0]),truth(p[2]),truth(p[3])])
    add('row-logical', 'bool2x3 b=bool2x3(p.xyz!=0,q.xyz!=0);bool3 r=(b[0]&&!b[1])||(b[0]==b[1]);return float4(r,1);',
        'bool3 a=p.xyz!=0;bool3 b=q.xyz!=0;bool3 r=(a&&!b)||(a==b);return float4(r,1);',
        lambda p,q: [float((p[i]!=0 and q[i]==0) or ((p[i]!=0)==(q[i]!=0))) for i in range(3)]+[1])
    def hqx(p,q):
        flags = [p[0]>q[0],p[1]>q[1],p[2]>q[2],p[3]>q[3],False,p[0]<q[0],p[1]<q[1],p[2]<q[2],p[3]<q[3]]
        return [sum(v*w for v,w in zip(flags,[1,2,4,8,0,16,32,64,128])),0,0,1]
    add('hqx-rows',
        'bool3x3 b=bool3x3(p.x>q.x,p.y>q.y,p.z>q.z,p.w>q.w,false,p.x<q.x,p.y<q.y,p.z<q.z,p.w<q.w);'
        'return float4(dot(b[0],float3(1,2,4))+dot(b[1],float3(8,0,16))+dot(b[2],float3(32,64,128)),0,0,1);',
        'bool3 a=bool3(p.x>q.x,p.y>q.y,p.z>q.z);bool3 b=bool3(p.w>q.w,false,p.x<q.x);bool3 c=bool3(p.y<q.y,p.z<q.z,p.w<q.w);'
        'return float4(dot(a,float3(1,2,4))+dot(b,float3(8,0,16))+dot(c,float3(32,64,128)),0,0,1);', hqx, both)

    decl = 'bool2x2 a=bool2x2(p.x!=0,p.y!=0,p.z!=0,p.w!=0);bool2x2 b=bool2x2(q.x!=0,q.y!=0,q.z!=0,q.w!=0);'
    refused = {name: decl+f'bool2x2 c={expr};return float4(c[0],c[1]);'
               for name,expr in [('whole-and','a&&b'),('whole-or','a||b'),('whole-not','!a'),
                                 ('whole-equal','a==b'),('whole-unequal','a!=b')]}
    refused.update({
        'negative-row': decl+'return float4(a[-1],0,1);',
        'past-end-row': decl+'return float4(a[2],0,1);',
        'dynamic-row': decl+'return float4(a[int(q.z)],0,1);',
        'matrix-comparison': 'float2x2 a=float2x2(p.xy,p.zw);bool2x2 b=a!=0;return float4(b[0],b[1]);',
        'implicit-matrix-init': 'float2x2 a=float2x2(p.xy,p.zw);bool2x2 b=a;return float4(b[0],b[1]);',
        'implicit-matrix-store': 'float2x2 a=float2x2(p.xy,p.zw);bool2x2 b=bool2x2(false);b=a;return float4(b[0],b[1]);',
        'scalar-matrix-cast': 'bool2x2 b=(bool2x2)p.x;return float4(b[0],b[1]);',
    })
    for name,body in refused.items():
        for profile in both:
            cases.append(dict(name=name+'-'+profile, body=body, profile=profile, refusal=True))
    for profile in both:
        cases.append(dict(name='entry-uniform-'+profile, profile=profile, refusal=True,
                          parameters=',uniform bool2x2 b', body='return float4(b[0],b[1]);'))
        cases.append(dict(name='global-uniform-'+profile, profile=profile, refusal=True,
                          prefix='uniform bool2x2 b;', body='return float4(b[0],b[1]);'))
    return cases


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    cases = make_cases()
    for case in cases:
        semantic = 'COLOR' if case['profile']=='sce_fp_rsx' else 'POSITION'
        header = case.get('prefix','')+f'float4 main(float4 p:TEXCOORD0,float4 q:TEXCOORD1'+case.get('parameters','')+f'):{semantic}'
        for suffix, body in (('',case['body']), ('-twin',case.get('twin'))):
            if body is not None:
                (root/(case['name']+suffix+'.cg')).write_text(header+'{'+body+'}\n')
    (root/'manifest.json').write_text(json.dumps(cases, indent=2)+'\n')
    return cases


def run(compiler, root):
    reports,failures=[],[]
    for case in prepare(root):
        for suffix in ('',) if case.get('refusal') else ('','-twin'):
            name=case['name']+suffix
            dst=root/(name+'.bin')
            if dst.exists(): dst.unlink()
            try:
                result=subprocess.run([compiler,'-p',case['profile'],'--emit-container',str(dst),str(root/(name+'.cg'))],
                                      capture_output=True,text=True,timeout=20)
                (root/(name+'.log')).write_text(result.stdout+result.stderr)
                report=dict(name=name,status=result.returncode)
                if case.get('refusal'):
                    require(result.returncode==1 and not dst.exists(), name+': expected exit1/no container')
                    report['refusal_verified']=True
                else:
                    require(result.returncode==0 and dst.exists(), name+': compile failed: '+result.stderr)
                    blob=dst.read_bytes()
                    require(not check_container(blob)['issues'], name+': container inconsistency')
                    if case['profile']=='sce_fp_rsx':
                        got=[evaluate_fp(blob,{'TEX0':p,'TEX1':q}) for p,q in INPUTS]
                    else:
                        got=[evaluate_vp(blob,{},inputs={8:p,9:q},binary32=True)[0] for p,q in INPUTS]
                    require(got==case['expected'], name+': values '+str(got)+' expected '+str(case['expected']))
                    report['values']=got
                reports.append(report)
            except (AssertionError,Unmodelled,ValueError,KeyError,subprocess.TimeoutExpired) as error:
                failures.append(name+': '+str(error))
    (root/'RESULT.json').write_text(json.dumps(dict(reports=reports,failures=failures),indent=2)+'\n')
    require(not failures,'\n'.join(failures))
    print('PASS: bool-matrix (typed constructors, nonzero values, FP/VP hqx twins, whole-op/bounds refusals)')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler',nargs='?')
    parser.add_argument('--prepare-only',type=Path)
    parser.add_argument('--work-dir',type=Path)
    args=parser.parse_args()
    if args.prepare_only:
        cases=prepare(args.prepare_only.resolve())
        print(f'{len(cases)} cases prepared at {args.prepare_only.resolve()}')
        return
    require(args.compiler is not None,'compiler path required')
    compiler=str(Path(args.compiler).resolve())
    if args.work_dir:
        run(compiler,args.work_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='bool-matrix-') as temp:
            run(compiler,Path(temp))


if __name__=='__main__':
    try:
        main()
    except AssertionError as error:
        raise SystemExit('FAIL: '+str(error))
