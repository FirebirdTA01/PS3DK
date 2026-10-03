"""An unreachable missing-return placeholder must not hide two real returns."""
import argparse
import itertools
from pathlib import Path
import subprocess
import tempfile

from fp_eval import evaluate, Unmodelled

CASES = {
    'two_returns': ('if(p.x>0)return p;else return p*2;', lambda p: p if p[0]>0 else [2*x for x in p]),
    'shared_prefix': ('float4 q=p*2;if(p.x>0)return q;else return p;', lambda p: [2*x for x in p] if p[0]>0 else p),
    'different_lanes': ('if(p.y>0)return p.wzyx;else return p;', lambda p: list(reversed(p)) if p[1]>0 else p),
    'early_return_control': ('if(p.x>0)return p;return p*2;', lambda p: p if p[0]>0 else [2*x for x in p]),
}
DEBTS = {
    'live_undef': 'float4 q;if(p.x>0)return q;else return p;',
    'three_live_returns': 'if(p.x>0)return p;else if(p.y>0)return p*2;else return p*3;',
    'loop': 'while(p.x>0)p.x-=p.y;return p;',
}


def main():
    ap=argparse.ArgumentParser();ap.add_argument('compiler');ap.add_argument('--keep',type=Path);a=ap.parse_args()
    failures=[];checks=0
    with tempfile.TemporaryDirectory(prefix='dead-return-stub-') as tmp:
        work=a.keep or Path(tmp);work.mkdir(parents=True,exist_ok=True)
        for name,body in [(n,c[0]) for n,c in CASES.items()]+list(DEBTS.items())+[('vp_two_returns',CASES['two_returns'][0])]:
            src=work/(name+'.cg');dst=work/(name+'.bin')
            vertex=name=='vp_two_returns'
            src.write_text(('float4 main(float4 p:POSITION):POSITION{' if vertex else 'float4 main(float4 p:TEXCOORD0):COLOR{')+body+'}\n')
            assert not dst.exists()
            run=subprocess.run([a.compiler,'-p','sce_vp_rsx' if vertex else 'sce_fp_rsx','-e','main','--emit-container',str(dst),str(src)],capture_output=True,text=True,timeout=30)
            (work/(name+'.log')).write_text(run.stdout+run.stderr)
            try:
                if name in DEBTS or vertex:
                    assert run.returncode==1 and not dst.exists(),run.stderr
                    checks+=1
                else:
                    assert run.returncode==0 and dst.is_file(),run.stderr
                    for x,y in itertools.product((-.5,0,.5),repeat=2):
                        p=[x,y,.25,.75]
                        got=evaluate(dst.read_bytes(),{'TEX0':p});want=CASES[name][1](p)
                        assert got==want,(p,got,want)
                        checks+=1
                print('PASS',name)
            except (AssertionError,Unmodelled) as e:
                failures.append(name);print('FAIL',name,str(e))
    print(f'dead-return-stub: {checks} checks; {len(failures)} failures')
    return int(bool(failures))


if __name__=='__main__':raise SystemExit(main())
