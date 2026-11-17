/* Report-ring attach (PS3DK_PROF_AUTO_RING, host only): option bounds before any
 * allocation, the MAIN page allocation and IO mapping with their failure seams,
 * retained pages, the default report partition, the wide metadata queue, the
 * third reserved label and its startup reset/readback. Fake device memory; no
 * GPU execution or ordering premise. */
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

const auto_capture *ps3dk_prof__auto_test_capture(void);
const auto_wire_config *ps3dk_prof__auto_test_config(void);
uint64_t ps3dk_prof__auto_test_ring_retained(void);
void ps3dk_prof__auto_test_reserves(uint32_t out[2]);
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_fifo_boundary(void *ctx);

static uint64_t reports[2048][2];
static uint32_t labels[256],words[256],other_words[64],third_words[64];
static gpp_ctx context,other,third;
static int drop_ring_label;
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static void flush(gpp_ctx *c) {(void)c;}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v)
{
    if(drop_ring_label && a==&labels[245]) return; /* a label write that does not land */
    __atomic_store_n(a,v,__ATOMIC_SEQ_CST);
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};

/* the native FIFO wrap stand-in: the identity the real libgcm_cmd checks */
static int32_t native(gpp_ctx *c,uint32_t n) {(void)c;(void)n;return -1;}
int ps3tc_fifo_wrap_native(const struct _gcmCtxData *c) {return c && ((const gpp_ctx *)(const void *)c)->callback==native;}

static unsigned allocations,maps,fail_alloc,fail_map,misalign;
static void *pages[16];
static uint32_t mapped_bytes;
static int ring_allocate(uint32_t bytes,uintptr_t *ea)
{
    void *p;
    allocations++;
    if(fail_alloc) return -1;
    assert(!(bytes%(1u<<20)));
    p=aligned_alloc(1u<<20,bytes+(misalign ? (1u<<20):0)); /* never freed: process lifetime, like LV2 */
    assert(p && allocations<=16);
    pages[allocations-1]=p;  /* still reachable (LSan): the runtime deliberately forgets retained pages */
    memset(p,0xc3,bytes);
    *ea=(uintptr_t)p+(misalign ? 4096u:0u);
    return 0;
}
static int ring_map(uintptr_t ea,uint32_t bytes,uint32_t *offset)
{
    maps++;
    assert(!(ea&((1u<<20)-1u)));
    if(fail_map) return -1;
    mapped_bytes=bytes;*offset=0x0c000000u;
    return 0;
}
static const auto_ring_memory memory={ring_allocate,ring_map};

static ps3dk_prof_auto_options ring_options(void)
{
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;
    o.ring_blocks=2;
    return o;
}

static void refused(const ps3dk_prof_auto_options *o,int rc,unsigned allocs,unsigned map_calls)
{
    uint32_t instance=7,domain=7,ticket=g_prof->auto_lease.ticket;
    const auto_capture *before=ps3dk_prof__auto_test_capture();
    assert(ps3dk_prof_auto_attach((void *)&context,o,&instance,&domain)==rc);
    assert(!instance && !domain && g_prof->auto_lease.ticket==ticket);
    assert(ps3dk_prof__auto_test_capture()==before);
    assert(allocations==allocs && maps==map_calls);
}

