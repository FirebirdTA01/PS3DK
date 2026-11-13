#!/usr/bin/env python3
"""Public report/label write seams preserve pinned application command words."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',action='store_true')
a=ap.parse_args()
current=(root/'sdk/libgcm_cmd/src/rsx_legacy/commands_impl.h').read_text()
assert 'ps3dk_prof__auto_resource_write' in current,'AUTO_RESOURCE_SEAMS'
spec=importlib.util.spec_from_file_location('fixture',root/'tests/host/ps3dk-prof-runtime/auto-rsx-emitter-fixture.py')
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)
baseline=(root/'tests/host/ps3dk-prof-runtime/data/auto-resource-60fe5924.h').read_text()
assert hashlib.sha256(baseline.encode()).hexdigest()=='13d71d29d3fe73c0c4c597441e61819bd1cccd571c592688ef561e7f1c3dd27e'
names=['SetWriteBackendLabel','SetWriteTextureLabel','SetWriteCommandLabel','SetReport','SetTimeStamp']
actual='\n'.join(f.extract(current,'void RSX_FUNC('+n+')') for n in names)
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <rsx/nv40.h>
#include "ps3tc_prof_auto.h"
typedef uint8_t u8;typedef uint32_t u32;
typedef struct _gcmCtxData {u32 *current,*end;} gcmContextData;
static unsigned groups,notifications;
static uint32_t want_kind,want_index;
static void reserve(gcmContextData *c,unsigned n) {assert((size_t)(c->end-c->current)>=n);groups++;}
void ps3dk_prof__auto_resource_write(struct _gcmCtxData *c,uint32_t kind,uint32_t index) {
 (void)c;assert(!groups && !notifications);assert(kind==want_kind && index==want_index);notifications++;
}
#define RSX_CONTEXT_CURRENT_BEGIN(n) reserve(context,n)
#define RSX_CONTEXT_CURRENTP (context->current)
#define RSX_CONTEXT_CURRENT_END(n) context->current+=(n)
#define RSX_METHOD(m,n) (((uint32_t)(n)<<18)|(m))
#define RSX_FUNC(n) original_##n
'''
middle='\n#undef RSX_FUNC\n#define RSX_FUNC(n) actual_##n\n'
suffix=r'''
static void emit(gcmContextData *c,unsigned function,uint32_t index,int actual) {
#define RUN(n,...) do {if(actual)actual_##n(c,__VA_ARGS__);else original_##n(c,__VA_ARGS__);} while(0)
 switch(function) {
 case 0:RUN(SetWriteBackendLabel,(u8)index,0x12345678);break;
 case 1:RUN(SetWriteTextureLabel,(u8)index,0x12345678);break;
 case 2:RUN(SetWriteCommandLabel,(u8)index,0x12345678);break;
 case 3:RUN(SetReport,1,index);break;
 case 4:RUN(SetTimeStamp,index);break;
 default:assert(0);
 }
}
int main(void) {
 unsigned cases=0;
 for(unsigned fn=0;fn<5;fn++) for(uint32_t i=0;i<(fn<3 ? 256u:2050u);i++) {
    uint32_t before[8]={0},after[8]={0};
    gcmContextData b={before,before+8},c={after,after+8};
    groups=notifications=0;emit(&b,fn,i,0);unsigned original_groups=groups;
    groups=notifications=0;want_kind=fn<3 ? 2:1;want_index=i;
    emit(&c,fn,i,1);
    assert(groups==original_groups && notifications==TEST_NOTIFY);
    assert(c.current-after==b.current-before && !memcmp(before,after,sizeof before));cases++;
 }
 printf("%u report/label word projections and pre-reservation notifications PASS\n",cases);
}
'''
a.out.mkdir(parents=True,exist_ok=False)
(a.out/'test.c').write_bytes((prefix+baseline+middle+actual+suffix).encode())
commands=[]
for name,flags in [('off',['-DTEST_NOTIFY=0','-DRSX_UNSAFE=0']),
                   ('safe',['-DTEST_NOTIFY=1','-DPS3TC_PROF_AUTO=1','-DRSX_UNSAFE=0']),
                   ('unsafe',['-DTEST_NOTIFY=1','-DPS3TC_PROF_AUTO=1','-DRSX_UNSAFE=1'])]:
    cmd=[os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',*flags,
         '-I'+str(root/'sdk/libgcm_cmd/src'),'-I'+str(root/'sdk/include'),str(a.out/'test.c'),'-o',str(a.out/name)]
    if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    r=subprocess.run(cmd,capture_output=True);(a.out/(name+'-build.stderr')).write_bytes(r.stderr)
    assert r.returncode==0,r.stderr.decode()
    r=subprocess.run([str(a.out/name)],capture_output=True,timeout=10)
    (a.out/(name+'.stdout')).write_bytes(r.stdout);(a.out/(name+'.stderr')).write_bytes(r.stderr)
    commands.append(dict(build=cmd,rc=r.returncode));(a.out/'commands.json').write_bytes((json.dumps(commands,indent=2)+'\n').encode())
    assert r.returncode==0,r.stderr.decode()
    print(name,r.stdout.decode(),end='')
