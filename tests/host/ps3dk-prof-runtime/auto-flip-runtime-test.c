/* Automatic frames from flips on the real runtime, synthetic device writes.
 * The application never calls the profiler for frames until late: a flip
 * queued on the owner thread opens frame 0 and then closes frame N and opens
 * N+1. Work before the first flip is unassociated. A failed flip is no
 * boundary. The first explicit ps3dk_prof_frame takes precedence: it closes the
 * flip frame OPEN, and later flips are ignored and counted. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof.h>
#include <ps3dk/prof_auto.h>
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
/* The application's PUT: publish, then let the "device" complete everything
 * published so far (every report written, both labels at their frontiers). */
static void complete(gpp_ctx *ctx,uint32_t clock_serial) {
    uint32_t op=ps3dk_prof__auto_publish_before((void *)ctx);
    ps3dk_prof__auto_publish_after((void *)ctx,op);
    for(unsigned r=1024;r<2048;r++) st64(&reports[r][0],100000+r);
    if(clock_serial) {st64(&reports[1024+(clock_serial-1)%16][0],10000*clock_serial);st32(&labels[238],clock_serial);}
    st32(&labels[239],op);
}
static void flush(gpp_ctx *ctx) {assert(ctx==&context);complete(ctx,++anchors);}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_frames(uint64_t out[3]);
static void clear(void)
{
    ps3tc_auto_call call={3,0,0,0,0,0,0,0xf3};
    uint32_t ticket=ps3dk_prof__auto_begin((void *)&context,&call,0x1234);
    assert(ticket);
    for(unsigned n=0;n<6;n++) *context.current++=0xc1ea0000u+n;
    ps3dk_prof__auto_end((void *)&context,ticket);
}
static void flip(int32_t ok) {ps3dk_prof__auto_flip((void *)&context,ok);}
int main(void) {
    uint64_t counts[3];
    context=(gpp_ctx){words,words+4096,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    clear(); /* before any flip: unassociated */
    for(unsigned i=0;i<20;i++) {flip(1);clear();}
    flip(0); /* a refused flip is no boundary */
    ps3dk_prof__auto_test_frames(counts);
    assert(counts[0]==20 && counts[1]==0 && counts[2]==0);
    ps3dk_prof_frame(); /* explicit: frame 20, flip frame 19 closes OPEN */
    clear();
    flip(1); /* ignored from now on */
    ps3dk_prof_frame(); /* frame 21 */
    clear();
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);
    ps3dk_prof__auto_test_frames(counts);
    assert(counts[0]==20 && counts[1]==2 && counts[2]==1);
    assert(anchors==3); /* frames 0, 8 and 16 */
    complete(&context,0);
    puts("AUTO flip-driven frames PASS");
}
