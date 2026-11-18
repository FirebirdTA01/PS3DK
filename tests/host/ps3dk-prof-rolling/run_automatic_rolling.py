#!/usr/bin/env python3
"""Actual AUTO constructor ordering with a traced core seam, C/C++ and GC.

The separate native-producer runtime gate proves the real setter/planner/core.
This gate proves weak-object selection, before-init ordering, and no fallback;
it makes no complete-capture or target-execution claim.
"""
import argparse
import json
from pathlib import Path
import shutil
from run_runtime_spu import run,sha

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
APP=r'''
#include <ps3dk/prof_rolling.h>
#include <ps3dk/prof_auto.h>
#include "prof_spu.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "pal.h"
#ifdef __cplusplus
}
#endif
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if CASE == 1 || CASE == 4 || CASE == 5
PS3DK_PROF_DEFINE_AUTOMATIC_ROLLING_OPTIONS(PS3DK_PROF_ROLLING_AUTO_FIFO_INIT(UINT64_C(67108864),UINT64_C(67108864)));
#elif CASE == 2
PS3DK_PROF_DEFINE_AUTOMATIC_ROLLING_OPTIONS(PS3DK_PROF_ROLLING_OPTIONS_INIT);
#elif CASE == 3
PS3DK_PROF_DEFINE_AUTOMATIC_ROLLING_OPTIONS(@BAD@);
#endif
#if CASE == 4 || CASE == 6
PS3DK_PROF_DEFINE_AUTOMATIC_RING_OPTIONS(PS3DK_PROF_AUTO_RING_OPTIONS_INIT(16777216,16384));
#elif CASE == 5
PS3DK_PROF_DEFINE_AUTOMATIC_RING_OPTIONS({0u,2u,16777216u,16384u});
#endif
#ifdef __cplusplus
extern "C" {
extern const char ps3dk_prof__auto_app[]="startup-test";
extern const char ps3dk_prof__auto_build_id[]="startup-test";
#else
const char ps3dk_prof__auto_app[]="startup-test";
const char ps3dk_prof__auto_build_id[]="startup-test";
#endif
static unsigned calls,ring_calls,initialized,registered,diagnostics;
void ps3dk_prof_config_defaults(ps3dk_prof_config *c){memset(c,0,sizeof *c);}
int ps3dk_prof_set_rolling_options(const ps3dk_prof_rolling_options *o)
{assert(!initialized);++calls;assert(o->size==192);return o->version==1?0:PS3DK_PROF_EINVAL;}
int ps3dk_prof__auto_build_ring(const ps3dk_prof_auto_ring_options *o)
{assert(!initialized);assert(calls==(CASE==6?0u:1u));++ring_calls;return o->size==16?0:PS3DK_PROF_EINVAL;}
int ps3dk_prof_init_ex(const ps3dk_prof_config *c,const ps3dk_prof_ext_config *e)
{assert(c&&e);++initialized;return 0;}
int ps3dk_prof_thread_begin(const char *n){assert(n);++registered;return 0;}
void ps3dk_prof_thread_end(void){}
int ps3dk_prof_shutdown(void){return 0;}
long pal_write(int fd,const void *p,unsigned long n,int *error)
{assert(fd==2&&error);++diagnostics;return (long)fwrite(p,1,n,stderr);}
#ifdef __cplusplus
}
#endif
__attribute__((constructor(101))) static void user_constructor(void)
{
    if(calls!=((CASE==0||CASE==6)?0u:1u)||ring_calls!=((CASE>=4)?1u:0u)||
       initialized!=((CASE==3||CASE==5)?0u:1u)||registered!=initialized||diagnostics!=((CASE==3||CASE==5)?1u:0u)){
        fputs("AUTO_STARTUP_ASSERT[options-before-user-constructor]\n",stderr);exit(1);
    }
}
int main(void){puts("actual constructor100 selection before user101 PASS");return 0;}
'''.replace('@BAD@','{192u,2u,'+','.join(['0']*33)+',{0}}')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,required=True)
    a=p.parse_args();out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    lib=ROOT/'sdk/libps3dkprof/src';inc=out/'include/ps3dk';inc.mkdir(parents=True)
    public=[ROOT/'sdk/include/ps3dk'/x for x in ('prof.h','prof_auto.h','prof_fifo.h','prof_rolling.h')]+[lib/'rsx/prof_gpu.h']
    for x in public:shutil.copyfile(x,inc/x.name)
    flags=['-O1','-Wall','-Wextra','-Werror','-Wno-prio-ctor-dtor','-ffunction-sections','-fdata-sections',
           '-DPS3DK_PROF=1','-DPS3DK_PROF_PAL_HOST','-DPS3DK_PROF_WITH_SPU','-DPS3DK_PROF_WITH_FIFO','-DPS3DK_PROF_WITH_AUTO',
           '-I',str(inc.parent)]
    for x in ('core','pal','spu','rsx','fifo','auto_rsx'):flags+=['-I',str(lib/x)]
    startup=lib/'auto_rsx/auto_startup.c'
    pins={str(x):sha(x) for x in [Path(__file__),startup,*public]}
    rows=[]
    rc=run(['cc','-std=c11',*flags,'-c',str(startup),'-o',str(out/'startup.o')],out,'startup',60)
    if rc:raise SystemExit('startup compilation failed, not behavioral evidence')
    for lang,cc,std in [('c','cc','c11'),('cpp','c++','c++17')]:
        for case in range(7):
            name=f'{lang}-{case}';source=out/(name+'.'+lang);source.write_text(APP)
            exe=out/name
            rc=run([cc,'-std='+std,*flags,'-DCASE='+str(case),str(source),str(out/'startup.o'),'-Wl,--gc-sections','-o',str(exe)],out,name+'-build',60)
            if rc:raise SystemExit('app compilation failed, not behavioral evidence: '+name)
            rc=run([str(exe)],out,name,15)
            text=(out/(name+'.stderr')).read_text()
            good=rc==0 and (case!=3 or text.count('rolling configuration refused')==1) and (case!=5 or text.count('PS3_PROF_AUTO_RING options refused')==1)
            rows.append(dict(name=name,rc=rc,passed=good))
    stable=all(sha(Path(x))==h for x,h in pins.items())
    (out/'results.json').write_text(json.dumps(dict(inputs=pins,inputs_unchanged=stable,rows=rows),indent=2)+'\n')
    if not stable or not all(x['passed'] for x in rows):raise SystemExit('AUTO_STARTUP_ASSERT[static-options-before-init-no-fallback]')
    print('14 actual constructor selection cases PASS (rolling/ring separately and together; traced core seam, not runtime capture)')

if __name__=='__main__':main()
