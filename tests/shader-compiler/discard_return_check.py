"""One surviving return, other exits killed: judge kill decisions AND colour."""
import argparse
import itertools
from pathlib import Path
import subprocess
import tempfile

from fp_eval import evaluate, Unmodelled, _container, _ins, _src, MOV, INPUT

CASES = {
    'return_else_kill': ('if(p.x>0)return p;else discard;', lambda p: p if p[0]>0 else None),
    'nested_kills': ('if(p.x>0){if(p.y>0)return p;else discard;}else discard;',
                     lambda p: p if p[0]>0 and p[1]>0 else None),
    'shared_prefix': ('float4 q=p*2;if(p.x>0)return q;else discard;',
                      lambda p: [2*x for x in p] if p[0]>0 else None),
    'single_return_control': ('if(p.x<=0)discard;return p;', lambda p: p if p[0]>0 else None),
}
DEBTS = {
    'live_undefined_output': 'float4 q;if(p.x>0)return q;else discard;',
    # Guard construction rejects a terminal discard without a successor.
    'terminal_discard_without_successor': 'if(p.x>0)return p;discard;',
    # This front-end shape retains an unreachable nonempty merge block.
    'kill_else_return_unreachable_merge': 'if(p.x<=0)discard;else return p;',
    'store_on_killed_arm': 'if(p.x>0){discard;return p;}return p*2;',
    'multiple_survivors': 'if(p.x>0)return p;else if(p.y>0)return p*2;else return p*3;',
    'two_survivors_and_kill': 'if(p.x>0)return p;else if(p.y>0)return p*2;else discard;',
    'all_returns_killed': 'if(p.x>0){discard;return p;}else{discard;return p*2;}',
}


def check_evaluator():
    # Encoded, compiler-independent controls: EQ and NE, sign/zero paths,
    # missing CC and the old evaluator's refusal of KIL remain covered.
    cc = _ins(MOV, 63, 1, [_src(INPUT)], sel=4, out_none=1)
    cc[0] |= 1 << 8
    for cond in (2, 5):
        kill = _ins(0x12, 63, 3, [], out_none=1)
        kill[1] = cond << 18  # replicated CC.x
        tail = _ins(MOV, 0, 15, [_src(INPUT)], sel=4, end=1)
        blob = _container(cc + kill + tail)
        for x in (-0.5, 0.0, 0.5):
            p = [x, 0.25, 0.5, 1.0]
            killed = (x == 0) if cond == 2 else (x != 0)
            assert evaluate(blob, {'TEX0': p}, scalar_kill=True) == (None if killed else p)
        for broken, enabled in ((_container(kill+tail), True), (blob, False)):
            try:
                evaluate(broken, {'TEX0': [0.5]*4}, scalar_kill=enabled)
            except Unmodelled:
                pass
            else:
                raise AssertionError('unmodelled kill was silently evaluated')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('compiler')
    ap.add_argument('--keep', type=Path)
    a = ap.parse_args()
    check_evaluator()
    failures, checks = [], 0
    with tempfile.TemporaryDirectory(prefix='discard-return-') as tmp:
        work = a.keep or Path(tmp)
        work.mkdir(parents=True, exist_ok=True)
        for stage in ('fp', 'vp'):
            for name, body in [(n, c[0]) for n,c in CASES.items()] + list(DEBTS.items()):
                label = name+'_'+stage
                src, dst = work/(label+'.cg'), work/(label+'.bin')
                si, so = ('TEXCOORD0','COLOR') if stage=='fp' else ('POSITION','POSITION')
                src.write_text(f'float4 main(float4 p:{si}):{so}{{{body}}}\n')
                assert not dst.exists(), dst
                run = subprocess.run([a.compiler,'-p','sce_'+stage+'_rsx','-e','main',
                                      '--emit-container',str(dst),str(src)],
                                     capture_output=True,text=True,timeout=30)
                (work/(label+'.log')).write_text(run.stdout+run.stderr)
                try:
                    if stage=='vp' or name in DEBTS:
                        assert run.returncode==1 and not dst.exists(), run.stderr
                        assert ('refusing' in run.stderr or 'fragment shaders' in run.stderr
                                or 'ends in a discard' in run.stderr), run.stderr
                        checks += 1
                    else:
                        assert run.returncode==0 and dst.is_file(), run.stderr
                        blob = dst.read_bytes()
                        for x,y in itertools.product((-0.5,0.0,0.5), repeat=2):
                            p = [x,y,0.25,0.75]
                            got = evaluate(blob, {'TEX0':p}, scalar_kill=True)
                            want = CASES[name][1](p)
                            assert got==want, (p,got,want)
                            checks += 1
                    print('PASS',label)
                except (AssertionError,Unmodelled) as e:
                    failures.append(label)
                    print('FAIL',label,str(e))
    print(f'discard-return: {checks} checks; {len(failures)} failures')
    return int(bool(failures))


if __name__=='__main__':
    raise SystemExit(main())
