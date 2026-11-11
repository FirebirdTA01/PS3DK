#!/usr/bin/env python3
"""Compare canonical C command bodies with frozen original words and groups.

Host command memory and fake observer only; not a target compile or GPU claim.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
HERE=ROOT/'tests/host/ps3dk-prof-runtime'
spec=importlib.util.spec_from_file_location('fixture',HERE/'auto-rsx-emitter-fixture.py')
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',action='store_true')
ap.add_argument('--mutant',choices=['missing-end','unsafe-hook','changed-argument','skip-delegate'])
a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=False)
baseline=f.write(a.out/'fixture.h')
current=(ROOT/'sdk/libgcm_cmd/src/rsx_legacy/commands_impl.h').read_text(encoding='utf-8')
signatures=['static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(DrawVertexArray)',
            'void RSX_FUNC(DrawVertexArray)',
            'static inline __attribute__((always_inline)) void RSX_FUNC_INTERNAL(DrawIndexArray)',
            'void RSX_FUNC(DrawIndexArray)', 'void RSX_FUNC(ClearSurface)']
bodies='\n\n'.join(f.extract(current,s) for s in signatures)
mutants={
    'missing-end': ('ps3dk_prof__auto_end(context,ticket);','(void)ticket;', 'begins==(unsigned)TEST_OBSERVED && ends==(unsigned)TEST_OBSERVED'),
    'unsafe-hook': ('defined(PS3TC_PROF_AUTO) && !RSX_UNSAFE','defined(PS3TC_PROF_AUTO)', 'begins==(unsigned)TEST_OBSERVED && ends==(unsigned)TEST_OBSERVED'),
    'changed-argument': ('{1u,type,start,count,0u,0u,0u,0u}', '{1u,type,start,count+1u,0u,0u,0u,0u}', 'memcmp(call,&expected,sizeof *call)==0'),
    'skip-delegate': ('RSX_FUNC_INTERNAL(DrawVertexArray)(context,type,start,count);', '(void)context;(void)type;(void)start;(void)count;', '(size_t)(after.current-actual)==n+markers'),
}
if a.mutant:
    old,new,expected=mutants[a.mutant]
    assert old in bodies
    bodies=bodies.replace(old,new) if a.mutant=='unsafe-hook' else bodies.replace(old,new,1)
prefix=r'''
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "fixture.h"
#include "ps3tc_prof_auto.h"
static uint32_t original[70032], actual[70032];
static unsigned begins, ends, allowed;
static ps3tc_auto_call expected;
uint32_t ps3dk_prof__auto_begin(struct _gcmCtxData *c,const ps3tc_auto_call *call,uint64_t pc) {
    (void)pc;
    begins++;
    assert(memcmp(call,&expected,sizeof *call)==0);
    if(!allowed) return 0;
    for(unsigned i=0;i<6;i++) *c->current++=0xabcd0000+i;
    return 1;
}
void ps3dk_prof__auto_end(struct _gcmCtxData *c,uint32_t ticket) {
    ends++;
    if(!ticket) return;
    for(unsigned i=0;i<6;i++) *c->current++=0xdcba0000+i;
}
#undef RSX_FUNC_INTERNAL
#undef RSX_FUNC
#define RSX_FUNC_INTERNAL(n) actual_internal_##n
#define RSX_FUNC(n) actual_##n
'''
suffix=r'''
static void one(unsigned kind,unsigned count,unsigned offset,unsigned index,unsigned admit) {
    gcmContextData before={original,original+70030}, after={actual,actual+70030};
    fixture_group groups[64];
    unsigned ng;
    memset(&expected,0,sizeof expected);
    expected.kind=kind;
    if(kind==1) {expected.mode=5;expected.start=3;expected.count=count;}
    if(kind==2) {expected.mode=5;expected.offset=offset;expected.count=count;expected.index_type=index;expected.location=1;}
    if(kind==3) expected.clear_mask=0xf3;
    fixture_group_count=0;
    if(kind==1) fixture_DrawVertexArray(&before,5,3,count);
    if(kind==2) fixture_DrawIndexArray(&before,5,offset,count,index,1);
    if(kind==3) fixture_ClearSurface(&before,0xf3);
    ng=fixture_group_count;memcpy(groups,fixture_groups,sizeof groups);
    fixture_group_count=begins=ends=0;allowed=admit;
    if(kind==1) actual_DrawVertexArray(&after,5,3,count);
    if(kind==2) actual_DrawIndexArray(&after,5,offset,count,index,1);
    if(kind==3) actual_ClearSurface(&after,0xf3);
    unsigned markers=TEST_OBSERVED ? admit*12:0;
    size_t n=(size_t)(before.current-original);
    assert((size_t)(after.current-actual)==n+markers);
    assert(begins==(unsigned)TEST_OBSERVED && ends==(unsigned)TEST_OBSERVED);
    assert(memcmp(original,actual+markers/2,n*4)==0);
    assert(fixture_group_count==ng);
    for(unsigned i=0;i<ng;i++) {
        assert(fixture_groups[i].count==groups[i].count);
        assert(fixture_groups[i].start-actual==(groups[i].start-original)+(ptrdiff_t)(markers/2));
    }
}
int main(void) {
    const unsigned counts[]={1,255,256,257,511,512,513,0x7ff00,0x7ff01,0xffffff};
    const unsigned offsets[]={0,2,4,126,128,130};
    unsigned cases=0;
    for(unsigned admit=0;admit<2;admit++) {
        for(unsigned i=0;i<sizeof counts/sizeof counts[0];i++) {
            one(1,counts[i],0,0,admit);cases++;
            for(unsigned j=0;j<sizeof offsets/sizeof offsets[0];j++)
                for(unsigned index=0;index<2;index++) {one(2,counts[i],offsets[j],index,admit);cases++;}
        }
        one(3,0,0,0,admit);cases++;
    }
    printf("canonical application words/groups + hook population: %u cases PASS\n",cases);
    return 0;
}
'''
(a.out/'test.c').write_bytes((prefix+bodies+suffix).encode())
commands=[]
for mode,defines in [('off',['-DTEST_OBSERVED=0','-DRSX_UNSAFE=0']),
                     ('unsafe',['-DTEST_OBSERVED=0','-DRSX_UNSAFE=1','-DPS3TC_PROF_AUTO=1']),
                     ('on',['-DTEST_OBSERVED=1','-DRSX_UNSAFE=0','-DPS3TC_PROF_AUTO=1'])]:
    cmd=[os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',*defines,
         '-I'+str(ROOT/'sdk/include'),'-I'+str(ROOT/'sdk/libgcm_cmd/src'),str(a.out/'test.c'),'-o',str(a.out/mode)]
    if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    build=subprocess.run(cmd,capture_output=True)
    (a.out/(mode+'-build.stderr')).write_bytes(build.stderr)
    assert build.returncode==0,build.stderr.decode()
    result=subprocess.run([str(a.out/mode)],capture_output=True,timeout=30)
    (a.out/(mode+'.stdout')).write_bytes(result.stdout);(a.out/(mode+'.stderr')).write_bytes(result.stderr)
    commands.append(dict(mode=mode,build=cmd,rc=result.returncode))
    (a.out/'commands.json').write_bytes((json.dumps(dict(baseline=baseline,runs=commands),indent=2)+'\n').encode())
    if result.returncode:
        if a.mutant and result.returncode==-6 and expected in result.stderr.decode():
            print(a.mutant,'KILLED',mode,expected)
            raise SystemExit(0)
        raise AssertionError(result.stderr.decode())
    print(mode,result.stdout.decode(),end='')
if a.mutant: raise AssertionError('SURVIVED '+a.mutant)
