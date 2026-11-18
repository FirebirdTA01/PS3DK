/* Build-option report ring (CMake PS3_PROF_AUTO_RING, <ps3dk/prof_auto.h>) on the real runtime with synthetic device
 * writes: the executable's generated constant, read by the AUTO start-up through its weak reference. Fake MAIN pages
 * through the ring memory hook; no GPU execution premise.
 *
 * TEST_RINGOPT_MODE 0 (ringopt): the constant the CMake defaults generate (16 MiB, 16,384 records): the automatic
 *   attach selects the ring with them (20 MiB of pages, one allocation); an override with NULL options takes the same
 *   build defaults and reuses the pages.
 * 1 (ringopt2): 2 MiB and 4096 records.
 * 2 (ringoptset): pre-init options without ring fields win over the constant: no ring, no ring allocation.
 * 3 (ringoptsetring): pre-init ring options (3 blocks, 1 MiB, 64 records) win over the constant.
 * 4 (ringoptbad): 3 MiB + 1 byte: the start-up refuses it with a diagnostic and does not initialize the profiler;
 *   the intercepted init then allocates nothing.
 * 5 (ringoptoff, ringofftoday): no constant: the default attach, no ring allocation; the driver compares this
 *   capture's CONFIG with the same program built without the ring compiled in (ringofftoday). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "auto_internal.h"
#include "ps3tc_prof_auto.h"
#ifndef TEST_RINGOPT_MODE
#define TEST_RINGOPT_MODE 0
#endif

#if TEST_RINGOPT_MODE==0 || TEST_RINGOPT_MODE==2 || TEST_RINGOPT_MODE==3
PS3DK_PROF_DEFINE_AUTOMATIC_RING_OPTIONS(PS3DK_PROF_AUTO_RING_OPTIONS_INIT(16777216u, 16384u));
#elif TEST_RINGOPT_MODE==1
PS3DK_PROF_DEFINE_AUTOMATIC_RING_OPTIONS(PS3DK_PROF_AUTO_RING_OPTIONS_INIT(2097152u, 4096u));
#elif TEST_RINGOPT_MODE==4
PS3DK_PROF_DEFINE_AUTOMATIC_RING_OPTIONS(PS3DK_PROF_AUTO_RING_OPTIONS_INIT(3145729u, 16384u));
#endif

#ifdef PS3DK_PROF_AUTO_RING
const auto_capture *ps3dk_prof__auto_test_capture(void);
uint64_t ps3dk_prof__auto_test_ring_retained(void);
#endif
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_reserves(uint32_t out[2]);

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

/* the native FIFO wrap stand-in: the identity the real libgcm_cmd checks */
static int32_t native(gpp_ctx *c,uint32_t n) {(void)c;(void)n;return -1;}
int ps3tc_fifo_wrap_native(const struct _gcmCtxData *c) {return c && ((const gpp_ctx *)(const void *)c)->callback==native;}

static unsigned allocations,maps;
static uint32_t allocated_bytes,mapped_bytes;
static void *pages[4];
static int ring_allocate(uint32_t bytes,uintptr_t *ea)
{
    void *p;
    assert(allocations<4 && !(bytes%(1u<<20)));
    p=aligned_alloc(1u<<20,bytes); /* never freed: process lifetime, like LV2 */
    assert(p);
    pages[allocations++]=p;allocated_bytes+=bytes;
    memset(p,0xc3,bytes);
    *ea=(uintptr_t)p;
    return 0;
}
static int ring_map(uintptr_t ea,uint32_t bytes,uint32_t *offset)
{
    (void)ea;maps++;mapped_bytes=bytes;*offset=0x0c000000u;
    return 0;
}
static const auto_ring_memory memory={ring_allocate,ring_map};
static void init(int32_t rc) {ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,rc);}

