/* Whole-CALL brackets on the real runtime with synthetic device writes (CONFIG 255/11). A build that is not fully
 * represented by its own records (a covered draw of it was refused) gets one report pair around its CALL: the
 * pre-CALL hook writes the begin report before the CALL, the CALL hook the end report after the retirement label,
 * then one CALL record (kind 5) completed by the same release. The pairs come from a reserve of 32 that covered
 * draws and FRAME marks never take. Nothing completes during the frame (no label is written), so no pair returns
 * before the end. The driver checks the capture's CALL records.
 *
 * frame 0: part 0, one timed clear: CALL without a bracket; part 1 draws until the pairs run out (every pair but
 *          the reserve), one more refused: CALL bracketed; part 2 refused, CALLed with no ring room: no bracket;
 *          parts 2..31 one refused clear each and part 0 rebuilt the same way: 31 more brackets (the reserve is
 *          spent); part 3 rebuilt: the 33rd is refused; part 1 CALLed again: no bracket, the replay latch. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
static uint64_t reports[2048][2];
static uint32_t labels[256],words[8192],seg[32][8192],flushes;
static gpp_ctx context,keys[32],port;
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
    st64(&reports[1024][0],10000);st32(&labels[246],2);
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_unrepresented(uint32_t out[3]);
void ps3dk_prof__auto_test_call_brackets(uint64_t out[2]);

/* One clear (6 words) through c; returns whether it was timed. */
static int clear(gpp_ctx *c)
{
    ps3tc_auto_call call={3,5,0,3,0,0,0,0xf3u};
    uint32_t *before=c->current;
    uint32_t ticket=ps3dk_prof__auto_begin((void *)c,&call,0x1234);
    for(unsigned n=0;n<6;n++) *c->current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)c,ticket);
    assert(c->current==before+(ticket ? 10:6));
    return ticket!=0;
}
static void part(unsigned k)
{
    ps3dk_prof__auto_segment_external_open((void *)&keys[k],seg[k],sizeof seg[k]);
    port=(gpp_ctx){seg[k],seg[k]+8191,seg[k],NULL};
}
static void close_part(unsigned k) {ps3dk_prof__auto_segment_external_close((void *)&keys[k]);}
/* cellGcmSegmentCall: the pre-CALL hook, the CALL and its label (6 words here), the CALL hook. `before` and
 * `after` are the words the observer must write before the CALL and after the label. */
static void call(unsigned k,unsigned before,unsigned after)
{
    uint32_t *a=context.current,*b;
    ps3dk_prof__auto_segment_call_begin((void *)&context,(void *)&keys[k]);
    assert(context.current==a+before);
    if(before) assert(a[0]==((1u<<18)|0x1800u) && a[1]>>24==1u && (a[1]&0xffffffu)/16u>=1040u);
    b=context.current;
    for(unsigned n=0;n<6;n++) *context.current++=0xca110000u+n;
    ps3dk_prof__auto_segment_call((void *)&context,(void *)&keys[k]);
    assert(context.current==b+6+after);
    if(before) {
        /* the end report is the begin report's pair partner, right after the label, then the release */
        assert(b[6]==((1u<<18)|0x1800u) && b[7]==a[1]+16u && b[8]==((1u<<18)|0x1d6cu));
    }
}
static void brackets(uint64_t begun,uint64_t refused)
{
    uint64_t v[2];
    ps3dk_prof__auto_test_call_brackets(v);
    if(v[0]!=begun || v[1]!=refused) fprintf(stderr,"brackets %llu refused %llu\n",(unsigned long long)v[0],(unsigned long long)v[1]);
    assert(v[0]==begun && v[1]==refused);
}
int main(void) {
    unsigned timed=0;
    uint32_t u[3];
    context=(gpp_ctx){words,words+8192,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    ps3dk_prof_frame(); /* frame 0: its begin mark holds one pair */

    /* fully represented: no bracket, the release only */
    part(0);assert(clear(&port));close_part(0);
    call(0,0,4);brackets(0,0);
    /* draws take every pair but the reserves: 504 - 32 (CALL) - 16 (ranges) - the mark - part 0's draw */
    part(1);
    while(clear(&port)) timed++;
    assert(timed==504u-32u-16u-2u);
    close_part(1);
    call(1,2,6);brackets(1,0);
    /* no ring room for the bracket: none begun, and the refused-only build releases nothing */
    part(2);assert(!clear(&port));close_part(2);
    {
        uint32_t *cur=context.current;
        context.current=context.end-12;
        ps3dk_prof__auto_segment_call_begin((void *)&context,(void *)&keys[2]);
        assert(context.current==context.end-12);
        ps3dk_prof__auto_segment_call((void *)&context,(void *)&keys[2]);
        assert(context.current==context.end-12);
        context.current=cur;
    }
    brackets(1,1);
    /* the reserve: 31 more brackets, each a build with one refused draw and no record of its own */
    for(unsigned k=2;k<32;k++) {part(k);assert(!clear(&port));close_part(k);call(k,2,6);}
    part(0);assert(!clear(&port));close_part(0);call(0,2,6);
    brackets(32,1);
    /* the 33rd: no pair left, no bracket */
    part(3);assert(!clear(&port));close_part(3);call(3,0,0);
    brackets(32,2);
    /* a second CALL of part 1 (it wrote reports): no bracket, the sticky replay latch */
    call(1,0,0);
    ps3dk_prof__auto_test_unrepresented(u);
    assert(u[2]==1);
    brackets(32,2);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);

    uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    for(unsigned r=1040;r<2048;r++) st64(&reports[r][0],1000+10*r);
    st32(&labels[247],frontier);
    printf("AUTO whole-CALL brackets: %u timed draws, 32 brackets from the reserve, the 33rd refused PASS\n",timed);
}
