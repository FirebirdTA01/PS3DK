#!/usr/bin/env python3
"""Measure target storage and retain production atomic/stack emission.

The compile-only writer uses stack_bytes=1 solely to allow inspection before a
bound is established. These objects must never be archived, linked or shipped.
"""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True);p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--sdk-include',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
src=root/'sdk/libps3dkprof/src';cc=a.compiler.resolve();prefix=str(cc).removesuffix('gcc');commands=[]
def run(name,argv):
    r=subprocess.run(list(map(str,argv)),capture_output=True,timeout=120)
    (out/(name+'.stdout')).write_bytes(r.stdout);(out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(name=name,argv=list(map(str,argv)),rc=r.returncode));(out/'commands.json').write_text(json.dumps(commands,indent=2))
    assert r.returncode==0,(name,r.stderr.decode());return r.stdout.decode()
run('compiler',[cc,'-v'])
layout='''#include "prof_internal.h"
#include <stddef.h>
#ifdef PS3DK_PROF_WITH_WAITS
#include "wait_internal.h"
_Static_assert(__atomic_always_lock_free(8,0),"64-bit wait atomics must be lock free");
_Static_assert(W_CAPTURE % _Alignof(wait_actor)==0,"actor base alignment");
_Static_assert(W_CONTROL % _Alignof(uint64_t)==0,"record base alignment");
_Static_assert(W_REC % _Alignof(uint64_t)==0,"record stride alignment");
#define FIELD(n,v) const unsigned char report_##n[(v)+1]={0}
FIELD(actor,sizeof(wait_actor)); FIELD(actor_align,_Alignof(wait_actor));
FIELD(capture,sizeof(wait_capture)); FIELD(capture_align,_Alignof(wait_capture));
FIELD(options,sizeof(p_wait_options)); FIELD(head_offset,offsetof(wait_actor,head));
FIELD(tail_offset,offsetof(wait_actor,tail)); FIELD(seq_offset,offsetof(wait_actor,seq));
FIELD(default_block,W_CAPTURE+64*(W_CONTROL+128*W_REC));
#else
#define FIELD(n,v) const unsigned char report_##n[(v)+1]={0}
#endif
FIELD(tls,sizeof(tls_state)); FIELD(tls_align,_Alignof(tls_state));
FIELD(prof,sizeof(prof)); FIELD(prof_align,_Alignof(prof));
'''
(out/'layout.c').write_text(layout)
report={}
for abi,extra in [('ilp32',[]),('lp64',['-mlp64'])]:
    flags=[*extra,'-O2','-g0','-Wall','-Wextra','-Werror','-std=c11','-mcpu=cell','-fno-instrument-functions','-fstack-usage','-DPS3DK_PROF=1','-DPS3DK_PROF_PAL_LV2','-I',root/'sdk/include','-I',a.sdk_include,'-I',src/'core','-I',src/'pal','-I',src/'waits']
    models={}
    for mode,defines in [('off',[]),('on',['-DPS3DK_PROF_WITH_WAITS=1'])]:
        name=abi+'-'+mode
        run(name,[cc,*flags,*defines,'-c',out/'layout.c','-o',out/(name+'.o')])
        symbols=run(name+'-symbols',[prefix+'readelf','-sW',out/(name+'.o')])
        models[mode]={m[2]:int(m[1],16 if m[1].startswith('0x') else 10)-1 for m in re.finditer(r'^\s*\d+:\s+[0-9a-f]+\s+(0x[0-9a-f]+|\d+)\s+OBJECT\s+GLOBAL\s+DEFAULT\s+\S+\s+report_(\w+)\s*$',symbols,re.M)}
    assert models['on']['default_block']==806912
    report[abi]=dict(layout=models,core_tls_delta=models['on']['tls']-models['off']['tls'],core_prof_delta=models['on']['prof']-models['off']['prof'])
    for rel in ['waits/wait_runtime.c','waits/wait_config.c','waits/wait_writer.c','core/prof_core.c','core/prof_drain.c','pal/pal_lv2.c']:
        name=abi+'-'+Path(rel).stem
        run(name,[cc,*flags,'-DPS3DK_PROF_WITH_WAITS=1','-DPS3DK_PROF_WAIT_STACK_BYTES=1','-c',src/rel,'-o',out/(name+'.o')])
        undefined=run(name+'-undefined',[prefix+'nm','-u',out/(name+'.o')])
        assert not re.search(r'\b__(atomic|sync)_',undefined),(name,'out of line atomic helper')
        run(name+'-disassembly',[prefix+'objdump','-dr',out/(name+'.o')])
    print(abi,json.dumps(report[abi]),flush=True)
(out/'RESULT.json').write_text(json.dumps(dict(scope=__doc__,compiler_sha256=hashlib.sha256(cc.read_bytes()).hexdigest(),abis=report),indent=2))
