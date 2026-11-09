#!/usr/bin/env python3
"""Execute the sample with the real host profiler and a deterministic start seam.

Only target timebase and sink path are substituted. No LV2, observer-cost,
target TLS or target ordering claim follows from this host test.
"""
import argparse, hashlib, json, os, re, subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--mutant',choices=['registration','handshake','holder','cleanup'])
p.add_argument('--exercise-failure',action='store_true',help='force exercise failure and require holder-release publication')
a=p.parse_args()
root=Path(__file__).resolve().parents[4]
sample=Path(__file__).resolve().parents[1]
out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
source=(sample/'source/main.c').read_text()
original=source
if a.mutant=='cleanup':
    old='    __atomic_store_n(&timed_done, 1, __ATOMIC_RELEASE); /* Also release on exercise failure. */'
    assert source.count(old)==1
    source=source.replace(old,'')
if a.mutant=='registration':
    old='ps3dk_prof_thread_begin("init") != PS3DK_PROF_EALREADY'
    assert source.count(old)==1
    source=source.replace(old,'ps3dk_prof_thread_begin("init") != 0')
if a.mutant=='handshake':
    old='    if (await_worker_entered()) return 2;'
    assert source.count(old)==1
    source=source.replace(old,'    if (0 && await_worker_entered()) return 2;')
if a.mutant=='holder':
    old='    __atomic_store_n(&timed_held, 1, __ATOMIC_RELEASE);\n    while (!__atomic_load_n(&timed_done, __ATOMIC_ACQUIRE)) usleep(1000);\n    worker_rc = pthread_mutex_unlock(&timed_mutex);'
    assert source.count(old)==1
    source=source.replace(old,'    worker_rc = pthread_mutex_unlock(&timed_mutex);\n    __atomic_store_n(&timed_held, 1, __ATOMIC_RELEASE);')
source,n=re.subn(r'static uint64_t tick\(void\) \{.*?\n\}', 'static uint64_t tick(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }',source,count=1,flags=re.S)
assert n==1
assert source.count('ps3dk_prof_set_sink_path("/dev_hdd0/tmp")')==1
source=source.replace('ps3dk_prof_set_sink_path("/dev_hdd0/tmp")','ps3dk_prof_set_sink_path(getenv("WAIT_HOST_SINK"))')
if a.exercise_failure:
    old='static int exercise(void) {'
    assert source.count(old)==1
    source=source.replace(old,old+'\n    return 1; /* Host-only early failure injection. */')
    assert source.count('int main(void) {')==1
    source=source.replace('int main(void) {','static int sample_main(void) {')
    source+='''
int main(void) {
    int rc = sample_main();
    if (rc != 2 || __atomic_load_n(&timed_done, __ATOMIC_ACQUIRE) != 1) return 3;
    puts("WAIT_FAILURE_RELEASE_OK");
    return 0;
}
'''
source='#define _DEFAULT_SOURCE\n#include <stdlib.h>\n'+source
(out/'sample.c').write_bytes(source.encode())
src=root/'sdk/libps3dkprof/src'
inc=out/'include/ps3dk';inc.mkdir(parents=True)
for name in ['prof.h','prof_wait.h','prof_wait_internal.h']:
    (inc/name).write_bytes((root/'sdk/include/ps3dk'/name).read_bytes())
files=[src/f for f in ['core/prof_core.c','core/prof_drain.c','core/prof_version.c','pal/pal_host.c']]
flags=['-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-prio-ctor-dtor','-pthread','-DPS3DK_PROF=1','-DPS3DK_PROF_PAL_HOST','-DWAIT_BUILD_ID="'+'a'*64+'"','-I',inc.parent,'-I',src/'core','-I',src/'waits']
commands=[]
def run(name,argv,env=None):
    argv=list(map(str,argv));r=subprocess.run(argv,capture_output=True,env=env,timeout=30)
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=argv,rc=r.returncode))
    (out/'commands.json').write_bytes((json.dumps(commands,indent=2)+'\n').encode())
    return r
inputs=[sample/'source/main.c',Path(__file__),Path(__file__).with_name('lifecycle.c'),*files,*sorted((src/'waits').glob('*.c'))]
pins={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
(out/'input-pins.json').write_bytes((json.dumps(pins,indent=2)+'\n').encode())
for observed in [0,1]:
    name='on' if observed else 'off'
    defs=['-DWAIT_OBSERVED='+str(observed)]
    more=[]
    if observed:
        defs+=['-DPS3DK_PROF_WITH_WAITS','-DPS3DK_PROF_WAIT_STACK_BYTES=1024']
        more=sorted((src/'waits').glob('*.c'))
    r=run(name+'-build',[os.environ.get('CC','cc'),*flags,*defs,*files,*more,out/'sample.c',Path(__file__).with_name('lifecycle.c'),'-Wl,--wrap=pthread_create','-Wl,--wrap=ps3dk_prof_init','-Wl,--wrap=usleep','-o',out/name])
    assert r.returncode==0,r.stderr.decode()
    sink=out/(name+'-sink');sink.mkdir()
    r=run(name,[out/name],dict(os.environ,WAIT_HOST_SINK=str(sink)))
    assert r.returncode==0,(name,r.returncode,r.stdout.decode(),r.stderr.decode())
    if a.exercise_failure:
        assert r.stdout.count(b'WAIT_FAILURE_RELEASE_OK')==1
        assert b'WAIT_SAMPLE_OK' not in r.stdout and not list(sink.glob('*.wait.txt'))
        continue
    assert r.stdout.count(b'WAIT_SAMPLE_OK')==1
    receipts=list(sink.glob('*.wait.txt'));assert len(receipts)==1
    receipt=receipts[0].read_text()
    assert 'COUNTS mutex=2050 timed_mutex=64 condition_timeout=16 worker_mutex=32 rows=82\n' in receipt
    assert 'SHUTDOWN rc=0\n' in receipt and receipt.endswith('WAIT_SAMPLE_END complete=1\n')
assert pins=={f:hashlib.sha256(Path(f).read_bytes()).hexdigest() for f in pins}
print(json.dumps(dict(cases=2,failures=0,scope='host sample lifecycle; real core, scheduled start seam; not LV2 adapters')))
