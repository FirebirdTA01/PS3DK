#!/usr/bin/env python3
"""Compile the automatic CONFIG ownership and freeze controls."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',choices=['address','thread'])
ap.add_argument('--mutant',choices=['early-read','replace-frozen','lose-stop','clear-conflict'])
a=ap.parse_args()
src=ROOT/'sdk/libps3dkprof/src/auto_rsx'
assert (src/'auto_config.c').is_file(), 'AUTO_CONFIG_IMPLEMENTATION'
a.out.mkdir(parents=True,exist_ok=False)
flags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-pthread']
if a.sanitize:
    flags+=['-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread'),'-fno-sanitize-recover=all']
code=(src/'auto_config.c').read_text()
mutants={
    'early-read': ('(ACQ(&s->state)&PHASE_MASK)==FROZEN', '(ACQ(&s->state)&PHASE_MASK)!=WRITING', '!auto_config_read(&store)'),
    'replace-frozen': ('phase!=EMPTY && phase!=READY', 'phase!=EMPTY && phase!=READY && phase!=FROZEN', '!auto_config_begin(&store)'),
    'lose-stop': ('if(CAS(&s->state,&expected,READY)) return 1;', '(void)expected; REL(&s->state,READY); return 1;', '!auto_config_commit(&store,ticket,&c)'),
    'clear-conflict': ('__atomic_fetch_or(&s->flags,flags,__ATOMIC_RELEASE);', '__atomic_store_n(&s->flags,flags,__ATOMIC_RELEASE);', 'auto_config_flags(&store)==(AUTO_STATUS_CONFLICT|AUTO_STATUS_RESET)'),
}
if a.mutant:
    old,new,expected=mutants[a.mutant]; assert code.count(old)==1
    code=code.replace(old,new)
(a.out/'auto_config.c').write_bytes(code.encode())
cmd=[os.environ.get('CC','cc'),*flags,'-I'+str(src),str(Path(__file__).with_suffix('.c')),str(a.out/'auto_config.c'),'-o',str(a.out/'test')]
r=subprocess.run(cmd,capture_output=True)
(a.out/'build.stderr').write_bytes(r.stderr)
assert r.returncode==0,r.stderr.decode()
command=[str(a.out/'test')]
if a.sanitize=='thread': command=['setarch','x86_64','-R',*command]
r=subprocess.run(command,capture_output=True,timeout=60,env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1:exitcode=66'))
(a.out/'stdout.txt').write_bytes(r.stdout); (a.out/'stderr.txt').write_bytes(r.stderr)
(a.out/'commands.json').write_bytes((json.dumps(dict(build=cmd,run=command,rc=r.returncode),indent=2)+'\n').encode())
if a.mutant:
    assert r.returncode==-6 and expected in r.stderr.decode(),(a.mutant,r.returncode,r.stderr.decode())
    print(a.mutant,'KILLED',expected)
else:
    assert r.returncode==0,r.stderr.decode()
print(r.stdout.decode(),end='')
