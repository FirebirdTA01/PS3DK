"""By-value matrix-bearing struct arguments retain caller leaf values.

Uniforms are patched by source name with asymmetric rows. Expected products
come from arithmetic here, not the container's register allocation. Reference
mode uses independently authored sources with the private compiler's CLI.
The --loops integration row also requires the VP branch-loop implementation
and its evaluator; the standalone matrix-argument slice predates that work.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile
from vp_pow_vector_check import evaluate

TYPE = 'struct T { float4 g; float4x4 m; }; '
HELP = 'float4 h(T t,float4 v) { return mul(v,t.m)+t.g; } '
MAIN = 'float4 main(float4 p:POSITION):POSITION { '
CASES = {
    'plain': (TYPE+'uniform T tu; '+HELP+MAIN+'return h(tu,p); }', 'tu', 'row'),
    'element': (TYPE+'uniform T tu[2]; '+HELP+MAIN+'return h(tu[1],p); }', 'tu[1]', 'row'),
    'same_name': (TYPE+'uniform T tu; '+HELP.replace('T t,','T tu,').replace('t.m','tu.m').replace('t.g','tu.g')+MAIN+'return h(tu,p); }', 'tu', 'row'),
    'forward': (TYPE+'uniform T tu; '+HELP+'float4 f(T t,float4 p) { return h(t,p); } '+MAIN+'return f(tu,p); }', 'tu', 'row'),
    'local_copy': (TYPE+'uniform T tu; '+HELP+MAIN+'T q=tu; return h(q,p); }', 'tu', 'row'),
    'argument_order': (TYPE+'uniform T tu; '+HELP+MAIN+'return h(tu,(tu.g=p)); }', 'tu', 'argument_order'),
    'private_write': (TYPE+'uniform T tu; float4 h(T t,float4 v) { t.g=v; return mul(v,t.m)+t.g; } '+MAIN+'return h(tu,p)+tu.g; }', 'tu', 'private'),
    'private_matrix': (TYPE+'uniform T tu; float4 h(T t,float4 v) { t.m=float4x4(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16); return mul(v,t.m); } '+MAIN+'return h(tu,p)+mul(p,tu.m)+tu.g; }', 'tu', 'private_matrix'),
    'nested': ('struct I { float4x4 m; }; struct T { float4 g; I a; }; uniform T tu; '+HELP.replace('t.m','t.a.m')+MAIN+'return h(tu,p); }', 'tu', 'nested'),
    'rect_row': ('struct T { float4 g; float2x3 m; }; uniform T tu; float4 h(T t,float4 v) { return float4(mul(v.xy,t.m),1)+t.g; } '+MAIN+'return h(tu,p); }', 'tu', 'rect_row'),
    'rect_col': ('struct T { float4 g; float2x3 m; }; uniform T tu; float4 h(T t,float4 v) { return float4(mul(t.m,v.xyz),.5,1)+t.g; } '+MAIN+'return h(tu,p); }', 'tu', 'rect_col'),
}
REFUSALS = {
    'dynamic_element': TYPE+'uniform T tu[2]; '+HELP+MAIN+'return h(tu[int(p.x)],p); }',
    'effectful_element': TYPE+'uniform T tu[2]; '+HELP+MAIN+'int i=0; return h(tu[i++],p); }',
}


def agrees(got, want):
    return got is not None and len(got)==len(want)==4 and all(
        math.isfinite(a) and abs(a-b)<=1e-5*max(1.,abs(b)) for a,b in zip(got,want))


def main():
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--reference',action='store_true')
    ap.add_argument('--loops',action='store_true',help='also judge a helper called inside a VP loop')
    ap.add_argument('--output',type=Path);args=ap.parse_args()
    temporary=None if args.output else tempfile.TemporaryDirectory(prefix='vp-struct-matrix-')
    root=args.output or Path(temporary.name)
    if args.output: root.mkdir(parents=True,exist_ok=False)
    toolhash=hashlib.sha256(Path(args.compiler).read_bytes()).hexdigest()
    assert not agrees([math.nan]*4,[0.]*4) and not agrees([],[0.]*4)
    failures=[];results=[]
    cases=dict(CASES)
    if args.loops:
        cases['helper_inside_loop']=(TYPE+'uniform T tu[2]; uniform float n; '+HELP+MAIN+
            'float4 s=p; for(int i=0;i<n;++i) s=h(tu[1],s); return s; }','tu[1]','loop')
    for name,(source,prefix,mode) in cases.items():
        src=root/(name+'.cg');out=root/(name+'.vpo');src.write_text(source+'\n')
        argv=[args.compiler,'-p','sce_vp_rsx','-e','main','-o' if args.reference else '--emit-container',str(out),str(src)]
        p=subprocess.run(argv,capture_output=True,timeout=45)
        (root/(name+'.log')).write_bytes(p.stdout+p.stderr)
        record={'case':name,'argv':argv,'rc':p.returncode,'tool_sha256':toolhash,
                'source_sha256':hashlib.sha256(src.read_bytes()).hexdigest()};results.append(record)
        if p.returncode or not out.exists() or not out.stat().st_size:
            failures.append(name+': no admitted container');continue
        blob=out.read_bytes();record['sha256']=hashlib.sha256(blob).hexdigest()
        wrong=0;checks=0
        for seed in (0,1,3):
            m=[[((r+1)*3+c+seed)/4 for c in range(4)] for r in range(4)]
            g=[.25+seed,.5,-.75,1.25]
            uniforms={prefix+'.g':g}
            if mode=='loop': uniforms['n']=[float(seed),0.,0.,0.]
            path=prefix+('.a.m' if mode=='nested' else '.m')
            uniforms.update({path+'[%d]'%r:row for r,row in enumerate(m)})
            for v in ([1.,-2.,.5,1.],[-.25,.75,2.,-1.],[0.,1.,-1.,.25]):
                if mode=='rect_row': value=[sum(v[r]*m[r][c] for r in range(2)) for c in range(3)]+[1.]
                elif mode=='rect_col': value=[sum(m[r][c]*v[c] for c in range(3)) for r in range(2)]+[.5,1.]
                else: value=[sum(v[r]*m[r][c] for r in range(4)) for c in range(4)]
                want=[a+b for a,b in zip(value,g)]
                if mode=='argument_order': want=[a+b for a,b in zip(value,v)]
                if mode=='private': want=[a+b for a,b in zip(want,v)]
                if mode=='private_matrix': want=[want[c]+sum(v[r]*(r*4+c+1) for r in range(4)) for c in range(4)]
                if mode=='loop':
                    want=list(v)
                    for _ in range(seed):
                        want=[sum(want[r]*m[r][c] for r in range(4))+g[c] for c in range(4)]
                options={'branches':True,'predication':True,'indexed_constants':True} if mode=='loop' else {}
                try: got=evaluate(blob,uniforms,inputs={0:v},binary32=True,**options).get(0)
                except (AssertionError,KeyError,ValueError) as e:
                    failures.append(name+': UNJUDGED '+str(e));break
                checks+=1
                if not agrees(got,want):
                    failures.append(name+': got %s want %s'%(got,want));break
                # Same checker must reject an incorrect row/column result.
                wrong+=not agrees([want[0]+1]+want[1:],want)
        record['value_checks']=checks
        if checks!=9 or wrong!=9: failures.append(name+': incomplete values/controls')
        print(name,checks,'value checks')
    if not args.reference:
        for name,source in REFUSALS.items():
            src=root/(name+'.cg');out=root/(name+'.vpo');src.write_text(source+'\n')
            argv=[args.compiler,'-p','sce_vp_rsx','-e','main','--emit-container',str(out),str(src)]
            p=subprocess.run(argv,capture_output=True,timeout=45)
            log=p.stdout+p.stderr;(root/(name+'.log')).write_bytes(log)
            ok=p.returncode==1 and not out.exists() and b'struct-matrix-argument' in log
            results.append({'case':name,'argv':argv,'rc':p.returncode,'refused':ok,'tool_sha256':toolhash})
            if not ok: failures.append(name+': expected named refusal and no container')
    (root/'results.json').write_text(json.dumps(results,indent=2))
    for failure in failures: print('FAIL:',failure)
    print('vp-struct-matrix-argument:', 'FAIL' if failures else 'PASS')
    return bool(failures)


if __name__=='__main__': raise SystemExit(main())
