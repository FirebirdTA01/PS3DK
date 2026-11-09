#!/usr/bin/env python3
"""Controls for the link selection guard; actual target links are a separate gate."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args()
out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
root=Path(__file__).resolve().parents[3];tool=root/'cmake/ps3-prof-waits-check.py'
lib=out/'prefix with spaces';lib.mkdir()
rt=lib/'librt_profwait.a';prof=lib/'libps3dkprof_wait.a'
rt.write_bytes(b'fixture observed runtime');prof.write_bytes(b'fixture observed profiler')
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
(lib/'prof-waits').mkdir()
runtime=dict(schema=1,kind='observed-pthread-runtime',abis={'ilp32':dict(sha256=sha(rt)),'lp64':dict(sha256='0'*64)})
runtime_path=lib/'prof-waits/runtime-manifest.json';runtime_path.write_text(json.dumps(runtime))
manifest=dict(schema=1,kind='profiler-wait-variant',abi='ilp32',mode='manual',sha256=sha(prof))
headers=out/'include/ps3dk';headers.mkdir(parents=True)
for name in ['prof_wait.h','prof_wait_internal.h']:(headers/name).write_text('fixture '+name)
manifest['public_header_sha256']={name:sha(headers/name) for name in ['prof_wait.h','prof_wait_internal.h']}
mp=prof.with_suffix('.a.json');mp.write_text(json.dumps(manifest))
providers={name:str(rt)+'(pthread.o)' for name in ['pthread_mutex_lock','pthread_cond_wait','pthread_create','pthread_exit','__lv2_thread_exit','__lv2_ppu_thread_create']}
providers.update({name:str(prof)+'('+member+')' for name,member in [
 ('ps3dk_prof__wait_begin','wait_runtime.o'),('ps3dk_prof__wait_end','wait_runtime.o'),
 ('__ps3dk_prof_wait_thread_start','wait_runtime.o'),('__ps3dk_prof_wait_thread_exit','wait_runtime.o'),
 ('__ps3dk_prof_wait_config_anchor','wait_startup.o'),('ps3dk_prof_init','prof_core.o')]})
def maptext(rows):return 'Archive member included to satisfy reference by file (symbol)\n\n'+str(rt)+'(pthread.o)\n\nCross Reference Table\n\nSymbol File\n'+''.join(n+' '+f+'\n' for n,f in sorted(rows.items()))
good=maptext(providers);mapfile=out/'consumer.map';rows=[]
def run(name,expected,text=good,abi='ilp32'):
 mapfile.write_text(text)
 argv=[sys.executable,str(tool),'--lib-dir',str(lib),'--include-dir',str(headers.parent),'--abi',abi,'--mode','manual','--map',str(mapfile)]
 r=subprocess.run(argv,capture_output=True,timeout=10)
 (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
 ok=r.returncode==expected and b"can't open file" not in r.stderr
 rows.append(dict(name=name,argv=argv,rc=r.returncode,expected=expected,passed=ok))
run('valid',0)
run('default-first',1,good.replace(str(rt)+'(pthread.o)',str(lib/'librt.a')+'(pthread.o)'))
run('default-unused-member',1,good+'\n .data 0x0 4 '+str(lib/'librt.a')+'(read.o)\n')
run('mixed-core',1,good+'\n '+str(lib/'libps3dkprof_ext.a')+'(prof_core.o)\n')
run('no-cross-reference',1,good.split('Cross Reference Table')[0])
run('missing-anchor',1,maptext({n:f for n,f in providers.items() if n!='__ps3dk_prof_wait_config_anchor'}))
changed=dict(providers);changed['ps3dk_prof__wait_begin']='application.o'
run('foreign-hook',1,maptext(changed))
run('wrong-abi',1,abi='lp64')
prof.write_bytes(b'substituted archive');run('changed-profiler',1);prof.write_bytes(b'fixture observed profiler')
rt.write_bytes(b'substituted runtime');run('changed-runtime',1);rt.write_bytes(b'fixture observed runtime')
prof.rename(lib/'saved.a');run('missing-profiler',1);(lib/'saved.a').rename(prof)
run('windows-separators',0,good.replace('/','\\'))
weak=dict(providers);weak['__ps3dk_prof_thread_exit']=str(rt)+'(pthread.o)'
run('legacy-weak-exit-reference',0,maptext(weak))
weak['__ps3dk_prof_thread_exit']='application.o'
run('foreign-legacy-exit-definition',1,maptext(weak))
header=headers/'prof_wait.h';header.write_text('changed');run('changed-public-header',1)
header.unlink();run('missing-public-header',1)
header.write_text('fixture prof_wait.h');run('restored-public-header',0)
(out/'commands.json').write_text(json.dumps(rows,indent=2)+'\n')
bad=[r['name'] for r in rows if not r['passed']];print(len(rows)-len(bad),'passed,',len(bad),'failed',bad)
sys.exit(bool(bad))