int main(void)
{
    ps3dk_prof_auto_options o;
    uint32_t instance,domain,manual=0,reserves[2];
    const auto_capture *a;
    memset(labels,0x5a,sizeof labels);
    context=(gpp_ctx){words,words+256,words,native};
    other=(gpp_ctx){other_words,other_words+64,other_words,native};
    third=(gpp_ctx){third_words,third_words+64,third_words,native};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof_test_hooks.auto_ring_memory=&memory;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
    assert(g_prof->auto_lease.ctx==&context && !g_prof->auto_lease.ring_label);
    assert(!ps3dk_prof__auto_test_capture()->ring_plan.blocks);

    /* option bounds: refused before any allocation or map */
    o=ring_options();o.ring_blocks=4;refused(&o,PS3DK_PROF_EINVAL,0,0);     /* no legacy pairs left */
    o=ring_options();o.ring_blocks=1;refused(&o,PS3DK_PROF_EINVAL,0,0);
    o=ring_options();o.ring_label=247;refused(&o,PS3DK_PROF_EINVAL,0,0);    /* the operation label */
    o=ring_options();o.ring_label=246;refused(&o,PS3DK_PROF_EINVAL,0,0);    /* the clock label */
    o=ring_options();o.ring_label=65;refused(&o,PS3DK_PROF_EINVAL,0,0);     /* PSGL */
    o=ring_options();o.ring_bytes=(1u<<20)+(1u<<19);refused(&o,PS3DK_PROF_EINVAL,0,0);
    o=ring_options();o.ring_bytes=257u<<20;refused(&o,PS3DK_PROF_EINVAL,0,0);
    o=ring_options();o.ring_records=65537;refused(&o,PS3DK_PROF_EINVAL,0,0);
    o=ring_options();o.report_first=1023;o.report_count=1024;refused(&o,PS3DK_PROF_EINVAL,0,0);
    o=ring_options();o.size=36;                                             /* ring fields beyond 36 bytes */
    assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0);
    assert(!ps3dk_prof__auto_test_capture()->ring_plan.blocks && !g_prof->auto_lease.ring_label);
    assert(allocations==0 && maps==0);

    /* allocation failure, misaligned pages (kept), map failure (kept): refused before the lease, which keeps the
     * current capture's reservation */
    o=ring_options();
    fail_alloc=1;refused(&o,PS3DK_PROF_ENOMEM,1,0);fail_alloc=0;
    assert(ps3dk_prof__auto_test_ring_retained()==0);
    misalign=1;refused(&o,PS3DK_PROF_ENOMEM,2,0);misalign=0;
    assert(ps3dk_prof__auto_test_ring_retained()==20u<<20);
    fail_map=1;refused(&o,PS3DK_PROF_ENOMEM,3,1);fail_map=0;
    assert(ps3dk_prof__auto_test_ring_retained()==40u<<20);

    /* a live manual domain on label 244 refuses that ring label; the pages it mapped stay reusable */
    assert(ps3dk_prof_gpu_attach((void *)&other,0,16,244,&manual)==0 && manual);
    o=ring_options();o.ring_label=244;refused(&o,PS3DK_PROF_EINVAL,4,2);
    assert(ps3dk_prof__auto_test_ring_retained()==40u<<20);

    /* the defaults: 16 MiB, 16,384 records, label 245, reports 1024..1039 clocks, 1040..1543 legacy, 1544..2047 ring */
    o=ring_options();
    assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0 && instance==1 && domain);
    assert(allocations==4 && maps==2 && mapped_bytes==16u<<20);    /* the pages mapped above */
    a=ps3dk_prof__auto_test_capture();
    assert(a->ring_plan.blocks==2 && a->ring_plan.pairs==126 && a->ring_plan.first==1544);
    assert(a->ring_plan.legacy_pairs==252 && a->ring_plan.slots==4096 && a->ring_plan.total==20u<<20);
    assert(!(a->ring_ea&((1u<<20)-1u)) && a->ring_offset==0x0c000000u && a->ring_label==245);
    assert(a->ring.capacity==4096 && a->ring.blocks==2 && a->ring.pairs==126 && !a->ring.invalid);
    assert((uintptr_t)a->ring.slots==a->ring_ea+a->ring_plan.slots_at);
    assert(a->operations.capacity==16384 && a->operations.pairs==252);
    assert((uintptr_t)a->operations.entries==a->ring_ea+(16u<<20));
    ps3dk_prof__auto_test_reserves(reserves);
    assert(reserves[0]==32 && reserves[1]==16);
    assert(a->ring_port.ready && a->ring_port.label==245 && a->ring_port.report_first==1544 &&
           a->ring_port.report_count==504);
    assert(g_prof->auto_lease.ring_label==245 && g_prof->auto_lease.first==1024 && g_prof->auto_lease.count==1024);
    /* the control block stays inside max_bytes; metadata is in the page allocation */
    assert(a->allocated<=2097152u && a->allocated<(uint64_t)sizeof(auto_capture)+256u*sizeof(auto_wire_frame)+512u);
    printf("AUTO ring attach footprint: control %llu + pages %u (ring %u, metadata %u, descriptors %u) bytes\n",
           (unsigned long long)a->allocated,a->ring_plan.total,a->ring_plan.bytes,
           a->ring_plan.records*a->ring_plan.metadata_stride,a->ring_plan.slots*a->ring_plan.slot_stride);
    /* manual attaches cannot take the ring label */
    assert(ps3dk_prof_gpu_attach((void *)&third,32,16,245,&manual)==PS3DK_PROF_EINVAL);

    /* an identical override reuses the mapped pages; another size maps new ones and keeps the old */
    {
        uintptr_t ea=a->ring_ea;                                /* the override frees this capture's control block */
        assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0);
        assert(allocations==4 && maps==2 && ps3dk_prof__auto_test_capture()->ring_ea==ea);
    }
    o.ring_bytes=1u<<20;o.ring_records=64;o.ring_blocks=3;
    assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0);
    assert(allocations==5 && maps==3 && mapped_bytes==1u<<20);
    assert(ps3dk_prof__auto_test_ring_retained()==60u<<20);
    a=ps3dk_prof__auto_test_capture();
    assert(a->ring_plan.first==1292 && a->ring_plan.legacy_pairs==126 && a->operations.capacity==64);
    assert(a->operations.pairs==126 && a->ring.capacity==256 && a->ring.blocks==3);

    /* CONFIG exposes both budgets after the owner's first observation freezes it */
    ps3dk_prof__auto_fifo_boundary(&context);
    {
        const auto_wire_config *c=ps3dk_prof__auto_test_config();
        assert(!c->reason && c->pairs==504 && c->metadata==64 && c->legacy_pairs==126);
        assert(c->ring_bytes==1u<<20 && c->ring_blocks==3 && c->ring_pairs==126 && c->ring_first==1292);
        assert(c->ring_label==245 && c->ring_stride==4096 && c->ring_slots==256);
        assert(c->metadata_stride==sizeof(auto_queue_entry) && c->call_pairs==31 && c->range_pairs==15);
    }

    /* the first device write resets the ring label; one that does not read back zero stops recycling: owner draws
     * are then untimed with the explicit unknown-ring-state reason (no markers, no legacy fallback) */
    {
        ps3tc_auto_call clear={3,0,0,0,0,0,0,0xf3};
        uint32_t ticket;
        drop_ring_label=1;
        ticket=ps3dk_prof__auto_begin((void *)&context,&clear,0);
        assert(!ticket && labels[247]==0 && labels[246]==0 && labels[245]==0x5a5a5a5au);
        a=ps3dk_prof__auto_test_capture();
        assert(a->ring.invalid && a->ring_refusal==AUTO_RING_REFUSED_LABEL);
        context.current+=6;
        ps3dk_prof__auto_end((void *)&context,ticket);
        assert(context.current==words+6);
    }

    /* an application write to the ring label is a resource conflict */
    ps3dk_prof__auto_resource_write((void *)&context,2,244);
    assert(a->conflicts==0);
    ps3dk_prof__auto_resource_write((void *)&context,2,245);
    assert(a->conflicts==1);
    puts("AUTO ring attach, partition, budgets and label reservation PASS");
    ps3dk_prof_thread_end();
    return 0;
}
