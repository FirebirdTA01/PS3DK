/* Segment capacity on the real runtime with synthetic device writes: 24 distinct segments (a 2 x 12 part pool)
 * are all tracked, one build times 100 draws (the shared pool, not a per-segment table), and when the report
 * pairs run out every later covered draw of that frame is untimed, so the untimed draws are the frame's tail.
 * Nothing completes during the frame here (no label is written), so pairs are released only after it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
static uint64_t reports[2048][2];
static uint32_t labels[256],words[4096],seg[24][8192],flushes;
static gpp_ctx context,keys[24],port;
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void flush(gpp_ctx *ctx) {
    assert(ctx==&context && ++flushes==1);
    uint32_t op=ps3dk_prof__auto_publish_before((void *)ctx);
    ps3dk_prof__auto_publish_after((void *)ctx,op);
    st64(&reports[1024][0],10000);st32(&labels[246],1);
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_segment_counts(uint64_t out[4]);

/* One clear (6 words) through the writer; returns whether it was timed (4 report words around it). */
static int clear(void)
{
    ps3tc_auto_call call={3,5,0,3,0,0,0,0xf3u};
    uint32_t *before=port.current;
    uint32_t ticket=ps3dk_prof__auto_begin((void *)&port,&call,0x1234);
    for(unsigned n=0;n<6;n++) *port.current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)&port,ticket);
    assert(port.current==before+(ticket ? 10:6));
    return ticket!=0;
}
static void part(unsigned k)
{
    ps3dk_prof__auto_segment_external_open((void *)&keys[k],seg[k],sizeof seg[k]);
    port=(gpp_ctx){seg[k],seg[k]+8191,seg[k],NULL};
}
static void call(unsigned k)
{
    for(unsigned n=0;n<6;n++) *context.current++=0xca110000u+n;
    ps3dk_prof__auto_segment_call((void *)&context,(void *)&keys[k]);
}
int main(void) {
    uint64_t counts[4];
    unsigned timed=0,untimed=0,first_untimed=0,visits=0;
    context=(gpp_ctx){words,words+4096,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    ps3dk_prof_frame();
    /* 24 distinct segments, one draw each: all tracked and timed */
    for(unsigned k=0;k<24;k++) {
        part(k);
        assert(clear());
        ps3dk_prof__auto_segment_external_close((void *)&keys[k]);
        call(k);
    }
    ps3dk_prof__auto_test_segment_counts(counts);
    assert(counts[2]==0 && counts[3]==24);   /* no table-full refusal, 24 segments */
    /* one build with 100 draws: all timed */
    part(0);
    for(unsigned n=0;n<100;n++) assert(clear());
    /* keep drawing in the same build until the report pairs run out; from then on, every draw is untimed */
    for(unsigned n=0;n<600;n++) {
        int t=clear();visits++;
        if(t) {assert(!untimed);timed++;}
        else {if(!untimed) first_untimed=visits;untimed++;}
    }
    /* at most 504 pairs, 124 of them already held: a timed prefix, then a contiguous untimed tail */
    assert(timed>0 && timed+124<=504 && untimed>0 && untimed==600-timed && first_untimed==timed+1);
    ps3dk_prof__auto_segment_external_close((void *)&keys[0]);
    call(0);
    printf("AUTO segment capacity: 24 segments, 100 draws per build, %u timed then %u untimed PASS\n",timed,untimed);
}
