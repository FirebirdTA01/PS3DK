/* Externally written CALL segments (cellGcmSegmentBeginExternal/EndExternal) on the real runtime with
 * synthetic device writes. The application emits through ONE context of its own, reused for every part, as a
 * port writer does: each BeginExternal part is its own segment, identified by the segment's embedded context
 * (what cellGcmSegmentCall passes), and the writer is bound by where its cursor lies. A covered emitter there
 * gets two report-only words around it; without that room it is untimed and never overflows; after
 * EndExternal, or outside every open part, it is a foreign visit. No RSX ordering is proven here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
static uint64_t reports[2048][2];
static uint32_t labels[256],words[1024],seg_a[256],seg_b[256],flushes;
static gpp_ctx context,key_a,key_b,port;
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

/* One covered emitter through c (a three-vertex array 10 words, a three-index indexed draw 11, a clear 6). */
static uint32_t draw(gpp_ctx *c,uint32_t kind,int timed)
{
    ps3tc_auto_call call={kind,5,0,3,0,0,0,kind==3 ? 0xf3u:0u};
    unsigned w=kind==1 ? 10:kind==2 ? 11:6;
    uint32_t *before=c->current;
    uint32_t ticket=ps3dk_prof__auto_begin((void *)c,&call,0x1234);
    uint32_t *app=c->current;
    for(unsigned n=0;n<w;n++) *c->current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)c,ticket);
    if(timed) {
        assert(ticket && app==before+2 && c->current==before+4+w);
        assert(before[0]==0x41800 && (before[1]>>24)==1 && before[2+w]==0x41800 && before[3+w]==before[1]+16);
    } else assert(!ticket && app==before && c->current==before+w);
    return ticket;
}
static void call(gpp_ctx *key)
{
    uint32_t *before=context.current;
    for(unsigned n=0;n<6;n++) *context.current++=0xca110000u+n;
    ps3dk_prof__auto_segment_call((void *)&context,(void *)key);
    assert(context.current==before+10 && before[6]==0x41d6c && before[7]==247*16 && before[8]==0x41d70);
}
int main(void) {
    uint64_t counts[4];
    assert(PS3DK_PROF_AUTO_SEGMENT_DRAW_WORDS==4u);   /* the published room per covered draw */
    context=(gpp_ctx){words,words+1024,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    ps3dk_prof_frame(); /* frame 0: clock anchor, coverage opens, FRAME begin mark */
    assert(context.current==words+18);
    /* part 1 in seg_a */
    ps3dk_prof__auto_segment_external_open((void *)&key_a,seg_a,sizeof seg_a);
    port=(gpp_ctx){seg_a,seg_a+255,seg_a,NULL};
    draw(&port,1,1);draw(&port,2,1);
    ps3dk_prof__auto_segment_external_close((void *)&key_a);
    call(&key_a);
    /* part 2 in seg_b through the SAME writer context */
    ps3dk_prof__auto_segment_external_open((void *)&key_b,seg_b,sizeof seg_b);
    port=(gpp_ctx){seg_b,seg_b+255,seg_b,NULL};
    draw(&port,3,1);
    /* room for the clear's 6 words plus 3: not the 4 report words, so untimed and no overflow */
    uint32_t *end=port.end;port.end=port.current+6+3;
    draw(&port,3,0);
    port.end=end;
    ps3dk_prof__auto_segment_external_close((void *)&key_b);
    call(&key_b);
    /* after EndExternal: the cursor is still in seg_b, but no part is open there */
    draw(&port,2,0);
    /* seg_a reopened while the writer's cursor is in seg_b: not that part's writer */
    ps3dk_prof__auto_segment_external_open((void *)&key_a,seg_a,sizeof seg_a);
    draw(&port,1,0);
    ps3dk_prof__auto_segment_external_close((void *)&key_a);
    ps3dk_prof__auto_test_segment_counts(counts);
    /* segment visits: 3 timed + 1 without room; foreign: 2; table full: 0; segments: 2 */
    assert(counts[0]==4 && counts[1]==2 && !counts[2] && counts[3]==2);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);
    uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    for(unsigned r=1040;r<1060;r++) st64(&reports[r][0],1000+10*r);
    st32(&labels[247],frontier);
    puts("AUTO external segment timing PASS");
}
