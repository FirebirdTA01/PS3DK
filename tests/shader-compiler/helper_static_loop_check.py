#!/usr/bin/env python3
"""Reference-measured helper expansion twins; unsupported forms remain named gaps.

Mutable/dynamic bounds, dependency writes and control-flow exits are reference
accepts but intentionally refused here - except the FRAGMENT rows a hardware
loop now compiles (FP-LOOP slice 1), which are judged by value ("accept": TEX0
inputs and the colour each must produce, on fp_eval). The existing 64-trip
bound is OUR resource limit, not a reference unroll heuristic. All sources are
independent.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

def main():
    compiler=str(Path(sys.argv[1]).resolve())
    repo=Path(__file__).resolve().parents[2]
    rows=json.loads(Path(__file__).with_name('helper_static_loop_cases.json').read_text(encoding='utf-8'))
    root=repo/'.local/tmp';root.mkdir(parents=True,exist_ok=True)
    twins=refusals=accepts=0
    with tempfile.TemporaryDirectory(prefix='helper-loop-',dir=root) as tmp:
        def compile_one(name, source, profile, refusal=None):
            src=Path(tmp)/(name+'.cg');out=src.with_suffix('.bin')
            src.write_text(source,encoding='utf-8')
            assert not out.exists(), name+': stale output'
            q=subprocess.run([compiler,'-p',profile,'--emit-container',str(out),str(src)],capture_output=True,text=True,timeout=30)
            log=q.stdout+q.stderr
            if refusal:
                assert q.returncode==1, f'{name}: expected exit 1, got {q.returncode}\n{log}'
                assert not out.exists(), name+': refusal left an output file'
                assert refusal in log, f'{name}: expected {refusal!r}\n{log}'
                return
            assert q.returncode==0, f'{name}: expected acceptance, got {q.returncode}\n{log}'
            assert out.exists(), name+': compiler wrote no container'
            data=out.read_bytes()
            assert len(data)>=32,name+': truncated container'
            h=struct.unpack_from('>8I',data)
            assert h[2]==len(data) and h[6]>0 and h[6]%16==0 and h[7]+h[6]<=len(data),name+': invalid container'
            return data
        for row in rows:
            name=row['name']
            if 'twin' in row:
                a=compile_one(name,row['source'],row['profile'])
                b=compile_one(name+'-explicit',row['twin'],row['profile'])
                assert a==b,name+': strict explicit-expansion twin differs'
                twins+=1
            elif 'accept' in row:
                sys.path.insert(0,str(Path(__file__).resolve().parent))
                import fp_eval
                data=compile_one(name,row['source'],row['profile'])
                for inputs,want in row['accept']:
                    got=fp_eval.evaluate(data,{'TEX0':[float(v) for v in inputs]})
                    assert got==[float(v) for v in want],f'{name}: TEX0={inputs} gave {got}, want {want}'
                accepts+=1
            else:
                compile_one(name,row['source'],row['profile'],row['refusal']);refusals+=1
    assert (twins,refusals,accepts)==(32,11,3),(twins,refusals,accepts)
    print(f'PASS ({twins} strict twins, {accepts} hardware-loop rows by value, {refusals} named helper-loop gaps)')

if __name__=='__main__':
    try:main()
    except (AssertionError,subprocess.TimeoutExpired) as e:
        print('FAIL: '+str(e),file=sys.stderr);sys.exit(1)
