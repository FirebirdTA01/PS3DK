/* Actual owner/frame/drainer path with explicit synthetic device writes. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
static uint64_t reports[2048][2];
static uint32_t labels[256],words[8192],flushes;
static gpp_ctx context;
#ifndef TEST_CLOCK_FRAMES
#define TEST_CLOCK_FRAMES 64
#endif
#define TEST_CLOCK_ANCHORS (TEST_CLOCK_FRAMES/8+TEST_CLOCK_FRAMES/64)
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
/* Sixteen physical clock reports, reused in order once each earlier record
 * was accepted; the serial keeps counting. Every frame also carries a begin and
 * an end FRAME mark (twelve words each); the flush completes them too. */
static void flush(gpp_ctx *ctx) {
    uint32_t serial=++flushes,slot=1024+(serial-1)%16;
    assert(ctx==&context && serial<=TEST_CLOCK_ANCHORS);
    uint32_t *w=ctx->current-6;
    assert(w[0]==0x41800 && w[1]==0x1000000u+slot*16);
    assert(w[2]==0x41d6c && w[3]==238*16 && w[4]==0x41d70);
    assert(w[5]==(((serial&0xff00ff00u)|((serial>>16)&0xffu)|((serial&0xffu)<<16))));
    assert(ld64(&reports[slot][0])==UINT64_MAX);
    uint32_t op=ps3dk_prof__auto_publish_before((void *)ctx);
    ps3dk_prof__auto_publish_after((void *)ctx,op); /* supplied original PUT seam */
    st64(&reports[slot][0],10000*serial);st32(&labels[238],serial);
    for(unsigned r=1040;r<2048;r++) st64(&reports[r][0],100000+r); /* FRAME mark reports */
    st32(&labels[239],op);
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
uint64_t ps3dk_prof__auto_test_clock_backlog(void);
int main(void) {
    context=(gpp_ctx){words,words+8192,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    for(unsigned frame=0;frame<TEST_CLOCK_FRAMES;frame++) {
        ps3dk_prof_frame();
        ps3dk_prof_token token=ps3dk_prof_frame_token();assert(token.owner && token.frame==frame);
        assert(ps3dk_prof_auto_frame_end(1,token)==0);
        /* The independent drainer must accept each anchor before its slot can
         * return; a bounded wait models a sink that keeps up. */
        unsigned polls=0;
        while(ps3dk_prof__auto_test_clock_backlog()) {
            const struct timespec pause={0,500000};
            assert(++polls<=20000);nanosleep(&pause,NULL);
        }
    }
    assert(flushes==TEST_CLOCK_ANCHORS && context.current==words+6*flushes+24*TEST_CLOCK_FRAMES);
    puts("AUTO actual bounded scheduled clock capture PASS");
}
