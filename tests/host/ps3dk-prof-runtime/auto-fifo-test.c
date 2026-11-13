/* Actual core/FIFO/AUTO setup; fake observer registry, no GPU execution. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "auto_internal.h"
#include "fifo_internal.h"
#include "prof_test_hooks.h"
extern int auto_test_fifo_fail;
extern unsigned auto_test_fifo_installs,auto_test_fifo_removes;
extern const ps3tc_fifo_observer *auto_test_fifo_observer;
static uint64_t reports[2048][2];
static uint32_t labels[256],words[64];
static gpp_ctx context;
static void *report(uint32_t n){return reports[n];}
static void *label(uint32_t n){return &labels[n];}
static void flush(gpp_ctx *c){(void)c;assert(0 && "attach cannot flush");}
static const gpp_dev device={report,label,flush};
void ps3dk_prof__auto_gcm_init_result(ps3dk_prof_gcm_ctx *,uint32_t,uint32_t,const void *,int32_t);
#if TEST_FIFO_POINT
static atomic_int entered,released,finished;
static void hook(unsigned point,unsigned value){
    (void)value;
    if(point!=TEST_FIFO_POINT)return;
    atomic_store_explicit(&entered,1,memory_order_release);
    while(!atomic_load_explicit(&released,memory_order_acquire))pal_sleep_us(100);
}
static void *owner(void *u){
    (void)u;
    ps3dk_prof__auto_gcm_init_result((void *)&context,sizeof words,sizeof words,words,0);
    atomic_store_explicit(&finished,1,memory_order_release);return NULL;
}
#endif
int main(void){
    context=(gpp_ctx){words,words+64,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
#if TEST_FIFO_POINT
    pthread_t t;prof *p=g_prof;
    p->cfg.quiesce_timeout_us=1000;
    ps3dk_prof__fifo_test_hook=hook;
    assert(pthread_create(&t,NULL,owner,NULL)==0);
    while(!atomic_load_explicit(&entered,memory_order_acquire) &&
          !atomic_load_explicit(&finished,memory_order_acquire))pal_sleep_us(100);
    assert(atomic_load_explicit(&entered,memory_order_acquire));
    int rc=ps3dk_prof_shutdown();
    atomic_store_explicit(&released,1,memory_order_release);
    assert(pthread_join(t,NULL)==0);
    if(TEST_FIFO_POINT==3)assert(rc==PS3DK_PROF_ESINKFAIL && !p->auto_lease.ctx);
    else assert(rc==PS3DK_PROF_ENOTQUIESCED && p->auto_lease.ctx==&context);
    assert(g_fifo.state==FS_SEALED && !g_fifo.instance && !g_fifo.block && !g_fifo.ring);
    assert(auto_test_fifo_installs==1 && auto_test_fifo_removes==1 && !auto_test_fifo_observer);
#else
    auto_test_fifo_fail=1;
    ps3dk_prof__auto_gcm_init_result((void *)&context,sizeof words,sizeof words,words,0);
    assert(!g_prof->auto_lease.ctx && !auto_test_fifo_observer && !g_fifo.block);
    assert(g_fifo.state==FS_NONE && !g_fifo.instance);
#endif
    assert(context.current==words);
    puts("AUTO FIFO refusal and unpublished cleanup PASS");return 0;
}
