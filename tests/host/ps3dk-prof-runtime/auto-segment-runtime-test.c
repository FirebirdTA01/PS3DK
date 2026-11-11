/* CALL segment timing (D1) and frame marks (D2) on the real runtime with
 * synthetic device writes. A covered emitter inside an open segment gets two
 * report-only words around it; cellGcmSegmentCall's hook appends one label
 * release in the ring that completes the whole build. A rebuilt, never-called
 * segment commits INCOMPLETE records and quarantines its pairs. A private
 * buffer stays an unsupported-form visit. No RSX ordering is proven here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
static uint64_t reports[2048][2];
static uint32_t labels[256],words[1024],seg_a[256],seg_b[256],private_words[64],flushes;
static gpp_ctx context,a_ctx,b_ctx,other;
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
/* Only the frame-0 clock anchor flushes; operation hooks never do. */
static void flush(gpp_ctx *ctx) {
    assert(ctx==&context && ++flushes==1);
    uint32_t op=ps3dk_prof__auto_publish_before((void *)ctx);
    ps3dk_prof__auto_publish_after((void *)ctx,op);
    st64(&reports[1024][0],10000);st32(&labels[246],1);
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_segment_counts(uint64_t out[4]);

/* One covered emitter: begin hook, the emitter's exact word count (a three-
 * vertex array 10, a three-index 32-bit indexed draw 11, a clear 6), end hook. */
static uint32_t draw(gpp_ctx *c,uint32_t kind)
{
    ps3tc_auto_call call={kind,5,0,3,0,0,0,kind==3 ? 0xf3u:0u};
    unsigned w=kind==1 ? 10:kind==2 ? 11:6;
    uint32_t *before=c->current;
    uint32_t ticket=ps3dk_prof__auto_begin((void *)c,&call,0x1234);
    uint32_t *app=c->current;
    for(unsigned n=0;n<w;n++) *c->current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)c,ticket);
    if(c==&a_ctx || c==&b_ctx) {
        /* QUERY_GET + timestamp report, the application words, the end report */
        assert(ticket && app==before+2 && c->current==before+4+w);
        assert(before[0]==0x41800 && (before[1]>>24)==1 && before[2+w]==0x41800 && before[3+w]==before[1]+16);
    }
    if(c==&other) assert(!ticket && app==before && c->current==before+w);
    return ticket;
}
/* The application's CALL and retirement label, then the observer's hook. */
static void call(gpp_ctx *seg)
{
    uint32_t *before=context.current;
    for(unsigned n=0;n<6;n++) *context.current++=0xca110000u+n;
    ps3dk_prof__auto_segment_call((void *)&context,(void *)seg);
    assert(context.current==before+10 && before[6]==0x41d6c && before[7]==247*16 && before[8]==0x41d70);
}
int main(void) {
    uint64_t counts[4];
    context=(gpp_ctx){words,words+1024,words,NULL};
    a_ctx=(gpp_ctx){seg_a,seg_a+255,seg_a,NULL};
    b_ctx=(gpp_ctx){seg_b,seg_b+255,seg_b,NULL};
    other=(gpp_ctx){private_words,private_words+64,private_words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    ps3dk_prof_frame(); /* frame 0: clock anchor (6 words), coverage opens, FRAME begin mark (12) */
    assert(context.current==words+18);
    assert(draw(&context,3)); /* the ring clear is observed */
    ps3dk_prof__auto_segment_open((void *)&a_ctx);
    assert(draw(&a_ctx,1) && draw(&a_ctx,2));
    ps3dk_prof__auto_segment_close((void *)&a_ctx);
    call(&a_ctx);
    ps3dk_prof__auto_segment_open((void *)&b_ctx);
    assert(draw(&b_ctx,3));
    ps3dk_prof__auto_segment_close((void *)&b_ctx);
    b_ctx.current=seg_b;
    ps3dk_prof__auto_segment_open((void *)&b_ctx); /* rebuilt without a call: dropped, quarantined */
    ps3dk_prof__auto_segment_close((void *)&b_ctx);
    assert(!draw(&other,2)); /* a private buffer: unsupported-form visit */
    ps3dk_prof__auto_test_segment_counts(counts);
    assert(counts[0]==3 && counts[1]==1 && !counts[2] && counts[3]==2);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0); /* FRAME end mark */
    ps3dk_prof_frame(); /* frame 1 */
    a_ctx.current=seg_a;
    ps3dk_prof__auto_segment_open((void *)&a_ctx);
    assert(draw(&a_ctx,1));
    ps3dk_prof__auto_segment_close((void *)&a_ctx);
    call(&a_ctx);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);
    /* Publish everything (the application's flush), then supply device facts:
     * every operation report, then the cumulative label at the last serial. */
    uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    for(unsigned r=1040;r<1060;r++) st64(&reports[r][0],1000+10*r);
    st32(&labels[247],frontier);
    assert(frontier==16);
    puts("AUTO segment timing and frame marks PASS");
}
