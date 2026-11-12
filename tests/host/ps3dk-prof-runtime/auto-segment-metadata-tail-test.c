/* Metadata exhaustion at cellGcmSegmentCall latches the frame tail (review 2026-11-12). With one metadata record,
 * the FRAME begin mark holds it, so a CALL cannot commit its build and drops it without a completion release;
 * every later covered draw of that frame, in a segment or in the ring, is then untimed and counted. The next
 * frame clears the latch. */
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
uint32_t ps3dk_prof__auto_test_pool_tail(void);

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
    context=(gpp_ctx){words,words+4096,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;uint32_t instance,domain;
    o.metadata_records=1;
    assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0);
    ps3dk_prof_frame(); /* the FRAME begin mark holds the one metadata record; it stays incomplete */
    part(0);assert(clear());
    ps3dk_prof__auto_segment_external_close((void *)&keys[0]);
    uint32_t *before=context.current;call(0);
    assert(context.current==before+6);     /* no completion release: the build was dropped */
    assert(ps3dk_prof__auto_test_pool_tail()==4);
    part(1);assert(!clear());              /* a later segment draw of the frame: untimed */
    ps3dk_prof__auto_segment_external_close((void *)&keys[1]);
    {   /* a later ring draw of the frame: untimed too */
        ps3tc_auto_call cl={3,5,0,3,0,0,0,0xf3u};
        uint32_t *b=context.current;
        uint32_t t=ps3dk_prof__auto_begin((void *)&context,&cl,0x1234);
        for(unsigned n=0;n<6;n++) *context.current++=0xab000000u+n;
        ps3dk_prof__auto_end((void *)&context,t);
        assert(!t && context.current==b+6);
    }
    assert(ps3dk_prof__auto_test_pool_tail()==4);
    ps3dk_prof_frame();                   /* the next frame clears the latch */
    assert(ps3dk_prof__auto_test_pool_tail()==0);
    puts("AUTO metadata exhaustion latches the frame tail PASS");
    return 0;
}