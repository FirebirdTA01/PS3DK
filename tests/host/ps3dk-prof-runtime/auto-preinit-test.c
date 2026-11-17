/* AUTO options set before cellGcmInit on the real runtime with synthetic device writes. The intercepted init uses
 * them in place of the defaults, so commands written right after init do not close the window for them; the override
 * after a write still refuses (PRIOR_WRITES_UNKNOWN). The driver checks the capture's CONFIG reserves.
 *
 * TEST_PREINIT_MODE 0 (preinit): invalid options refused (NULL, bad size, a range reserve that leaves fewer than 64
 *   pairs, a ring field in a build without the ring), the 36-byte struct accepted (an exact allocation), a valid set replaced by a later one, the caller's struct changed after the call without effect; the init
 *   uses range_pairs 100; a set after init is EBUSY; a raw write then the override is refused and changes nothing; 100
 *   ranges in frame 0 admitted, the 101st refused.
 * TEST_PREINIT_MODE 1 (preinitthread): options set on another thread; the init on this thread takes the defaults.
 * TEST_PREINIT_MODE 2 (preinitold): the earlier 32-byte struct (an exact 32-byte allocation: nothing past it is read)
 *   with 512 reports: the init uses them, with the default range reserve of that pool (31).
 * TEST_PREINIT_MODE 3 (preinitfail): a failed init leaves the options for the next, successful one. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
#ifndef TEST_PREINIT_MODE
#define TEST_PREINIT_MODE 0
#endif
static uint64_t reports[2048][2];
static uint32_t labels[256],words[16384];
static gpp_ctx context;
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static unsigned flushes;
static void flush(gpp_ctx *ctx) {
    assert(ctx==&context);
    ++flushes;
    uint32_t op=ps3dk_prof__auto_publish_before((void *)ctx);
    ps3dk_prof__auto_publish_after((void *)ctx,op);
    st64(&reports[1024+(flushes-1)%16][0],10000*flushes);st32(&labels[246],2*flushes);
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_reserves(uint32_t out[2]);
static uint32_t reserves[2];
static void init(int32_t rc) {ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,rc);}

#if TEST_PREINIT_MODE==1
static int thread_rc=-1;
static void *setter(void *unused)
{
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;
    (void)unused;
    o.range_pairs=100;
    thread_rc=ps3dk_prof_auto_options_set(&o);
    return NULL;
}
#endif

int main(void) {
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;
    context=(gpp_ctx){words,words+16384,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
#if TEST_PREINIT_MODE==0
    assert(ps3dk_prof_auto_options_set(NULL)==PS3DK_PROF_EINVAL);
    o.size=20;assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_EINVAL);o.size=sizeof o;
    o.range_pairs=504-32-64+1;assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_EINVAL);
    /* ring fields are validated as the attach validates them: reserved (refused) in this build */
    o.range_pairs=0;o.ring_blocks=2;assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_EINVAL);o.ring_blocks=0;
    {
        void *v36=malloc(36);                                                   /* the 36-byte struct: nothing past it is read */
        assert(v36);
        o.size=36;o.range_pairs=150;
        memcpy(v36,&o,36);
        assert(ps3dk_prof_auto_options_set((const ps3dk_prof_auto_options *)v36)==PS3DK_PROF_OK);
        free(v36);
        o.size=sizeof o;
    }
    o.range_pairs=200;assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_OK);
    o.range_pairs=100;assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_OK);   /* replaces the earlier set */
    o.range_pairs=7;                                                            /* the copy was taken at the call */
    init(0);
    ps3dk_prof__auto_test_reserves(reserves);
    assert(reserves[0]==32 && reserves[1]==100);
    assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_EBUSY);                 /* after the intercepted init */
    {
        /* a raw command right after init: the override is refused as before and changes nothing */
        uint32_t instance=9,domain=9;
        *context.current++=0;
        o.range_pairs=50;
        assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==PS3DK_PROF_EINVAL && !instance);
        ps3dk_prof__auto_test_reserves(reserves);
        assert(reserves[1]==100);
    }
    ps3dk_prof_frame(); /* frame 0 */
    {
        unsigned ok=0;
        for(unsigned i=0;i<101;i++) {
            if(ps3dk_prof_auto_range_begin_at("pre-init range",__FILE__,1u,0)) ok++;
            (void)ps3dk_prof_auto_range_end();
        }
        assert(ok==100);
    }
#elif TEST_PREINIT_MODE==1
    {
        pthread_t t;
        assert(pthread_create(&t,NULL,setter,NULL)==0 && pthread_join(t,NULL)==0);
        assert(thread_rc==PS3DK_PROF_OK);
    }
    init(0);
    ps3dk_prof__auto_test_reserves(reserves);
    assert(reserves[0]==32 && reserves[1]==63);                                /* another thread's options: defaults */
    assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_EBUSY);
    ps3dk_prof_frame();
#elif TEST_PREINIT_MODE==2
    {
        void *old=malloc(32);                                                   /* exactly 32 bytes: ASan sees any read past the old struct */
        assert(old);
        o.size=32;o.report_first=1024;o.report_count=512;
        memcpy(old,&o,32);
        assert(ps3dk_prof_auto_options_set((const ps3dk_prof_auto_options *)old)==PS3DK_PROF_OK);
        free(old);
    }
    init(0);
    ps3dk_prof__auto_test_reserves(reserves);
    assert(reserves[0]==32 && reserves[1]==31);                                /* 248 pairs: 248/8 */
    ps3dk_prof_frame();
#else
    o.range_pairs=100;
    assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_OK);
    init(-1);                                                                   /* the application's init failed */
    init(0);
    ps3dk_prof__auto_test_reserves(reserves);
    assert(reserves[0]==32 && reserves[1]==100);
    ps3dk_prof_frame();
#endif
    {
        uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
        ps3dk_prof__auto_publish_after((void *)&context,frontier);
        for(unsigned n=1040;n<2048;n++) st64(&reports[n][0],1000+10*n);
        st32(&labels[247],frontier);
    }
    puts("AUTO pre-init options PASS");
}
