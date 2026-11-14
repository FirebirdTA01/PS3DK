#!/usr/bin/env python3
"""Bracketed clock anchors (owner issues, drainer completes) with independently supplied device facts."""
import argparse
import json
import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',choices=['address','thread'])
ap.add_argument('--mutant',choices=['deadline','put','unstable','label-regression','partial-ack','handoff'])
a=ap.parse_args();src=root/'sdk/libps3dkprof/src/auto_rsx'
assert (src/'auto_clock.c').is_file(),'AUTO_CLOCK_IMPLEMENTATION'
a.out.mkdir(parents=True,exist_ok=False)
source=src/'auto_clock.c'
mutations={
    'deadline':('if(seal || now-q->issue[i]>=q->frequency*2u)','if(seal)','++n < 1000'),
    'put':('if(ACQ(&q->published)!=serial) goto failed;','if(0) goto failed;','r && r->flags == 8'),
    'unstable':('raw!=UINT64_MAX && raw==repeated','raw!=UINT64_MAX','done()->flags == 8 && !done()->raw'),
    'label-regression':('label<q->last_label || ','','!auto_clock_poll(&clock_state, &port, 0) && clock_state.invalid'),
    'partial-ack':('if(!whole || !r ||','(void)whole; if(!r ||','!auto_clock_ack(&clock_state, 1, 0)'),
    'handoff':('if(r==ACQ(&q->write_idx) || ACQ(&q->state[r%AUTO_CLOCK_RECORDS])!=AUTO_CLOCK_DONE) return NULL;','if(r==ACQ(&q->write_idx)) return NULL;','!done() && fake.emits == 1 && fake.flushes == 1'),
}
if a.mutant:
    assert (a.sanitize=='thread')==(a.mutant=='handoff')
    old,new,expected=mutations[a.mutant];code=source.read_text();assert code.count(old)==1
    source=a.out/'auto_clock.c';source.write_bytes(code.replace(old,new).encode())
flags=['-std=c11','-O2','-g','-pthread','-Wall','-Wextra','-Werror']
if a.sanitize:flags+=['-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread'),'-fno-sanitize-recover=all']
cmd=[os.environ.get('CC','cc'),*flags,'-I'+str(src),str(source),str(Path(__file__).with_suffix('.c')),'-o',str(a.out/'test')]
r=subprocess.run(cmd,capture_output=True);(a.out/'build.stderr').write_bytes(r.stderr)
assert r.returncode==0,r.stderr.decode()
run=[str(a.out/'test')]
if a.sanitize=='thread':run=['setarch','x86_64','-R',*run]
r=subprocess.run(run,capture_output=True,timeout=30,env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1:exitcode=66'))
(a.out/'run.stdout').write_bytes(r.stdout);(a.out/'run.stderr').write_bytes(r.stderr)
(a.out/'commands.json').write_bytes((json.dumps(dict(build=cmd,run=run,rc=r.returncode),indent=2)+'\n').encode())
if a.mutant:
    assert r.returncode!=0 and expected.encode() in r.stderr,r.stderr.decode()
    assert b'Assertion' in r.stderr
    print(a.mutant+' rejected by '+expected)
else:
    assert r.returncode==0,r.stderr.decode()
    print(r.stdout.decode(),end='')
