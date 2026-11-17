/* Actual public live API + real agent/OS shim over append and rolling files. */
#define RUNTIME_PPU_ENTRY live_existing_d5_entry
#include "runtime_ppu_test.c"
#include "live_api.h"
#include "live_agent.h"
#include "live_os.h"
static unsigned live_calls,listen_calls,watch_alloc;
static uint64_t live_allocations;
void *__real_malloc(size_t);void *__real_calloc(size_t,size_t);void *__real_realloc(void *,size_t);
void *__wrap_malloc(size_t n){if(__atomic_load_n(&watch_alloc,__ATOMIC_RELAXED))__atomic_fetch_add(&live_allocations,1,__ATOMIC_RELAXED);return __real_malloc(n);}
void *__wrap_calloc(size_t n,size_t s){if(__atomic_load_n(&watch_alloc,__ATOMIC_RELAXED))__atomic_fetch_add(&live_allocations,1,__ATOMIC_RELAXED);return __real_calloc(n,s);}
void *__wrap_realloc(void *p,size_t n){if(__atomic_load_n(&watch_alloc,__ATOMIC_RELAXED))__atomic_fetch_add(&live_allocations,1,__ATOMIC_RELAXED);return __real_realloc(p,n);}
int __real_ps3dk_prof__live_agent_start(live_agent *,const live_config *,const live_source *);
int __wrap_ps3dk_prof__live_agent_start(live_agent *a,const live_config *c,const live_source *s)
{++live_calls;return __real_ps3dk_prof__live_agent_start(a,c,s);}
int __real_ps3dk_prof__live_os_listen(uint16_t,int,int *,uint16_t *);
int __wrap_ps3dk_prof__live_os_listen(uint16_t p,int loop,int *fd,uint16_t *bound)
{++listen_calls;return __real_ps3dk_prof__live_os_listen(p,loop,fd,bound);}
int main(int argc,char **argv)
{
    ps3dk_prof_config c;int append,rc;unsigned worker_before;uint64_t allocated_before;
    if(argc!=3){fputs("usage: runtime-live-rolling append-live|rolling-live OUTPUT\n",stderr);return 2;}
    append=!strcmp(argv[1],"append-live");CHECK(append||!strcmp(argv[1],"rolling-live"),"known-live-case");
    directory=argv[2];config(&c);
    if(append){options.enabled=0;CHECK(ps3dk_prof_test_rolling_set(&options)==0,"live-append-disabled");CHECK(ps3dk_prof_init(&c)==0,"live-append-real-init");}
    else start(&c);
    CHECK(ps3dk_prof_thread_begin("owner")==0&&PS3DK_PROF_CAPTURE_WHOLE_RUN()==0,"live-owner-window");
    ps3dk_prof_test_hooks.live_ephemeral_port=1;
    worker_before=workers;allocated_before=allocated;
    __atomic_store_n(&watch_alloc,1,__ATOMIC_RELAXED);rc=ps3dk_prof_live_start(NULL);__atomic_store_n(&watch_alloc,0,__ATOMIC_RELAXED);
    if(append){
        CHECK(rc==0&&ps3dk_prof__live_port()!=0,"append-live-real-listener");
        CHECK(live_calls==1&&listen_calls==1&&workers==worker_before+1,"append-live-real-agent-thread");
        CHECK(ps3dk_prof_live_start(NULL)==PS3DK_PROF_EALREADY,"append-live-once");
        CHECK(ps3dk_prof_live_stop(0,2000000)==0,"append-live-real-stop");
    }else{
        CHECK(rc==PS3DK_PROF_ENOTSUP,"rolling-live-refused-before-consumer");
        __atomic_store_n(&watch_alloc,1,__ATOMIC_RELAXED);rc=ps3dk_prof_live_start(NULL);__atomic_store_n(&watch_alloc,0,__ATOMIC_RELAXED);
        CHECK(rc==PS3DK_PROF_ENOTSUP,"rolling-live-repeat-refusal");
        CHECK(!live_calls,"rolling-live-no-agent");CHECK(!listen_calls,"rolling-live-no-listener");
        CHECK(workers==worker_before,"rolling-live-no-thread");
        CHECK(!__atomic_load_n(&live_allocations,__ATOMIC_RELAXED)&&allocated==allocated_before,"rolling-live-no-allocation");
        CHECK(!ps3dk_prof__live_port()&&ps3dk_prof_live_stop(0,0)==PS3DK_PROF_ENOTINIT,"rolling-live-no-enrollment");
    }
    ps3dk_prof_frame();zone(&early);CHECK(ps3dk_prof_drain()==0,"live-capture-remains-writable");
    if(append){CHECK(ps3dk_prof_shutdown()==0,"append-live-public-shutdown");snapshot("capture.prof");}
    else finished(0,PFR_SEALED,0);
    manifest(argv[1],0);puts("PASS actual legacy live runtime admission");return 0;
}
