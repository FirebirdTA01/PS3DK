#!/usr/bin/env python3
"""Build AUTO chunk ordering and sink-acceptance controls."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',action='store_true')
ap.add_argument('--mutant',choices=['retry-sink','refused-data','early-account','omit-conflict','publication-regression'])
a=ap.parse_args()
src=ROOT/'sdk/libps3dkprof/src/auto_rsx'
assert (src/'auto_writer.c').is_file(), 'AUTO_WRITER_IMPLEMENTATION'
a.out.mkdir(parents=True,exist_ok=False)
flags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror']
if a.sanitize: flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
code=(src/'auto_writer.c').read_text()
mutants={
    'retry-sink': ('w->failed=1;', 'w->failed=0;', 'writer.failed && !writer.config_sent'),
    'refused-data': ('if(c->reason) {', 'if(c->reason>10) {', '!auto_writer_begin(&writer)'),
    'early-account': ('w->operations_written+=count;', 'w->operations_written+=count;', 'writer.failed && writer.operations_written==0'),
    'omit-conflict': ('flags|=(uint16_t)auto_config_flags(w->config);', '(void)auto_config_flags(w->config);', 'output.data[4][2]==AUTO_STATUS_CONFLICT'),
    'publication-regression': ('values[13]<w->publication_max', 'values[13]>UINT64_MAX', '!auto_writer_status(&writer,0,counters)'),
}
if a.mutant:
    old,new,expected=mutants[a.mutant]
    if a.mutant=='early-account':
        old='if(!send(w,C_AUTO_OPS,n)) return 0;\n    w->operations_written+=count;'
        new='w->operations_written+=count;\n    if(!send(w,C_AUTO_OPS,n)) return 0;'
    assert code.count(old)==1
    code=code.replace(old,new)
(a.out/'auto_writer.c').write_bytes(code.encode())
cmd=[os.environ.get('CC','cc'),*flags,'-I'+str(src),str(Path(__file__).with_suffix('.c')),
     str(a.out/'auto_writer.c'),str(src/'auto_wire.c'),str(src/'auto_config.c'),'-o',str(a.out/'test')]
r=subprocess.run(cmd,capture_output=True)
(a.out/'build.stderr').write_bytes(r.stderr)
assert r.returncode==0,r.stderr.decode()
r=subprocess.run([str(a.out/'test')],capture_output=True,timeout=30)
(a.out/'stdout.txt').write_bytes(r.stdout); (a.out/'stderr.txt').write_bytes(r.stderr)
(a.out/'commands.json').write_bytes((json.dumps(dict(build=cmd,rc=r.returncode),indent=2)+'\n').encode())
if a.mutant:
    assert r.returncode==-6 and expected in r.stderr.decode(),(a.mutant,r.returncode,r.stderr.decode())
    print(a.mutant,'KILLED',expected)
else: assert r.returncode==0,r.stderr.decode()
print(r.stdout.decode(),end='')
