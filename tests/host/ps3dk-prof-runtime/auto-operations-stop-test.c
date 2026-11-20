/* The owner is parked between the original command and its end marker. A
 * bounded seal must retain the live claim and publish an unacknowledged bound. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "auto_internal.h"
#include "prof_test_hooks.h"
#include "ps3tc_prof_auto.h"
#ifndef TEST_STOP_VISIT
#define TEST_STOP_VISIT 0
#endif
#ifndef TEST_STOP_SEAL
#define TEST_STOP_SEAL 0
#endif

static uint64_t reports[2048][2];
static uint32_t labels[256],words[256];
static gpp_ctx context;
static atomic_int entered,release_owner;
#if TEST_STOP_VISIT
static atomic_int drainer_parked,release_drainer,owner_may_visit,owner_returned;
static uint32_t shutdown_incarnation;
static void let_owner_ack(void) {
    atomic_store_explicit(&owner_may_visit,1,memory_order_release);
    while(!atomic_load_explicit(&owner_returned,memory_order_acquire)) pal_sleep_us(100);
}
/* Force the shutdown caller's foreign CONFIG claim to overlap the owner's
 * one-shot retirement, if that claim still exists. With the fix, the caller
 * never claims CONFIG and releases the owner at its ordinary ack boundary. */
uint32_t __real_ps3dk_prof__auto_config_begin(auto_config_store *s);
uint32_t __wrap_ps3dk_prof__auto_config_begin(auto_config_store *s) {
#if TEST_STOP_SEAL
    /* The sealing thread may freeze CONFIG after owner_capture saw NULL but
     * before its claim. Force that ordering without releasing the drainer. */
    if(shutdown_incarnation && g_tls.incarnation!=shutdown_incarnation && LD_ACQ(&g_prof->stop_latch))
        p_auto_seal_claim();
#endif
    uint32_t ticket=__real_ps3dk_prof__auto_config_begin(s);
    if(ticket && shutdown_incarnation && g_tls.incarnation==shutdown_incarnation && LD_ACQ(&g_prof->stop_latch))
        let_owner_ack();
    return ticket;
}
static void before_ack(void) {
    if(g_tls.incarnation==shutdown_incarnation) let_owner_ack();
}
static void park_drainer(void) {
    atomic_store_explicit(&drainer_parked,1,memory_order_release);
    while(!atomic_load_explicit(&release_drainer,memory_order_acquire)) pal_sleep_us(100);
}
/* Install before automatic startup creates the drainer, never by racing a
 * live callback pointer. Keep CONFIG unsealed while the global STOP is set. */
__attribute__((constructor(99))) static void install_pause(void) {
    ps3dk_prof_test_hooks.drainer_after_snapshot=park_drainer;
    ps3dk_prof_test_hooks.before_ack_store=before_ack;
}
#endif
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static void flush(gpp_ctx *c) {(void)c;assert(0);}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(ps3dk_prof_gcm_ctx *,uint32_t,uint32_t,const void *,int32_t);
static void *owner(void *unused) {
    (void)unused;
    ps3tc_auto_call clear={3,0,0,0,0,0,0,0xf3};
    ps3dk_prof__auto_gcm_init_result((void *)&context,sizeof words,sizeof words,words,0);
#if TEST_STOP_VISIT
    atomic_store_explicit(&entered,1,memory_order_release);
    while(!LD_ACQ(&g_prof->stop_latch)) pal_sleep_us(100);
    while(!atomic_load_explicit(&owner_may_visit,memory_order_acquire)) pal_sleep_us(100);
    assert(!ps3dk_prof__auto_begin((void *)&context,&clear,321));
    assert(g_tls.acked && !g_tls.in_gpu);
    atomic_store_explicit(&owner_returned,1,memory_order_release);
    atomic_store_explicit(&release_drainer,1,memory_order_release);
#else
    uint32_t ticket=ps3dk_prof__auto_begin((void *)&context,&clear,321);
    assert(ticket && g_tls.in_gpu);
    for(unsigned i=0;i<6;i++) *context.current++=0xcafe0000+i;
    atomic_store_explicit(&entered,1,memory_order_release);
    while(!atomic_load_explicit(&release_owner,memory_order_acquire)) pal_sleep_us(100);
    ps3dk_prof__auto_end((void *)&context,ticket);
    assert(!g_tls.in_gpu && g_tls.acked);
#endif
    return NULL;
}
int main(void) {
    pthread_t thread;prof *p=g_prof;
    p->cfg.quiesce_timeout_us=TEST_STOP_VISIT ? 1000000:1000;
    context=(gpp_ctx){words,words+256,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
#if TEST_STOP_VISIT
    shutdown_incarnation=g_tls.incarnation;
    assert(shutdown_incarnation);
    while(!atomic_load_explicit(&drainer_parked,memory_order_acquire)) pal_sleep_us(100);
#endif
    assert(!pthread_create(&thread,NULL,owner,NULL));
    while(!atomic_load_explicit(&entered,memory_order_acquire)) pal_sleep_us(100);
    int rc=ps3dk_prof_shutdown();
    atomic_store_explicit(&release_owner,1,memory_order_release);
    assert(!pthread_join(thread,NULL));
#if TEST_STOP_VISIT
    assert(rc==PS3DK_PROF_OK && !p->auto_lease.published && context.current==words);
    puts("AUTO stopped owner visit acknowledges without marker PASS");
#else
    assert(rc==PS3DK_PROF_ENOTQUIESCED);
    assert(p->auto_lease.ctx==&context && p->auto_lease.published);
    assert(context.current==words+18); /* the clear between its two report markers */
    puts("AUTO in-flight owner seal retains claim and lower bound PASS");
#endif
}
