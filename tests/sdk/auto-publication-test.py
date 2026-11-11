#!/usr/bin/env python3
"""Real flush/reset bodies and two-phase protocol, with host publication hooks."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--sanitize',action='store_true')
ap.add_argument('--mutant',choices=['flush-before-put','wrap-phase2-missing','reset-missing'])
a=ap.parse_args()
source=(root/'sdk/libgcm_cmd/src/rsx_legacy/buffer_impl.h').read_text()
assert 'ps3dk_prof__auto_publish_before' in source,'AUTO_PUBLICATION_SEAM'
a.out.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('fixture',root/'tests/host/ps3dk-prof-runtime/auto-rsx-emitter-fixture.py')
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)
bodies='\n'.join(f.extract(source,'void RSX_FUNC('+name+')') for name in ('ResetCommandBuffer','FlushBuffer','Finish'))
protocol=(root/'sdk/libgcm_cmd/src/ps3tc_fifo_wrap_protocol.h').read_text()
if a.mutant=='flush-before-put':
    bodies=bodies.replace('ctrl->put = offset;\n#ifdef PS3TC_PROF_AUTO\n\t__sync();\n\tps3dk_prof__auto_publish_after(context,frontier);',
                          '#ifdef PS3TC_PROF_AUTO\n\t__sync();\n\tps3dk_prof__auto_publish_after(context,frontier);\n\tctrl->put = offset;\n#else\n\tctrl->put = offset;')
elif a.mutant=='wrap-phase2-missing':
    protocol=protocol.replace('p->publish_after(p->arg, frontier2);','(void)frontier2;')
elif a.mutant=='reset-missing':
    bodies=bodies.replace('ps3dk_prof__auto_context_reset(context);','(void)context;')
(a.out/'ps3tc_fifo_wrap_protocol.h').write_bytes(protocol.encode())
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ps3tc_prof_auto.h"
#include "ps3tc_fifo_wrap_protocol.h"
typedef uint32_t u32;
typedef struct _gcmCtxData { uint32_t *begin,*current,*end; } gcmContextData;
typedef struct { volatile uint32_t get,put,ref; } gcmControlRegister;
static gcmControlRegister ctrl;
static unsigned syncs,pre,post,resets,jumps;
static uint32_t expected_put,old_put;
static gcmControlRegister *gcmGetControlRegister(void) { return &ctrl; }
static int gcmAddressToOffset(uint32_t *p,uint32_t *v) { (void)p;*v=expected_put;return 0; }
static void __sync(void) { ++syncs; }
static void usleep(unsigned n) { (void)n;ctrl.get=ctrl.put; }
uint32_t ps3dk_prof__auto_publish_before(struct _gcmCtxData *c) {
 (void)c;assert(ctrl.put==old_put);assert(syncs);pre++;return 47;
}
void ps3dk_prof__auto_publish_after(struct _gcmCtxData *c,uint32_t serial) {
 (void)c;assert(serial==47 && ctrl.put==expected_put && syncs>=2);post++;
}
void ps3dk_prof__auto_context_reset(struct _gcmCtxData *c) { (void)c;assert(!jumps);resets++; }
static void test_SetJumpCommand(gcmContextData *c,uint32_t n) {
 (void)c;(void)n;assert(resets==TEST_AUTO);jumps++;
}
static void test_SetReferenceCommand(gcmContextData *c,uint32_t n) { (void)c;ctrl.ref=n; }
#define RSX_FUNC(n) test_##n
'''
suffix=r'''
static uint32_t words[64];
static uint64_t ticks;
static unsigned wp_before,wp_after,barriers;
static void barrier(void *p) { (void)p;barriers++; }
static uint64_t now(void *p) { (void)p;return ticks++; }
static void pause_wrap(void *p,uint32_t want,uint32_t n,unsigned phase,uint64_t elapsed) {
 (void)p;(void)n;(void)phase;(void)elapsed;ctrl.get=want;
}
#ifdef PS3TC_PROF_AUTO
static uint32_t before_wrap(void *p) {
 (void)p;assert(barriers);assert(ctrl.put==(wp_before ? 16u:0u));
 return ++wp_before;
}
static void after_wrap(void *p,uint32_t serial) {
 (void)p;assert(serial==wp_before && serial==wp_after+1);
 assert(ctrl.put==(serial==1 ? 16u:4u));assert(barriers>=serial+1);wp_after++;
}
#endif
int main(void) {
 gcmContextData c={words,words+4,words+64};
 old_put=ctrl.put=12;expected_put=16;
 test_FlushBuffer(&c);assert(ctrl.put==16 && pre==TEST_AUTO && post==TEST_AUTO);
 old_put=ctrl.put;expected_put=24;syncs=0;
 test_Finish(&c,42);assert(ctrl.ref==42 && pre==TEST_AUTO*2 && post==TEST_AUTO*2);
 test_ResetCommandBuffer(&c);assert(ctrl.put==0x1000 && ctrl.get==0x1000 && c.current==c.begin);
 assert(resets==TEST_AUTO && jumps==1);
 ctrl.get=ctrl.put=0;
 ps3tc_fifo_port port={&ctrl.put,&ctrl.get,barrier,now,pause_wrap,NULL
#ifdef PS3TC_FIFO_OBSERVE
 ,NULL,NULL
#endif
#ifdef PS3TC_PROF_AUTO
 ,before_wrap,after_wrap
#endif
 };
 ps3tc_fifo_wrap_protocol(words+4,16,4,&port);
 assert(words[4]==(PS3TC_NV40_JUMP_FLAG|4) && ctrl.get==4 && ctrl.put==4);
 assert(wp_before==TEST_AUTO*2 && wp_after==TEST_AUTO*2);
 puts("flush, reset and both wrap publication boundaries PASS");
}
'''
(a.out/'test.c').write_bytes((prefix+bodies+suffix).encode())
rows=[]
for name,flags in [('off',['-DTEST_AUTO=0']),('on',['-DTEST_AUTO=1','-DPS3TC_PROF_AUTO=1']),
                   ('fifo-on',['-DTEST_AUTO=1','-DPS3TC_PROF_AUTO=1','-DPS3TC_FIFO_OBSERVE=1'])]:
    cmd=[os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',*flags,
         '-I'+str(a.out),'-I'+str(root/'sdk/libgcm_cmd/src'),str(a.out/'test.c'),'-o',str(a.out/name)]
    if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    r=subprocess.run(cmd,capture_output=True);(a.out/(name+'-build.stderr')).write_bytes(r.stderr)
    assert r.returncode==0,r.stderr.decode()
    r=subprocess.run([str(a.out/name)],capture_output=True,timeout=10)
    (a.out/(name+'.stdout')).write_bytes(r.stdout);(a.out/(name+'.stderr')).write_bytes(r.stderr)
    rows.append(dict(build=cmd,rc=r.returncode));(a.out/'commands.json').write_bytes((json.dumps(rows,indent=2)+'\n').encode())
    if a.mutant and name!='off':
        assert r.returncode!=0 and b'Assertion' in r.stderr, 'mutant must fail a runtime assertion'
        print(name,a.mutant,'rejected')
        continue
    assert r.returncode==0,r.stderr.decode()
    print(name,r.stdout.decode(),end='')
