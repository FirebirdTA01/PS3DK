"""Literal FP entry conditions select one arm without losing scope or return."""
import argparse
import itertools
from pathlib import Path
import subprocess
import tempfile

from fp_eval import evaluate, Unmodelled

CASES = {
    'false_after_join': ('float4 q=p;if(p.x>0)q=p*2;else q=p*3;if(0.0)return p;return q;', lambda p: [x*(2 if p[0]>0 else 3) for x in p]),
    'true_after_join': ('float4 q=p;if(p.x>0)q=p*2;else q=p*3;if(1.0)return q;return p;', lambda p: [x*(2 if p[0]>0 else 3) for x in p]),
    'false_else_scope': ('float4 q=p;if(false){return p*4;}else{float4 q=p*2;p=q;}return p;', lambda p: [x*2 for x in p]),
    'true_nested_return': ('if(true){{float4 q=p*2;return q;}}return p*3;', lambda p: [x*2 for x in p]),
    'false_discard': ('if(0)discard;return p;', lambda p: p),
    'runtime_control': ('if(p.x>0)return p;else return p*2;', lambda p: p if p[0]>0 else [x*2 for x in p]),
    'side_effect_condition': ('float k=0;if(k=1)p*=2;return p+k;', lambda p: [x*2+1 for x in p]),
    'true_scope_unwinds': ('float4 q=p;if(true){float4 q=p*3;p=q;}return p+q;', lambda p: [x*4 for x in p]),
    'false_int': ('float4 q=p;if(p.y>0)q=p*2;else q=p*3;if(0)return p;return q;', lambda p: [x*(2 if p[1]>0 else 3) for x in p]),
    'nonzero_int': ('float4 q=p;if(p.y>0)q=p*2;else q=p*3;if(2)return q;return p;', lambda p: [x*(2 if p[1]>0 else 3) for x in p]),
}


def main():
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--keep',type=Path);a=ap.parse_args()
    failures=[];checks=0
    with tempfile.TemporaryDirectory(prefix='entry-literal-branch-') as tmp:
        work=a.keep or Path(tmp);work.mkdir(parents=True,exist_ok=True)
        rows=[(n,b,'fp',expected) for n,(b,expected) in CASES.items()]
        rows += [('unreachable_type_error','if(false){return undeclared;}return p;','fp',None),
                 ('vp_control',CASES['false_after_join'][0],'vp',None)]
        for name,body,stage,expected in rows:
            src=work/(name+'.cg');dst=work/(name+'.bin')
            si,so=('TEXCOORD0','COLOR') if stage=='fp' else ('POSITION','POSITION')
            src.write_text('float4 main(float4 p:'+si+'):'+so+'{'+body+'}\n')
            assert not dst.exists()
            run=subprocess.run([a.compiler,'-p','sce_'+stage+'_rsx','-e','main','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
            (work/(name+'.log')).write_text(run.stdout+run.stderr)
            try:
                if expected is None:
                    assert run.returncode==1 and not dst.exists(),run.stderr
                    if name=='unreachable_type_error':assert 'undeclared' in run.stderr,run.stderr
                    checks+=1
                else:
                    assert run.returncode==0 and dst.is_file(),run.stderr
                    for x,y in itertools.product((-.5,0,.5),repeat=2):
                        p=[x,y,.25,.75];got=evaluate(dst.read_bytes(),{'TEX0':p});want=expected(p)
                        assert got==want,(p,got,want)
                        checks+=1
                print('PASS',name)
            except (AssertionError,Unmodelled) as e:
                failures.append(name);print('FAIL',name,str(e))
    print(f'entry-literal-branch: {checks} checks; {len(failures)} failures')
    return int(bool(failures))


if __name__=='__main__':raise SystemExit(main())
