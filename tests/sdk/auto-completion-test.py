#!/usr/bin/env python3
"""Actual C completion checks; fake device values do not establish RSX premises."""
import argparse
import json
import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',choices=['address','thread'])
ap.add_argument('--mutant',choices=['early-publication','future-label','unstable-report','label-regression','relaxed-publication'])
a=ap.parse_args()
src=root/'sdk/libps3dkprof/src/auto_rsx'
assert (src/'auto_completion.c').is_file(),'AUTO_COMPLETION_IMPLEMENTATION'
a.out.mkdir(parents=True,exist_ok=False)
code=(src/'auto_completion.c').read_text()
changes={
    'early-publication':('REL(&p->committed,serial);','REL(&p->committed,serial); REL(&p->published,serial);'),
    'future-label':('if(first>snapshot) return AUTO_COMPLETION_DEFER;','(void)snapshot;'),
    'unstable-report':('b0!=b1 || e0!=e1 ||','(void)b1, (void)e1, '),
    'label-regression':('if(first<s->last_label) {s->invalid=1;return AUTO_COMPLETION_REFUSED;}','(void)s->last_label;'),
    'relaxed-publication':('__ATOMIC_RELEASE','__ATOMIC_RELAXED'),
}
if a.mutant:
    old,new=changes[a.mutant];assert code.count(old)==1;code=code.replace(old,new)
(a.out/'auto_completion.c').write_bytes(code.encode())
flags=['-std=c11','-O2','-g','-pthread','-Wall','-Wextra','-Werror']
if a.sanitize:flags+=['-fsanitize='+('address,undefined' if a.sanitize=='address' else 'thread'),'-fno-sanitize-recover=all']
cmd=[os.environ.get('CC','cc'),*flags,'-I'+str(src),str(a.out/'auto_completion.c'),
     str(Path(__file__).with_suffix('.c')),'-o',str(a.out/'test')]
r=subprocess.run(cmd,capture_output=True)
(a.out/'build.stderr').write_bytes(r.stderr)
assert r.returncode==0,r.stderr.decode()
run=[str(a.out/'test')]
if a.sanitize=='thread':run=['setarch','x86_64','-R',*run]
r=subprocess.run(run,capture_output=True,timeout=30,env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1:exitcode=66'))
(a.out/'run.stdout').write_bytes(r.stdout);(a.out/'run.stderr').write_bytes(r.stderr)
(a.out/'commands.json').write_bytes((json.dumps(dict(build=cmd,run=run,rc=r.returncode),indent=2)+'\n').encode())
if a.mutant:
    if a.mutant=='relaxed-publication':
        assert a.sanitize=='thread' and r.returncode==66 and b'ThreadSanitizer: data race' in r.stderr,r.stderr.decode()
    else:
        expected={
            'early-publication':b'auto_publication_before(&publication)==2 && !auto_publication_read(&publication)',
            'future-label':b'!timers_read && !fact.raw_begin && !fact.raw_end',
            'unstable-report':b'==AUTO_COMPLETION_REFUSED',
            'label-regression':b'==AUTO_COMPLETION_REFUSED',
        }[a.mutant]
        assert r.returncode!=0 and b'Assertion' in r.stderr and expected in r.stderr,r.stderr.decode()
    print(a.mutant,'rejected')
else:
    assert r.returncode==0,r.stderr.decode()
    print(r.stdout.decode(),end='')
