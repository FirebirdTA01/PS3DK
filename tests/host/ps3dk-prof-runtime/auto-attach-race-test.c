/* Hold the owner in a mapping accessor until the real bounded shutdown has
 * returned. No prepared state may be read by the drainer or published later. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "auto_internal.h"
#include "prof_test_hooks.h"

static uint64_t reports[2048][2];
static uint32_t labels[256], words[64];
static gpp_ctx context;
static atomic_int entered,release_owner;
static void *report(uint32_t n)
{
    atomic_store_explicit(&entered,1,memory_order_release);
    while(!atomic_load_explicit(&release_owner,memory_order_acquire)) pal_sleep_us(100);
    return reports[n];
}
static void *label(uint32_t n) { return &labels[n]; }
static void flush(gpp_ctx *ctx) { (void)ctx;assert(0 && "attach must not flush"); }
static const gpp_dev device={report,label,flush};
void ps3dk_prof__auto_gcm_init_result(ps3dk_prof_gcm_ctx *,uint32_t,uint32_t,const void *,int32_t);
static void *owner(void *unused)
{
    (void)unused;
    ps3dk_prof__auto_gcm_init_result((void *)&context,sizeof words,sizeof words,words,0);
    return NULL;
}
int main(void)
{
    pthread_t thread;
    int rc;
    prof *p=g_prof;
    p->cfg.quiesce_timeout_us=1000;
    context=(gpp_ctx){words,words+64,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    assert(pthread_create(&thread,NULL,owner,NULL)==0);
    while(!atomic_load_explicit(&entered,memory_order_acquire)) pal_sleep_us(100);
    rc=ps3dk_prof_shutdown();
    atomic_store_explicit(&release_owner,1,memory_order_release);
    assert(pthread_join(thread,NULL)==0);
    assert(rc==PS3DK_PROF_ESINKFAIL);
    assert(!p->auto_lease.ctx && p->gpu_lifetime==0);
    assert(context.current==words);
    puts("AUTO attach/seal blocked owner refused without publication PASS");
    return 0;
}
