/* Covered emitters writing into a CALL segment context (or another private
 * buffer) on the owner thread: no markers, the command runs unchanged, and the
 * visit is counted, never silently absent. Synthetic device writes only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
static uint64_t reports[2048][2];
static uint32_t labels[256],words[512],segment_words[256],private_words[64],flushes;
static gpp_ctx context,segment,other;
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
/* Only the frame-0 clock anchor flushes; operation hooks never do. */
static void flush(gpp_ctx *ctx) {
    assert(ctx==&context && ++flushes==1 && ctx->current==words+6);
    uint32_t op=ps3dk_prof__auto_publish_before((void *)ctx);
    ps3dk_prof__auto_publish_after((void *)ctx,op);
    st64(&reports[1024][0],10000);st32(&labels[238],1);
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_segment_counts(uint64_t out[4]);

/* One covered emitter: begin hook, six application words, end hook. */
static uint32_t draw(gpp_ctx *c,uint32_t kind)
{
    ps3tc_auto_call call={kind,5,0,3,0,0,0,kind==3 ? 0xf3u:0u};
    uint32_t *before=c->current;
    uint32_t ticket=ps3dk_prof__auto_begin((void *)c,&call,0x1234);
    uint32_t *app=c->current;
    for(unsigned n=0;n<6;n++) *c->current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)c,ticket);
    if(c!=&context) assert(!ticket && app==before && c->current==before+6); /* unchanged, unmarked */
    return ticket;
}
int main(void) {
    uint64_t counts[4];
    context=(gpp_ctx){words,words+512,words,NULL};
    segment=(gpp_ctx){segment_words,segment_words+255,segment_words,NULL};
    other=(gpp_ctx){private_words,private_words+64,private_words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    ps3dk_prof_frame(); /* frame 0: clock anchor, then coverage opens */
    assert(draw(&context,3)); /* the ring clear is observed */
    ps3dk_prof__auto_segment_open((void *)&segment);
    ps3dk_prof__auto_segment_open((void *)&segment); /* idempotent */
    assert(!draw(&segment,1) && !draw(&segment,2) && !draw(&segment,3));
    ps3dk_prof__auto_segment_close((void *)&segment);
    assert(!draw(&segment,1)); /* after End: still not the ring, counted as foreign */
    assert(!draw(&other,2));   /* a private buffer that is no segment */
    ps3dk_prof__auto_test_segment_counts(counts);
    assert(counts[0]==3 && counts[1]==2 && !counts[2] && !counts[3]);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);
    assert(!draw(&other,1)); /* no open frame: unassociated */
    ps3dk_prof_frame(); /* frame 1: no foreign work */
    ps3dk_prof__auto_segment_open((void *)&segment);
    assert(!draw(&segment,2));
    ps3dk_prof__auto_segment_close((void *)&segment);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);
    ps3dk_prof__auto_test_segment_counts(counts);
    assert(counts[0]==4 && counts[1]==3 && !counts[3]);
    assert(flushes==1 && context.current==words+6+18);
    puts("AUTO segment and foreign context visits counted without markers PASS");
}
