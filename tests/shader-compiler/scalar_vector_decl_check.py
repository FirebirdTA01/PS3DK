"""Scalar local initializers broadcast once, as an explicit vector constructor.

Dyadic samples pin shape, conversion of constants, copy independence and side
effects. They make no claim about the pending half/fixed precision policy.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import fp_eval
import vp_pow_vector_check as vp_eval

INPUTS = [[-.5, .25, 1.5, 2.], [0., -.5, .25, 1.], [.25, 1.5, -.5, .5]]
CASES = []
for width in (2, 3, 4):
    tail = 'float4(v,v)' if width == 2 else 'float4(v,1)' if width == 3 else 'v.wzyx'
    for name, expr, value in [('constant', '2', lambda t: 2.),
                              ('lane', 't.y', lambda t: t[1]),
                              ('computed', '-t.x*2', lambda t: -t[0]*2),
                              ('quotient', 't.x/2.0', lambda t: t[0]/2)]:
        template = 'float%d v=INIT; return %s;' % (width, tail)
        CASES.append((name+str(width), template.replace('INIT',expr),
                      template.replace('INIT','float%d(%s)' % (width,expr)),
                      lambda t,w=width,f=value: [f(t)]*3+[1.] if w==3 else [f(t)]*4,
                      ('fp','vp')))
CASES += [
    ('once', 'float x=t.x; float3 v=x++; return float4(v,x);',
     'float x=t.x; float3 v=float3(x++); return float4(v,x);',
     lambda t: [t[0]]*3+[t[0]+1], ('fp','vp')),
    ('copy', 'float3 a=t.y; float3 b=a; a.z=t.z; return float4(b,a.z);',
     'float3 a=float3(t.y); float3 b=a; a.z=t.z; return float4(b,a.z);',
     lambda t: [t[1]]*3+[t[2]], ('fp','vp')),
    ('int_constant', 'int3 v=-2; return float4(v,1);',
     'int3 v=int3(-2); return float4(v,1);', lambda t: [-2.,-2.,-2.,1.], ('fp','vp')),
    ('half_constant', 'half3 v=0.5; return float4(v,1);',
     'half3 v=half3(0.5); return float4(v,1);', lambda t: [.5,.5,.5,1.], ('fp',)),
    ('bool_control', 'bool3 v=true; return float4(v,1);',
     'bool3 v=bool3(true); return float4(v,1);', lambda t: [1.,1.,1.,1.], ('fp','vp')),
]

def source(body,stage):
    return 'float4 main(float4 t:%s):%s {%s}\n' % (
        'TEXCOORD0' if stage=='fp' else 'POSITION', 'COLOR' if stage=='fp' else 'POSITION',body)

def check(compiler,work):
    assert fp_eval.self_test()
    vp_eval.predication_selftest()
    failures=[];values=0;twins=0
    for name,body,control,want,stages in CASES:
        for stage in stages:
            blobs={}
            for mode,text in [('implicit',body),('constructor',control)]:
                stem=work/(name+'-'+stage+'-'+mode)
                src=stem.with_suffix('.cg');dst=stem.with_suffix('.bin')
                src.write_text(source(text,stage));dst.unlink(missing_ok=True)
                p=subprocess.run([compiler,'-p','sce_'+stage+'_rsx','--emit-container',str(dst),str(src)],
                                 capture_output=True,text=True,timeout=30)
                stem.with_suffix('.log').write_text(p.stdout+p.stderr)
                if p.returncode!=0 or not dst.exists():
                    failures.append('%s/%s/%s refused rc=%d: %s' % (name,stage,mode,p.returncode,p.stderr.strip()))
                    continue
                blobs[mode]=dst.read_bytes()
                for t in INPUTS:
                    got=(fp_eval.evaluate(blobs[mode],{'TEX0':t}) if stage=='fp' else
                         vp_eval.evaluate(blobs[mode],{},inputs={0:t},binary32=True,predication=True).get(0))
                    values+=1
                    if got!=want(t):failures.append('%s/%s/%s input=%s got=%s want=%s' % (name,stage,mode,t,got,want(t)))
            if len(blobs)==2:
                twins+=1
                if blobs['implicit']!=blobs['constructor']:failures.append(name+'/'+stage+': constructor byte twin differs')
    for row in failures:print('FAIL:',row)
    print('scalar-vector-decl: %s twins=%d values=%d' % ('FAIL' if failures else 'PASS',twins,values))
    return int(bool(failures))

def main():
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--work',type=Path);args=ap.parse_args()
    if args.work:
        args.work.mkdir(parents=True,exist_ok=True)
        return check(args.compiler,args.work)
    with tempfile.TemporaryDirectory(prefix='scalar-vector-decl-') as tmp:return check(args.compiler,Path(tmp))

if __name__=='__main__':raise SystemExit(main())
