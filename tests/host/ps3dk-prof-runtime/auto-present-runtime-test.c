/* Presentation evidence on the real runtime, with a controlled timebase and
 * synthetic device writes. The application drives the observed hooks the way
 * rsx/gcm_sys.h (gcmGetFlipStatus) and cell/gcm.h (cellGcmWaitFlipComplete)
 * do. "work" and "sleep" only advance the clock; the runtime cannot tell them
 * apart, which is the point: between two nonblocking polls the owner may have
 * done anything.
 *
 * Rules under test (docs/design/ps3dk-prof-auto-rsx.md, COVERAGE):
 *  - only a flip-wait scope is waiting evidence; its outer elapsed time is
 *    counted once, and polls inside it are not counted again as cadence;
 *  - polls outside a scope are polling cadence: a run of pending polls with
 *    every gap within 1 ms, closed by a done poll within 1 ms; a larger gap
 *    breaks the run;
 *  - a run or scope that crosses a frame boundary (flip-derived or explicit)
 *    is charged to neither frame and counted as spanning in both. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ps3dk/prof.h>
#include <ps3dk/prof_auto.h>
#ifndef TEST_SPANNING
#define TEST_SPANNING 0
#endif
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
static uint64_t reports[2048][2];
static uint32_t labels[256],words[4096];
static gpp_ctx context;
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static uint32_t anchors;
static void complete(gpp_ctx *ctx,uint32_t clock_serial) {
    uint32_t op=ps3dk_prof__auto_publish_before((void *)ctx);
    ps3dk_prof__auto_publish_after((void *)ctx,op);
    for(unsigned r=1024;r<2048;r++) st64(&reports[r][0],100000+r);
    if(clock_serial) {st64(&reports[1024+(clock_serial-1)%16][0],10000*clock_serial);st32(&labels[246],clock_serial);}
    st32(&labels[247],op);
}
static void flush(gpp_ctx *ctx) {assert(ctx==&context);complete(ctx,++anchors);}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_present(uint64_t out[8]);
/* Weak, so this control also links against a runtime without flip-wait
 * scopes; such a runtime fails the first assertion below (review finding 1). */
uint64_t ps3dk_prof__auto_flip_wait_begin(void) __attribute__((weak));
void ps3dk_prof__auto_flip_wait_end(uint64_t,int32_t) __attribute__((weak));

/* The host timebase is nanoseconds. Every read advances it by one tick, so
 * back-to-back calls are 1 ns apart plus whatever the drainer read meanwhile. */
static uint64_t clock_base,clock_reads,clock_advance;
static uint64_t fake_clock(void)
{
    return clock_base+__atomic_add_fetch(&clock_reads,1,__ATOMIC_SEQ_CST)+
           __atomic_load_n(&clock_advance,__ATOMIC_SEQ_CST);
}
static void elapse(uint64_t ns) {__atomic_add_fetch(&clock_advance,ns,__ATOMIC_SEQ_CST);}
#define MS UINT64_C(1000000)

static void poll(uint32_t status)
{
    uint64_t before=ps3dk_prof__auto_flip_status_begin();
    ps3dk_prof__auto_flip_status_end(before,status);
}
static void flip(void) {ps3dk_prof__auto_flip((void *)&context,1);}
static void clear(void)
{
    ps3tc_auto_call call={3,0,0,0,0,0,0,0xf3};
    uint32_t ticket=ps3dk_prof__auto_begin((void *)&context,&call,0x1234);
    assert(ticket);
    for(unsigned n=0;n<6;n++) *context.current++=0xc1ea0000u+n;
    ps3dk_prof__auto_end((void *)&context,ticket);
}
/* cellGcmWaitFlipComplete as the header writes it: a scope around polls and
 * sleeps of `step` each, done after `pending` pending polls (or a timeout). */
