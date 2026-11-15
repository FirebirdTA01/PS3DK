/* Tags, fragment-program offsets, AUTO ranges and timed-range limits on the real runtime with synthetic device writes
 * (CONFIG 1023/11). Nothing completes before the end (no label is written), so held pairs stay held; the driver then
 * checks the capture's records and rows.
 *
 * frame 0: tag 0xa ring clear; fragment program 0x1000 on the ring, tag 0xb ring clear; part 0 built under tag 0xa
 *          with its own program 0x2000 (one timed clear, one refused for room), CALLed under tag 0xc (the CALL
 *          record carries 0xc, its draw keeps 0xa); a range "outer" with a nested "inner" around two clears; five
 *          nested begins (the fifth refused); a range begun while a part is open (refused); an END without a range
 *          (refused); a range open across the frame boundary (refused at its END in frame 1).
 * frame 1: the range reserve: 16 ranges, the 17th refused; then timing limited to timed ranges: a ring clear and a
 *          segment clear outside every timed range are untimed, a clear inside a timed range (and inside a plain
 *          range nested in it) is timed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
static uint64_t reports[2048][2];
static uint32_t labels[256],words[16384],seg[2][1024],flushes;
static gpp_ctx context,keys[2],port;
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
void ps3dk_prof__auto_test_ranges(uint64_t out[4]);
static ps3dk_prof_desc outer={"outer","",__FILE__,1u,2u,0u,0u},inner={"inner","",__FILE__,2u,2u,0u,0u},
                       pass={"pass","",__FILE__,3u,2u,0u,0u};

/* One clear (6 words) through c; returns whether it was timed. */
static int clear(gpp_ctx *c)
{
    ps3tc_auto_call call={3,5,0,3,0,0,0,0xf3u};
    uint32_t ticket=ps3dk_prof__auto_begin((void *)c,&call,0x1234);
    for(unsigned n=0;n<6;n++) *c->current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)c,ticket);
    return ticket!=0;
}
static void part(unsigned k)
{
    ps3dk_prof__auto_segment_external_open((void *)&keys[k],seg[k],sizeof seg[k]);
    port=(gpp_ctx){seg[k],seg[k]+1023,seg[k],NULL};
}
static void close_part(unsigned k) {ps3dk_prof__auto_segment_external_close((void *)&keys[k]);}
static void call(unsigned k)
{
    ps3dk_prof__auto_segment_call_begin((void *)&context,(void *)&keys[k]);
    for(unsigned n=0;n<6;n++) *context.current++=0xca110000u+n;
    ps3dk_prof__auto_segment_call((void *)&context,(void *)&keys[k]);
}
static uint64_t r[4];
static void ranges(void) {ps3dk_prof__auto_test_ranges(r);}

int main(void) {
    context=(gpp_ctx){words,words+16384,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);

    ps3dk_prof_frame(); /* frame 0 */
    PS3DK_PROF_AUTO_TAG(0xa);assert(clear(&context));   /* the public macro */
    ps3dk_prof__auto_fragment_program((void *)&context,0x1000);
    ps3dk_prof_auto_tag(0xb);assert(clear(&context));
    ranges();assert(r[3]==0xb);
    /* part 0: built under 0xa with its own program, called under 0xc */
    ps3dk_prof_auto_tag(0xa);
    part(0);
    assert(clear(&port));                                 /* binds the part's writer */
    ps3dk_prof__auto_fragment_program((void *)&port,0x2000);
    assert(clear(&port));
    {
        uint32_t *end=port.end;port.end=port.current+6+3;   /* room for the clear, not its reports */
        assert(!clear(&port));
        port.end=end;
    }
    /* a range begun while a part is open is refused */
    assert(!ps3dk_prof_auto_range_begin(&outer,0));assert(!ps3dk_prof_auto_range_end());
    ranges();assert(r[0]==0 && r[1]==1 && r[2]==0);
    close_part(0);
    ps3dk_prof_auto_tag(0xc);call(0);
    /* nested ranges */
    ps3dk_prof_auto_tag(0xd);
    {
        uint32_t *w0=context.current;
        assert(ps3dk_prof_auto_range_begin(&outer,0));
        assert(context.current==w0+2 && w0[0]==((1u<<18)|0x1800u));   /* BEGIN: one timestamp report */
    }
    assert(clear(&context));
    assert(ps3dk_prof_auto_range_begin(&inner,0));
    ranges();assert(r[2]==2);
    assert(clear(&context));
    {
        /* END: the end report (2 words) and the release (4) in the ring */
        uint32_t *w0=context.current;
        assert(ps3dk_prof_auto_range_end());
        assert(context.current==w0+6 && w0[0]==((1u<<18)|0x1800u) && w0[2]==((1u<<18)|0x1d6cu));
    }
    assert(ps3dk_prof_auto_range_end());
    ranges();assert(r[0]==2 && r[1]==1 && r[2]==0);
    /* depth 5 is refused; END stays balanced */
    for(unsigned i=0;i<5;i++) assert(ps3dk_prof_auto_range_begin(&pass,0)==(i<4));
    for(unsigned i=0;i<5;i++) (void)ps3dk_prof_auto_range_end();
    ranges();assert(r[0]==6 && r[1]==2 && r[2]==0);
    /* an END without a range */
    assert(!ps3dk_prof_auto_range_end());
    ranges();assert(r[1]==3);
    /* a range across the frame boundary is refused at its END */
    assert(ps3dk_prof_auto_range_begin(&outer,0));
    ps3dk_prof_frame(); /* frame 1 */
    assert(!ps3dk_prof_auto_range_end());
    ranges();assert(r[0]==6 && r[1]==4);

    /* the range reserve: 16 pairs, minus the 6 committed and the quarantined one above (none completes) */
    {
        unsigned ok=0;
        for(unsigned i=0;i<16;i++) {
            if(ps3dk_prof_auto_range_begin(&pass,0)) ok++;
            (void)ps3dk_prof_auto_range_end();
        }
        assert(ok==16u-7u);
        ranges();assert(r[0]==6+ok && r[1]==4+(16-ok));
    }
    ps3dk_prof_auto_tag(0xe);
    /* timing limited to timed ranges: the reserve is spent, so the timed range's own record is refused, but its
     * draws are still timed */
    PS3DK_PROF_AUTO_TIMED_RANGES_ONLY();
    assert(!clear(&context));                             /* outside: untimed */
    part(1);assert(!clear(&port));close_part(1);          /* a segment draw outside: untimed */
    PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED("timed");
    assert(clear(&context));
    PS3DK_PROF_AUTO_RANGE_BEGIN("inner");
    assert(clear(&context));                              /* inside a plain range inside a timed one */
    PS3DK_PROF_AUTO_RANGE_END();PS3DK_PROF_AUTO_RANGE_END();
    assert(!clear(&context));                             /* outside again */
    ps3dk_prof_auto_timing(0);
    assert(clear(&context));                              /* every draw timed again */
    call(1);
    assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);

    uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    for(unsigned n=1040;n<2048;n++) st64(&reports[n][0],1000+10*n);
    st32(&labels[247],frontier);
    printf("AUTO tags, fragment programs, ranges and timed ranges PASS\n");
}
