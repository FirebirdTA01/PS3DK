#!/usr/bin/env python3
"""Compile verbatim pthread wait bodies at controlled LV2/resolver boundaries.

Function text is extracted before preprocessing; hashes and generated TU are kept.
Only public symbol/type names are renamed to avoid host libc interposition.
This is not a full pthread TU, target ABI or kernel scheduling test.
"""
import argparse,hashlib,json,os,re,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',required=True,type=Path)
p.add_argument('--sanitize',action='store_true')
p.add_argument('--mutant',choices=['errno','reentry','duplicate','site','exit-early'])
a=p.parse_args();root=Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
source=root/'runtime/lv2/librt/pthread.c';text=source.read_text()
if a.mutant=='site':
    old='pt_wait_call(3, cond_id, mutex_id, timeout, site)';assert text.count(old)==1
    text=text.replace(old,'pt_wait_call(3, cond_id, mutex_id, timeout, PT_WAIT_SITE)')
    # Avoid turning a semantic mutant into an unused-parameter compile failure.
    text=text.replace('return pt_wait_call(3, cond_id','(void)site; return pt_wait_call(3, cond_id')
if a.mutant=='exit-early':
    old='pt_wait_notify(__ps3dk_prof_wait_thread_exit);';assert text.count(old)==1
    text=text.replace(old,'(void)0;')
    text=text.replace('__lv2_tls_dtors_run();','#if defined(PS3DK_PROF_WAIT_OBSERVE)\npt_wait_notify(__ps3dk_prof_wait_thread_exit);\n#endif\n\t__lv2_tls_dtors_run();')
names=['pt_err','pthread_mutex_lock','pt_mutex_timed','pthread_mutex_timedlock','pthread_mutex_clocklock','pt_cond_wait_common','pthread_cond_wait','pthread_cond_timedwait','pthread_cond_clockwait','pt_thread_cleanup','pt_trampoline','__lv2_thread_exit','pt_raw_trampoline']
def extract(name):
    start=re.search(r'(?m)^(?:static )?[\w *]+\n'+name+r'\(',text)
    assert start,name
    brace=text.index('{',start.start());depth=0
    for m in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',text[brace:],re.S):
        if m[0]=='{':depth+=1
        elif m[0]=='}':
            depth-=1
            if not depth:return text[start.start():brace+m.end()]+'\n'
    raise AssertionError(name)
bodies={n:extract(n) for n in names}
inc=out/'include/ps3dk';inc.mkdir(parents=True)
shutil.copyfile(root/'sdk/include/ps3dk/prof_wait_internal.h',inc/'prof_wait_internal.h')
header=root/'runtime/lv2/librt/prof_wait_adapter.h'
joined=bodies['pt_err']
if header.exists():
    h=header.read_text()
    if a.mutant in ['errno','reentry','duplicate']:
        old,new={
            'errno':('errno = saved_errno;','(void)saved_errno;'),
            'reentry':('pt_wait_observer_busy || ps3dk_prof__wait_begin','ps3dk_prof__wait_begin'),
            'duplicate':('raw = operation == 3 ? sysCondWait(target, timeout) : sysMutexLock(target, timeout);',
                         '(void)(operation == 3 ? sysCondWait(target, timeout) : sysMutexLock(target, timeout));\n    raw = operation == 3 ? sysCondWait(target, timeout) : sysMutexLock(target, timeout);')
        }[a.mutant]
        assert old in h;h=h.replace(old,new)
    (out/header.name).write_text(h)
    joined+='\n#if defined(PS3DK_PROF_WAIT_OBSERVE)\n#include "prof_wait_adapter.h"\n#endif\n'
joined+='\n'.join(bodies[n] for n in names[1:])
for old,new in [('pthread_mutex_t','wait_mutex'),('pthread_cond_t','wait_cond'),('free','probe_free')]+[(n,'tested_'+n) for n in names if n.startswith('pthread_')]:
    joined=re.sub(r'\b'+old+r'\b',new,joined)
template=Path(__file__).with_name('wait_pthread_test.c').read_text()
(out/'probe.c').write_text(template.replace('/* @PRODUCTION_WAIT_BODIES@ */',joined))
(out/'source-pins.json').write_text(json.dumps({'source':hashlib.sha256(source.read_bytes()).hexdigest(),'bodies':{k:hashlib.sha256(v.encode()).hexdigest() for k,v in bodies.items()}},indent=2))
commands=[];env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1',UBSAN_OPTIONS='halt_on_error=1')
def run(name,argv):
    r=subprocess.run(list(map(str,argv)),capture_output=True,timeout=60,env=env)
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=list(map(str,argv)),rc=r.returncode));(out/'commands.json').write_text(json.dumps(commands,indent=2))
    return r
flags=['-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-g','-Wall','-Wextra','-Werror','-fno-inline','-fno-optimize-sibling-calls','-I',inc.parent]
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
failed=0
for name,extra in [('off',[]),('on',['-DPS3DK_PROF_WAIT_OBSERVE=1']),('missing-begin',['-DPS3DK_PROF_WAIT_OBSERVE=1','-DOMIT_BEGIN']),('missing-end',['-DPS3DK_PROF_WAIT_OBSERVE=1','-DOMIT_END']),('missing-lifecycle',['-DPS3DK_PROF_WAIT_OBSERVE=1','-DOMIT_LIFECYCLE'])]:
    r=run(name+'-compile',[os.environ.get('CC','cc'),*flags,*extra,out/'probe.c','-o',out/name])
    assert r.returncode==0,r.stderr.decode()
    r=run(name,[out/name]);failed+=r.returncode!=0
    print(name,'rc',r.returncode,r.stdout.decode().strip(),flush=True)
raise SystemExit(bool(failed))
