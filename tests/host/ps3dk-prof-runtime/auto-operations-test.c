/* Real runtime and device port, host atomic device memory. No GPU premise is
 * established by these programmed report/label values. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"

static uint64_t reports[2048][2];
static uint32_t labels[256],words[256];
static gpp_ctx context;
/* With frames, the frame-0 begin FRAME mark takes the first report pair. */
#if TEST_FRAMES
#define OP_REPORT 82
#define FRONTIER 6
#else
#define OP_REPORT 80
#define FRONTIER 4
#endif
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static void flush(gpp_ctx *c);
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void flush(gpp_ctx *c) {
#if TEST_FRAMES
    assert(c==&context && c->current==words+6);
    assert(c->current[-5]==0x1000000u+64*16 && c->current[-3]==246*16);
    uint32_t frontier=ps3dk_prof__auto_publish_before((void *)c);
    ps3dk_prof__auto_publish_after((void *)c,frontier);
    st64(&reports[64][0],10000);st32(&labels[246],1);
#else
    (void)c;assert(0 && "observer must not flush inside operation hooks");
#endif
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);

int main(void) {
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;
    ps3tc_auto_call clear={3,0,0,0,0,0,0,0xf3};
    uint32_t instance,domain,ticket;
    struct _gcmCtxData *ctx=(void *)&context;
    memset(labels,0x5a,sizeof labels);
    context=(gpp_ctx){words,words+256,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    ps3dk_prof__auto_resource_write(ctx,1,1); /* outside the initial lease */
    o.report_first=64;o.report_count=20;o.metadata_records=4;o.frame_records=2;
#if TEST_FRAMES
    o.frame_records=8;o.report_count=22; /* three pairs: the begin mark and the two operations */
#endif
    assert(ps3dk_prof_auto_attach((void *)ctx,&o,&instance,&domain)==0);
#if TEST_FRAMES
    const uint32_t attached_instance=instance;
    ps3dk_prof_frame();
    ps3dk_prof_token first=ps3dk_prof_frame_token();
    assert(first.owner && first.frame==0);
    assert(context.current==words+18); /* the boundary's clock (6 words), then the begin FRAME mark (12) */
#endif
    uint32_t *application_begin=context.current;
    clear.clear_mask=0x80000000;
    assert(!ps3dk_prof__auto_begin(ctx,&clear,123) && context.current==application_begin);
    assert(ps3dk_prof_auto_attach((void *)ctx,&o,&instance,&domain)==PS3DK_PROF_ESTOPPED);
    assert(!instance && !domain); /* failed attach clears outputs, not the live instance */
    clear.clear_mask=0xf3;context.current=words+250;
    assert(!ps3dk_prof__auto_begin(ctx,&clear,123) && context.current==words+250);
#if TEST_FRAMES
    assert(ld32(&labels[247])==0); /* the preceding owner-boundary clock initialized it */
#else
    assert(ld32(&labels[247])==0x5a5a5a5a); /* exclusions never initialize device state */
#endif
    context.current=application_begin;
    for(unsigned op=0;op<2;op++) {
        uint32_t *before=context.current;
        ticket=ps3dk_prof__auto_begin(ctx,&clear,123+op);
        assert(ticket && context.current==before+6 && g_tls.in_gpu);
#if TEST_FRAMES
        if(!op) assert(!ps3dk_prof__auto_begin(ctx,&clear,124));
#endif
        assert(ld32(&labels[247])==0 && ld32(&labels[246])==
#if TEST_FRAMES
               1
#else
               0
#endif
        );
        assert(ld64(&reports[OP_REPORT+2*op][0])==UINT64_MAX);
        for(unsigned n=0;n<6;n++) *context.current++=0xcafe0000+n;
        ps3dk_prof__auto_end(ctx,ticket);
        assert(context.current==before+18 && !g_tls.in_gpu);
        assert(before[0]==0x41800 && before[1]==(0x1000000u+(OP_REPORT+2*op)*16));
        assert(before[12]==0x41800 && before[13]==(0x1000000u+(OP_REPORT+1+2*op)*16));
    }
    assert(g_prof->auto_lease.published);
#if TEST_FRAMES
    /* Both report pairs remain owned: there is no published completion yet. */
    assert(!ps3dk_prof__auto_begin(ctx,&clear,125));
    ps3dk_prof_token foreign=first;foreign.owner++;
    assert(ps3dk_prof_auto_frame_end(attached_instance,foreign)!=0);
    foreign=first;foreign.reserved=1;
    assert(ps3dk_prof_auto_frame_end(attached_instance,foreign)!=0);
    assert(ps3dk_prof_auto_frame_end(attached_instance,first)==0);
    assert(ps3dk_prof_auto_frame_end(attached_instance,first)!=0);
    clear.clear_mask=0x80000000;
    assert(!ps3dk_prof__auto_begin(ctx,&clear,126)); /* after closure: unassociated */
    ps3dk_prof_frame(); /* frame 1 has no calls but is not explicitly closed */
    ps3dk_prof_frame(); /* frame 2 implies OPEN for frame 1 */
    assert(!ps3dk_prof__auto_begin(ctx,&clear,127));
    assert(ps3dk_prof_auto_frame_end(attached_instance,ps3dk_prof_frame_token())==0);
    clear.clear_mask=0xf3;
#endif
    assert(ps3dk_prof__auto_publish_before(ctx)==FRONTIER);
    ps3dk_prof__auto_publish_after(ctx,FRONTIER);
#if TEST_INVALIDATE == 1
    gpp_ctx foreign={words,words+256,words,NULL};
    ps3dk_prof__auto_resource_write((void *)&foreign,1,80);
    assert(!ps3dk_prof__auto_begin(ctx,&clear,126));
#elif TEST_INVALIDATE == 2
    ps3dk_prof__auto_context_reset(ctx);
    assert(!ps3dk_prof__auto_begin(ctx,&clear,126));
#endif
#if TEST_FRAMES
    st64(&reports[80][0],50);st64(&reports[81][0],60); /* the begin mark */
#endif
    st64(&reports[OP_REPORT][0],100);st64(&reports[OP_REPORT+1][0],110);
    st64(&reports[OP_REPORT+2][0],200);st64(&reports[OP_REPORT+3][0],220);
    st32(&labels[247],FRONTIER); /* device facts are still supplied by this host fixture */
    assert(context.current==application_begin+36);
    puts("AUTO actual marker producer and cumulative completion PASS");
    return 0;
}
