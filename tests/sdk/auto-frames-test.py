#!/usr/bin/env python3
"""Bounded frame queue and exact per-frame visit/outcome accounting."""
import argparse
import json
import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',choices=['address','thread'])
ap.add_argument('--mutant',choices=['accounting','partial-ack','implicit-close','lost','publication'])
a=ap.parse_args();src=root/'sdk/libps3dkprof/src/auto_rsx'
assert (src/'auto_frames.c').is_file(),'AUTO_FRAME_IMPLEMENTATION'
a.out.mkdir(parents=True,exist_ok=False)
source=src/'auto_frames.c'
mutations={
    'accounting':('if(remaining) return 0;','(void)remaining;','!auto_frames_close(&frames,42,0,20,0)'),
    'partial-ack':('if(!whole || !f ||','(void)whole; if(!f ||','!auto_frames_ack(&frames,42,0,0)'),
    'implicit-close':('if(open || tb<q->live.tb_begin)','(void)open; if(tb<q->live.tb_begin)','f->frame==1 && f->flags==19'),
    'lost':('else REL(&q->lost,lost+1u);','else REL(&q->lost,lost);','frames.lost==1 && !frames.active'),
    'publication':('REL(&q->write_idx,w+1u);','__atomic_store_n(&q->write_idx,w+1u,__ATOMIC_RELAXED);','WARNING: ThreadSanitizer: data race'),
}
if a.mutant:
    assert (a.sanitize=='thread')==(a.mutant=='publication')
    old,new,expected=mutations[a.mutant];code=source.read_text();assert code.count(old)==1
    source=a.out/'auto_frames.c';source.write_bytes(code.replace(old,new).encode())
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
    if a.mutant=='publication':assert r.returncode==66
    else:assert b'Assertion' in r.stderr
    print(a.mutant+' rejected by '+expected)
else:
    assert r.returncode==0,r.stderr.decode()
    print(r.stdout.decode(),end='')
