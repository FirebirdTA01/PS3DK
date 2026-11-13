/* CALLs emitted inside an open draw bracket (from the writer's own callback) on the real runtime with synthetic
 * device writes (CONFIG 63/11). The pending draw is not disturbed and nothing is committed for the CALL, but the
 * CALL executed its build: a first CALL of a build with report writes sets the sticky SEGMENT_REPLAY status (its
 * writes may land on pairs no record of the call owns), its visits count in the calling row, and a later CALL of
 * the same build is a replay. A replay inside a bracket counts the same way. No RSX ordering is proven here. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
static uint64_t reports[2048][2];
static uint32_t labels[256],words[4096],seg[3][256],flushes;
enum { A, B, C };
static gpp_ctx context,keys[3],port;
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
void ps3dk_prof__auto_test_segment_history(const void *key,uint32_t out[4]);
void ps3dk_prof__auto_test_unrepresented(uint32_t out[3]);

static int clear(gpp_ctx *c)
{
    ps3tc_auto_call call={3,5,0,3,0,0,0,0xf3u};
    uint32_t *before=c->current;
    uint32_t ticket=ps3dk_prof__auto_begin((void *)c,&call,0x1234);
    for(unsigned n=0;n<6;n++) *c->current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)c,ticket);
    if(c!=&context) assert(c->current==before+(ticket ? 10:6));
    return ticket!=0;
}
static void part(unsigned k)
{
    ps3dk_prof__auto_segment_external_open((void *)&keys[k],seg[k],sizeof seg[k]);
    port=(gpp_ctx){seg[k],seg[k]+255,seg[k],NULL};
}
static void close_part(unsigned k) {ps3dk_prof__auto_segment_external_close((void *)&keys[k]);}
static void call(unsigned k,int released)
{
    uint32_t *before=context.current;
    for(unsigned n=0;n<6;n++) *context.current++=0xca110000u+n;
    ps3dk_prof__auto_segment_call((void *)&context,(void *)&keys[k]);
    assert(context.current==before+(released ? 10:6));
}
static void history(unsigned k,uint32_t v,uint32_t r,uint32_t called,uint32_t written)
{
    uint32_t h[4];
    ps3dk_prof__auto_test_segment_history(&keys[k],h);
    if(h[0]!=v || h[1]!=r || h[2]!=called || h[3]!=written)
        fprintf(stderr,"part %u history %u %u %u %u\n",k,h[0],h[1],h[2],h[3]);
    assert(h[0]==v && h[1]==r && h[2]==called && h[3]==written);
}
static uint32_t unrepresented(uint32_t *replay)
{
    uint32_t u[3];
    ps3dk_prof__auto_test_unrepresented(u);
    if(replay) *replay=u[2];
    return u[0];
}
/* A timed clear in part C with CALL(k) emitted between its begin and end (as the writer's callback would). The
 * draw itself stays timed: its end report lands where it would without the CALL. */
static void call_inside_draw(unsigned k)
{
    ps3tc_auto_call cc={3,5,0,3,0,0,0,0xf3u};
    uint32_t *before=port.current;
    uint32_t t=ps3dk_prof__auto_begin((void *)&port,&cc,0x1234);
    assert(t && port.current==before+2);
    for(unsigned n=0;n<3;n++) *port.current++=0xab000000u+n;
    call(k,0);
    for(unsigned n=3;n<6;n++) *port.current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)&port,t);
    assert(port.current==before+10 && "the pending draw is not disturbed");
}
int main(void) {
    uint32_t replay;
    context=(gpp_ctx){words,words+4096,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    ps3dk_prof_frame(); /* frame 0 */
    part(A);assert(clear(&port));close_part(A);
    part(B);assert(clear(&port));close_part(B);
    history(A,1,0,0,1);
    assert(unrepresented(&replay)==0 && !replay);
    /* first CALL of A inside a draw of C */
    part(C);
    call_inside_draw(A);
    (void)unrepresented(&replay);
    assert(replay==1 && "a CALL inside a draw bracket sets the replay latch");
    assert(unrepresented(NULL)==1 && "a CALL inside a draw bracket counts the build's visits");
    history(A,1,0,1,1);
    /* a later ordinary CALL of A is a replay: no records, its visit counted again */
    call(A,0);
    assert(unrepresented(NULL)==2);
    /* B called normally, then replayed inside a draw of C */
    call(B,1);
    assert(unrepresented(NULL)==2);
    call_inside_draw(B);
    assert(unrepresented(NULL)==3 && "a replay inside a draw bracket counts the build's visits");
    close_part(C);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);
    uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    for(unsigned r=1040;r<1080;r++) st64(&reports[r][0],1000+10*r);
    st32(&labels[247],frontier);
    puts("AUTO segment CALL inside a draw bracket PASS");
}
