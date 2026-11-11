#!/usr/bin/env python3
"""Build the AUTO SPSC runtime queue controls, optionally with a sanitizer."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',choices=['address','thread'])
ap.add_argument('--mutant',choices=['short-ack','foreign-ack','free-incomplete','relaxed-publication'])
a=ap.parse_args()
src=ROOT/'sdk/libps3dkprof/src/auto_rsx'
assert (src/'auto_queue.c').is_file(), 'AUTO_QUEUE_IMPLEMENTATION'
a.out.mkdir(parents=True,exist_ok=False)
flags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-pthread']
if a.sanitize:
    flags+=['-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread'),'-fno-sanitize-recover=all']
code=(src/'auto_queue.c').read_text()
mutants={
    'short-ack': ('if(!whole || !e || e->op.id!=id)', 'if(whole<0 || !e || e->op.id!=id)', '!auto_queue_ack(&q,1,1,0)'),
    'foreign-ack': ('e->op.id!=id', 'id==UINT64_MAX', '!auto_queue_ack(&q,99,1,1)'),
    'free-incomplete': ('completed && !ACQ(&q->stopped)', 'completed>=0 && !ACQ(&q->stopped)', 'q.written==3 && auto_queue_held(&q)==1'),
    'relaxed-publication': ('REL(&q->write_idx,RLX(&q->write_idx)+1u);',
                            '__atomic_store_n(&q->write_idx,RLX(&q->write_idx)+1u,__ATOMIC_RELAXED);', 'ThreadSanitizer: data race'),
}
if a.mutant:
    old,new,expected=mutants[a.mutant]; assert code.count(old)==1
    assert a.mutant!='relaxed-publication' or a.sanitize=='thread'
    code=code.replace(old,new)
(a.out/'auto_queue.c').write_bytes(code.encode())
cmd=[os.environ.get('CC','cc'),*flags,'-I'+str(src),str(Path(__file__).with_suffix('.c')),str(a.out/'auto_queue.c'),'-o',str(a.out/'test')]
r=subprocess.run(cmd,capture_output=True)
(a.out/'build.stderr').write_bytes(r.stderr)
assert r.returncode==0,r.stderr.decode()
command=[str(a.out/'test')]
if a.sanitize=='thread': command=['setarch','x86_64','-R',*command]
r=subprocess.run(command,capture_output=True,timeout=60,env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1:exitcode=66'))
(a.out/'stdout.txt').write_bytes(r.stdout); (a.out/'stderr.txt').write_bytes(r.stderr)
(a.out/'commands.json').write_bytes((json.dumps(dict(build=cmd,run=command,rc=r.returncode),indent=2)+'\n').encode())
if a.mutant:
    want=66 if a.mutant=='relaxed-publication' else -6
    assert r.returncode==want and expected in r.stderr.decode(),(a.mutant,r.returncode,r.stderr.decode())
    print(a.mutant,'KILLED',expected)
else:
    assert r.returncode==0,r.stderr.decode()
print(r.stdout.decode(),end='')
