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
/* The application's flip calls go through the cellGcm entry points (and their
 * gcm* aliases) as an application build sees them; the firmware stubs are
 * fakes in the test bridge. */
#include <cell/gcm/ps3tc_gcm_flip.h>
#define gcmSetFlip cellGcmSetFlip
#define gcmSetPrepareFlip cellGcmSetPrepareFlip
#define gcmGetFlipStatus cellGcmGetFlipStatus
extern int auto_test_fw_flip_rc,auto_test_fw_flips,auto_test_fw_prepares,auto_test_fw_polls,auto_test_fw_pending;
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
    if(clock_serial) {st64(&reports[1024+(clock_serial-1)%16][0],10000*clock_serial);st32(&labels[246],clock_serial);}
    st32(&labels[247],op);
}
static void flush(gpp_ctx *ctx) {assert(ctx==&context);complete(ctx,++anchors);}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_frames(uint64_t out[3]);
void ps3dk_prof__auto_test_present(uint64_t out[8]);
/* The application's flip-status loop: pending twice, then done, back to
 * back, through either spelling: one complete polling-cadence run, which is
 * never a flip-wait scope. */
static void wait_flip(unsigned spelling)
{
    int polls=auto_test_fw_polls;
    auto_test_fw_pending=2;
    if(spelling&1) while(cellGcmGetFlipStatus()!=0) {}
    else while(gcmGetFlipStatus()!=0) {}
    assert(auto_test_fw_polls==polls+3);
}
static void clear(void)
{
    ps3tc_auto_call call={3,0,0,0,0,0,0,0xf3};
    uint32_t ticket=ps3dk_prof__auto_begin((void *)&context,&call,0x1234);
    assert(ticket);
    for(unsigned n=0;n<6;n++) *context.current++=0xc1ea0000u+n;
    ps3dk_prof__auto_end((void *)&context,ticket);
}
/* One flip through one of the four spellings; the firmware result decides acceptance. */
static void flip_as(unsigned spelling,int32_t ok)
{
    int calls=auto_test_fw_flips+auto_test_fw_prepares;
    auto_test_fw_flip_rc=ok ? 0:-1;
    switch(spelling%4) {
    case 0: assert((gcmSetFlip((void *)&context,1)==0)==(ok!=0));break;
    case 1: assert((cellGcmSetFlip((void *)&context,1)==0)==(ok!=0));break;
    case 2: assert(((int32_t)gcmSetPrepareFlip((void *)&context,1)>=0)==(ok!=0));break;
    default: assert(((int32_t)cellGcmSetPrepareFlip((void *)&context,1)>=0)==(ok!=0));break;
    }
    assert(auto_test_fw_flips+auto_test_fw_prepares==calls+1); /* one firmware call per flip */
}
static void flip(int32_t ok) {static unsigned n;flip_as(n++,ok);}
int main(void) {
    uint64_t counts[3];
    context=(gpp_ctx){words,words+4096,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    clear(); /* before any flip: unassociated */
    for(unsigned i=0;i<20;i++) {
        uint64_t present[8];
        flip(1);
        ps3dk_prof__auto_test_frames(counts);
        assert(counts[0]==i+1); /* each accepted flip opens exactly one frame, whichever spelling */
        wait_flip(i);
        ps3dk_prof__auto_test_present(present);
        assert(present[0]==0 && present[1]==0); /* polls are not a flip-wait scope */
        assert(present[2]==1 && present[3]>0); /* one cadence run in the frame the flip opened */
        clear();
    }
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
