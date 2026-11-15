/* Label-poll cadence on the real runtime, with a controlled timebase and synthetic device writes. The application
 * polls through the SDK's non-blocking label queries (<cell/gcm/ps3tc_gcm_labels.h> in a PS3_PROF_AUTO unit), so
 * the observed seam is the header's own code. "work" only advances the clock.
 *
 * Rules under test (COVERAGE label-poll block at 384, CONFIG 511/11):
 *  - a run is consecutive not-yet-passed polls of one label with every gap within 1 ms, closed by a passed poll;
 *  - a larger gap, a poll of another label or a flip-wait scope ends the run as broken;
 *  - polls inside a flip-wait scope are not label cadence;
 *  - a run across a frame boundary is charged to neither frame;
 *  - the row names the complete runs' label + 1, or 0xffffffff for several;
 *  - the label area has the real 16-byte stride: an address outside it or off a label slot has no identity, ends an
 *    open run as broken and forms no run of its own;
 *  - cadence is kept per label: u64@408 is the largest one-label total, never a sum across labels. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
/* the GCM label area: label i at base + 16 i (four words per label slot) */
static uint32_t app_labels[256*4], outside;
static uint32_t *cellGcmGetLabelAddress(uint8_t index) {return &app_labels[index*4u];}
#define LABEL(i) app_labels[(i)*4u]
#define PS3TC_GCM_LABELS_NO_GCM 1
#include <cell/gcm/ps3tc_gcm_labels.h>
#include <ps3dk/prof.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
#ifndef PS3TC_GCM_LABEL_POLL_OBSERVED
#error "the label queries are not observed in a PS3_PROF_AUTO unit"
#endif
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
void ps3dk_prof__auto_test_label(uint64_t out[6]);

static uint64_t clock_base,clock_reads,clock_advance;
static uint64_t fake_clock(void)
{
    return clock_base+__atomic_add_fetch(&clock_reads,1,__ATOMIC_SEQ_CST)+
           __atomic_load_n(&clock_advance,__ATOMIC_SEQ_CST);
}
static void elapse(uint64_t ns) {__atomic_add_fetch(&clock_advance,ns,__ATOMIC_SEQ_CST);}
#define MS UINT64_C(1000000)

static void flip(void) {ps3dk_prof__auto_flip((void *)&context,1);}
static void clear(void)
{
    ps3tc_auto_call call={3,0,0,0,0,0,0,0xf3};
    uint32_t ticket=ps3dk_prof__auto_begin((void *)&context,&call,0x1234);
    assert(ticket);
    for(unsigned n=0;n<6;n++) *context.current++=0xc1ea0000u+n;
    ps3dk_prof__auto_end((void *)&context,ticket);
}
/* one query of label i for target 5: pending while the label is below it */
static void poll(uint8_t i,int passed) {LABEL(i)=passed ? 5:4;assert(cellGcmIsLabelPassed(i,5)==passed);}
/* one query through the address form */
static void at(uint32_t *w,int passed) {*w=passed ? 5:4;assert(cellGcmIsLabelValuePassed(w,5)==passed);}
static uint64_t l[6];
static void row(void) {ps3dk_prof__auto_test_label(l);}

int main(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    clock_base=(uint64_t)ts.tv_sec*1000000000u+(uint64_t)ts.tv_nsec+1000000000u;
    ps3dk_prof_test_hooks.clock=fake_clock;
    context=(gpp_ctx){words,words+4096,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);

    /* frame 0 */
    flip();clear();
    for(unsigned i=0;i<40;i++) {poll(70,0);elapse(MS/2);}
    poll(70,1);
    row();
    assert(l[0]==1 && l[1]==0 && l[3]==71 && l[4]>=20*MS && l[4]<21*MS && l[5]==l[4]);
    /* 10 ms of work between two polls: broken */
    poll(70,0);elapse(10*MS);poll(70,1);
    row();
    assert(l[0]==1 && l[1]==1);
    /* a poll of another label ends the run as broken; label 80's own run completes: the row names several */
    poll(70,0);poll(80,0);poll(80,1);
    row();
    assert(l[0]==2 && l[1]==2 && l[3]==0xffffffffu);
    /* two labels: the one-label total is label 70's run alone, never the sum with label 80's */
    assert(l[5]>=20*MS && l[5]<21*MS && l[5]<l[4]);

    /* frame 1: the address form names its label (16-byte stride); polls inside a flip-wait scope are not label
     * cadence */
    flip();clear();
    at(&LABEL(90),0);at(&LABEL(90),1);
    row();
    assert(l[0]==1 && l[3]==91 && l[5]==l[4]);
    {
        uint64_t scope=ps3dk_prof__auto_flip_wait_begin();
        assert(scope);
        poll(70,0);elapse(MS/2);poll(70,1);
        ps3dk_prof__auto_flip_wait_end(scope,0);
    }
    row();
    assert(l[0]==1 && l[1]==0);
    /* the review case: a pending pointer query of label 90, then a passed one of label 91, is a label switch */
    at(&LABEL(90),0);at(&LABEL(91),1);
    row();
    assert(l[0]==1 && l[1]==1 && l[3]==91);
    /* an address outside the label area, or inside it off a label slot, has no identity: no run */
    at(&outside,0);at(&outside,1);
    at(&LABEL(90)+1,0);at(&LABEL(90)+1,1);
    row();
    assert(l[0]==1 && l[1]==1 && l[3]==91);
    /* an unidentified poll ends an open run as broken */
    at(&LABEL(90),0);at(&outside,0);at(&LABEL(90),1);
    row();
    assert(l[0]==1 && l[1]==2 && l[3]==91);

    /* frame 1 -> 2: a pending run across the boundary is charged to neither frame */
    poll(70,0);
    flip();clear();
    poll(70,1);
    row();
    assert(l[0]==0 && l[2]==1 && l[3]==0);
    complete(&context,0);
    puts("AUTO label-poll cadence PASS");
}
