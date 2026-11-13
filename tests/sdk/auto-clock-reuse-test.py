#!/usr/bin/env python3
"""Clock-anchor slot reuse lifetime controls (host fake device; no RSX evidence)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',choices=['address','thread'])
ap.add_argument('--mutant',choices=['release-failed','partial-ack','owner-release','slot-serial',
                                    'serial-wrap','skip-cursor','handoff'])
a=ap.parse_args();src=root/'sdk/libps3dkprof/src/auto_rsx'
assert (src/'auto_clock.c').is_file(),'AUTO_CLOCK_IMPLEMENTATION'
a.out.mkdir(parents=True,exist_ok=False)
source=src/'auto_clock.c'
# Each mutant removes one load-bearing rule; the named assertion must catch it.
mutations={
    'release-failed':('release=r->flags==1u || !emitted;','(void)emitted;release=1u;',
                      'slot_state(FIRST+1)==AUTO_CLOCK_QUARANTINED'),
    'partial-ack':('if(!whole || !r || r->id!=id) return 0;','(void)whole;if(!r || r->id!=id) return 0;',
                   '!auto_clock_ack(&q,1,0)'),
    'owner-release':('q->release[index]=release;','q->release[index]=release;if(release)REL(&q->slots[s],AUTO_CLOCK_FREE);',
                     'slot_state(r->slot)==AUTO_CLOCK_HELD'),
    'slot-serial':('r->id=r->serial=serial;','r->id=r->serial=s+1u;','auto_clock_after(&q,serial)'),
    'serial-wrap':('q->serial>=UINT32_MAX-1u','0','auto_clock_sample(&q,&port,7,8)==2'),
    'skip-cursor':('q->cursor=(s+1u)%AUTO_CLOCK_SLOTS;','q->cursor=0;',
                   'r->slot==FIRST+(n-1)%AUTO_CLOCK_SLOTS'),
    'handoff':('REL(&q->write_idx,w+1u);','__atomic_store_n(&q->write_idx,w+1u,__ATOMIC_RELAXED);',
               'WARNING: ThreadSanitizer: data race'),
}
if a.mutant:
    assert (a.sanitize=='thread')==(a.mutant=='handoff')
    old,new,expected=mutations[a.mutant];code=source.read_text();assert code.count(old)==1,old
    source=a.out/'auto_clock.c';source.write_bytes(code.replace(old,new).encode())
flags=['-std=c11','-O2','-g','-pthread','-Wall','-Wextra','-Werror']
if a.sanitize:flags+=['-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread'),'-fno-sanitize-recover=all']
cmd=[os.environ.get('CC','cc'),*flags,'-I'+str(src),str(source),str(Path(__file__).with_suffix('.c')),'-o',str(a.out/'test')]
r=subprocess.run(cmd,capture_output=True);(a.out/'build.stderr').write_bytes(r.stderr)
assert r.returncode==0,r.stderr.decode()
run=[str(a.out/'test')]
if a.sanitize=='thread':run=['setarch','x86_64','-R',*run]
r=subprocess.run(run,capture_output=True,timeout=300,env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1:exitcode=66'))
(a.out/'run.stdout').write_bytes(r.stdout);(a.out/'run.stderr').write_bytes(r.stderr)
(a.out/'commands.json').write_bytes((json.dumps(dict(build=cmd,run=run,rc=r.returncode),indent=2)+'\n').encode())
if a.mutant:
    assert r.returncode!=0 and expected.encode() in r.stderr,r.stderr.decode()
    if a.mutant=='handoff':assert r.returncode==66
    else:assert b'Assertion' in r.stderr
    print(a.mutant+' rejected by '+expected)
else:
    assert r.returncode==0,r.stderr.decode()
    print(r.stdout.decode(),end='')