#ifdef PS3DK_PROF_AUTO_RING
__attribute__((unused)) static void ring_is(uint32_t blocks,uint32_t bytes,uint32_t records,uint32_t total)
{
    const auto_capture *a=ps3dk_prof__auto_test_capture();
    assert(a && a->ring_plan.blocks==blocks && a->ring_plan.bytes==bytes && a->ring_plan.records==records);
    assert(a->ring_plan.total==total && a->ring_label==245 && a->operations.capacity==records);
    assert(g_prof->auto_lease.ring_label==245);
    printf("AUTO build-option ring: blocks %u, extents %u bytes, records %u, pages %u bytes\n",blocks,bytes,records,
           total);
}
#endif

int main(void) {
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;
    uint32_t reserves[2];
    (void)o;(void)reserves;
    context=(gpp_ctx){words,words+16384,words,native};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof_test_hooks.auto_ring_memory=&memory;
#if TEST_RINGOPT_MODE==0
    init(0);
    ring_is(2,16u<<20,16384,20u<<20);
    assert(allocations==1 && maps==1 && mapped_bytes==16u<<20);
    {
        uint32_t instance=0,domain=0;
        assert(ps3dk_prof_auto_attach((void *)&context,NULL,&instance,&domain)==0 && instance && domain);
        ring_is(2,16u<<20,16384,20u<<20);
        assert(allocations==1 && maps==1 && ps3dk_prof__auto_test_ring_retained()==0);
    }
    ps3dk_prof__auto_test_reserves(reserves);
    assert(reserves[0]==32 && reserves[1]==31);                 /* from the 252 legacy pairs */
#elif TEST_RINGOPT_MODE==1
    init(0);
    ring_is(2,2u<<20,4096,(uint32_t)ps3dk_prof__auto_test_capture()->ring_plan.total);
    assert(ps3dk_prof__auto_test_capture()->ring_plan.total<=4u<<20);
    assert(allocations==1 && maps==1 && mapped_bytes==2u<<20 && allocated_bytes==ps3dk_prof__auto_test_capture()->ring_plan.total);
#elif TEST_RINGOPT_MODE==2
    o.range_pairs=100;
    assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_OK);
    init(0);
    assert(!ps3dk_prof__auto_test_capture()->ring_plan.blocks && !g_prof->auto_lease.ring_label);
    assert(allocations==0 && maps==0);
    ps3dk_prof__auto_test_reserves(reserves);
    assert(reserves[0]==32 && reserves[1]==100);                /* the pre-init options, not the constant */
    puts("AUTO build-option ring: pre-init options without ring fields win, no ring allocation");
#elif TEST_RINGOPT_MODE==3
    o.ring_blocks=3;o.ring_bytes=1u<<20;o.ring_records=64;
    assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_OK);
    init(0);
    ring_is(3,1u<<20,64,(uint32_t)ps3dk_prof__auto_test_capture()->ring_plan.total);
    assert(allocations==1 && mapped_bytes==1u<<20);
#elif TEST_RINGOPT_MODE==4
    assert(!p_ready());                                          /* start-up refused: never initialized */
    assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_ENOTINIT);
    init(0);
    assert(allocations==0 && maps==0);
    puts("AUTO build-option ring: invalid constant refused at start-up, nothing allocated");
#else
    init(0);
    assert(allocations==0 && maps==0);
#ifdef PS3DK_PROF_AUTO_RING
    assert(!ps3dk_prof__auto_test_capture()->ring_plan.blocks && !g_prof->auto_lease.ring_label);
    assert(ps3dk_prof__auto_test_ring_retained()==0);
#endif
    ps3dk_prof__auto_test_reserves(reserves);
    assert(reserves[0]==32 && reserves[1]==63);
    ps3dk_prof_frame();
    {
        uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
        ps3dk_prof__auto_publish_after((void *)&context,frontier);
        for(unsigned n=1040;n<2048;n++) st64(&reports[n][0],1000+10*n);
        st32(&labels[247],frontier);
    }
    puts("AUTO build-option ring: no constant, the default attach, no ring allocation");
#endif
    puts("AUTO build-option ring PASS");
#if TEST_RINGOPT_MODE!=4
    ps3dk_prof_thread_end();
#endif
    return 0;
}
