/* Segment build frames and CALL completeness on the real runtime with synthetic device writes (CONFIG 63/11).
 * Every segment draw carries the coverage frame that built it; a CALL that executes draws without committed
 * records under its token counts them in the calling row (COVERAGE u32@380); a second CALL of a build that
 * wrote reports sets the sticky SEGMENT_REPLAY status. The driver then checks the capture's records, rows and
 * status. No RSX ordering is proven here.
 *
 * frame 0: a ring clear; part A (one timed clear); part S (state only); part X (one clear without room: refused);
 *          a clear that begins in part C and ends in part D (an overflow callback switches parts mid-emitter);
 *          part E's first clear.
 * frame 1: part E's second clear (the build spans the boundary); CALL A, S, X, D, an untracked part U, E;
 *          a second CALL of S (state only: no replay).
 * frame 2: a second CALL of A (replay); part R built and CALLed through a context other than the ring (refused);
 *          part P with saturated counters CALLed (the row counter saturates). */
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
static uint32_t labels[256],words[4096],seg[9][256],other_words[256],flushes;
enum { A, S, X, C, D, E, R, P, U };
static gpp_ctx context,keys[9],port,other_ring;
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
void ps3dk_prof__auto_test_segment_preset(const void *key,uint32_t visits,uint32_t refused);
void ps3dk_prof__auto_test_unrepresented(uint32_t out[3]);

/* One clear (6 words) through c; returns whether it was timed (in a segment, 4 report words around it; the ring
 * path brackets with its own label-carrying markers). */
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
/* The application's CALL and retirement label, then the hook: four more words when it releases the label. */
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
static uint32_t unrepresented(uint32_t *flags,uint32_t *replay)
{
    uint32_t u[3];
    ps3dk_prof__auto_test_unrepresented(u);
    if(flags) *flags=u[1];
    if(replay) *replay=u[2];
    return u[0];
}
int main(void) {
    uint32_t flags,replay;
    context=(gpp_ctx){words,words+4096,words,NULL};
    other_ring=(gpp_ctx){other_words,other_words+256,other_words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);

    ps3dk_prof_frame(); /* frame 0 */
    assert(clear(&context));
    part(A);assert(clear(&port));close_part(A);
    part(S);close_part(S);
    part(X);
    {
        uint32_t *end=port.end;port.end=port.current+6+3; /* room for the clear, not the 4 report words */
        assert(!clear(&port));
        port.end=end;
    }
    close_part(X);
    /* a clear that begins in C; the overflow callback closes C and opens D; it ends in D, quarantined */
    part(C);
    {
        ps3tc_auto_call cc={3,5,0,3,0,0,0,0xf3u};
        uint32_t t=ps3dk_prof__auto_begin((void *)&port,&cc,0x1234);
        assert(t && port.current==seg[C]+2);
        for(unsigned n=0;n<3;n++) *port.current++=0xab000000u+n;
        close_part(C);part(D);
        for(unsigned n=3;n<6;n++) *port.current++=0xab000000u+n;
        ps3dk_prof__auto_end((void *)&port,t);
        assert(port.current==seg[D]+3);
    }
    close_part(D);
    history(A,1,0,0,1);history(S,0,0,0,0);history(X,1,1,0,0);
    history(C,1,1,0,1);
    {
        uint32_t h[4];
        ps3dk_prof__auto_test_segment_history(&keys[D],h);
        assert(h[0]==1 && h[1]==1 && "the part a moved draw ends in counts it");
    }
    part(E);assert(clear(&port));
    assert(unrepresented(NULL,NULL)==0);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);

    ps3dk_prof_frame(); /* frame 1 */
    assert(clear(&port));close_part(E);     /* E's build spans frames 0 and 1 */
    call(A,1);assert(unrepresented(NULL,NULL)==0);
    call(S,0);assert(unrepresented(NULL,NULL)==0);
    call(X,0);assert(unrepresented(NULL,NULL)==1);  /* its one draw was refused while building */
    call(D,0);assert(unrepresented(NULL,NULL)==2);  /* the moved draw is not in D's pool */
    call(U,0);assert(unrepresented(NULL,NULL)==3);  /* untracked: at least one */
    call(E,1);assert(unrepresented(NULL,NULL)==3);
    call(S,0);assert(unrepresented(NULL,&replay)==3 && !replay); /* a state-only re-CALL replays no report */
    history(A,1,0,1,1);history(S,0,0,1,0);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);

    ps3dk_prof_frame(); /* frame 2 */
    call(A,0);
    (void)unrepresented(NULL,&replay);
    assert(replay==1 && "re-CALL of a timed build sets the replay latch");
    assert(unrepresented(NULL,NULL)==1 && "a re-CALL counts the build's visits");
    part(R);assert(clear(&port));close_part(R);
    {
        uint32_t *before=other_ring.current;
        for(unsigned n=0;n<6;n++) *other_ring.current++=0xca110000u+n;
        ps3dk_prof__auto_segment_call((void *)&other_ring,(void *)&keys[R]); /* not the ring: refused, dropped */
        assert(other_ring.current==before+6);
    }
    assert(unrepresented(NULL,NULL)==2);
    history(R,1,0,1,1);
    /* saturation: part P's counters, then the row counter */
    part(P);
    ps3dk_prof__auto_test_segment_preset(&keys[P],UINT32_MAX,UINT32_MAX-1u);
    {
        uint32_t *end=port.end;port.end=port.current+6+3;
        assert(!clear(&port));
        port.end=end;
    }
    history(P,UINT32_MAX,UINT32_MAX,0,0);
    close_part(P);
    call(P,0);
    assert(unrepresented(&flags,NULL)==UINT32_MAX && (flags&4));
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);

    uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    for(unsigned r=1040;r<1080;r++) st64(&reports[r][0],1000+10*r);
    st32(&labels[247],frontier);
    puts("AUTO segment build frames and CALL completeness PASS");
}