static void wait_flip(unsigned pending,uint64_t step,int32_t rc)
{
    uint64_t scope=ps3dk_prof__auto_flip_wait_begin();
    assert(scope);
    for(unsigned i=0;i<pending;i++) {poll(1);elapse(step);}
    if(!rc) poll(0);
    ps3dk_prof__auto_flip_wait_end(scope,rc);
}
static uint64_t p[8];
static void present(void) {ps3dk_prof__auto_test_present(p);}

int main(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    clock_base=(uint64_t)ts.tv_sec*1000000000u+(uint64_t)ts.tv_nsec+1000000000u;
    ps3dk_prof_test_hooks.clock=fake_clock;
    context=(gpp_ctx){words,words+4096,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);

#if !TEST_SPANNING
    /* frame 0: pending poll, 10 ms of application work, done poll. Two
     * nonblocking polls establish no wait: no flip-wait evidence, and the
     * 10 ms gap breaks the cadence run. */
    flip();clear();
    poll(1);elapse(10*MS);poll(0);
    present();
    assert(p[0]==0 && p[1]==0); /* no waiting evidence from two polls */
    assert(p[2]==0 && p[4]==1); /* a broken cadence run, not a complete one */

    /* frame 1: useful CPU work between every poll, each gap 0.5 ms: a complete
     * cadence run of >= 20 ms, still no waiting evidence. */
    flip();clear();
    for(unsigned i=0;i<40;i++) {poll(1);elapse(MS/2);}
    poll(0);
    present();
    assert(p[0]==0 && p[1]==0);
    assert(p[2]==1 && p[3]>=20*MS && p[3]<21*MS && p[4]==0);

    /* frame 2: the flip-wait helper with nested polls and 1 ms sleeps: one
     * scope, counted once; its polls are not cadence. Then an unrelated 5 ms
     * sleep after a pending observation (a broken run, no wait), then a helper
     * that times out (a scope with the timeout counted). */
    flip();clear();
    wait_flip(20,MS,0);
    present();
    assert(p[0]==1 && p[1]>=20*MS && p[1]<21*MS && p[2]==0 && p[4]==0);
    poll(1);elapse(5*MS);poll(0);
    present();
    assert(p[0]==1 && p[1]<21*MS && p[4]==1);
    wait_flip(3,MS,(int32_t)0x802b0001);
    present();
    assert(p[0]==2 && p[1]>=23*MS && p[7]==1 && p[2]==0);
    assert(anchors==1); /* frame 0 */
#else
    /* frame 0 -> 1: a pending run across a flip-derived boundary, done in
     * frame 1 with short gaps. Charged to neither frame. */
    flip();clear();
    poll(1);
    flip();clear();
    poll(0);
    present();
    assert(p[2]==0 && p[3]==0 && p[5]==1); /* not a complete run of frame 1 */

    /* frame 1 -> 2: a flip-wait scope across a boundary: charged to neither;
     * frame 2 then has a complete scope of its own. */
    {
        uint64_t scope=ps3dk_prof__auto_flip_wait_begin();
        assert(scope);
        elapse(4*MS);
        flip();clear();
        elapse(4*MS);
        ps3dk_prof__auto_flip_wait_end(scope,0);
    }
    present();
    assert(p[0]==0 && p[1]==0 && p[6]==1);
    wait_flip(2,MS,0);
    present();
    assert(p[0]==1 && p[1]>=2*MS && p[1]<3*MS && p[6]==1);

    /* explicit frames take precedence: frame 3 -> 4 with a pending run across
     * the explicit boundary, then a run left open by an explicit close. */
    ps3dk_prof_frame(); /* frame 3; flip frame 2 closes OPEN */
    clear();
    poll(1);
    ps3dk_prof_frame(); /* frame 4 */
    clear();
    poll(0);
    present();
    assert(p[2]==0 && p[5]==1);
    poll(1);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);
    poll(0); /* no active frame: nothing to charge */
    assert(anchors==1); /* frame 0 */
#endif
    complete(&context,0);
    puts("AUTO presentation evidence PASS");
}
