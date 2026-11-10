"""Serial host-only model gate; each output directory is fresh and retained."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--sanitize',action='store_true');ap.add_argument('--mutants',action='store_true')
a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
jobs=[(name,[sys.executable,str(HERE/f'auto-rsx-{name}-test.py')]) for name in ('read','fixture')]
jobs.append(('target-lifetime',[sys.executable,str(HERE/'auto-target-model-test.py'),'--out',str(out/'target-lifetime')]+
             (['--mutants'] if a.mutants else [])))
for name in ('rsx','wait'):
    jobs.append((name,[sys.executable,str(HERE/f'auto-{name}-model-test.py'),'--out',str(out/name)]+(['--sanitize'] if a.sanitize else [])))
if a.mutants:
    rsx=['minus-marker-room','unsafe-injection','alias-double','drop-delegated-operation','recursive-reserve',
         'reuse-on-label','ack-on-short-write','unwind-uncertain-emission','stale-generation','discard-open-on-stop']
    wait=['gpu-command-as-cpu-wait','sum-parent-and-child','close-lost-begin','zero-open-at-stop','hide-internal-cost']
    for kind,names in [('rsx',rsx),('wait',wait)]:
        for name in names:
            cmd=[sys.executable,str(HERE/f'auto-{kind}-model-test.py'),'--out',str(out/name),'--mutant',name]
            if kind=='rsx':cmd+=['--case','projection' if name=='alias-double' else 'capacity']
            jobs.append((name,cmd))
receipt=[]
for name,cmd in jobs:
    result=subprocess.run(cmd,capture_output=True)
    (out/(name+'.stdout')).write_bytes(result.stdout);(out/(name+'.stderr')).write_bytes(result.stderr)
    receipt.append(dict(name=name,argv=cmd,rc=result.returncode));(out/'commands.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(name,result.returncode,flush=True)
    if result.returncode:print(result.stdout.decode(),result.stderr.decode());raise SystemExit(1)
sources=sorted(HERE.glob('auto*'))
(out/'sources.json').write_text(json.dumps({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sources if p.is_file()},indent=2)+'\n')
print('host model suite PASS',len(jobs),'commands; no target execution')
