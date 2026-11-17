/* Report-ring physical tail and native pre-wrap hook (PS3DK_PROF_AUTO_RING,
 * host only). The REAL ps3tc_fifo_wrap.c (host stubs for its LV2 headers) and
 * the real runtime run against a tiny in-order executor of the emitted words:
 * labels, GPU waits, timestamp reports, M2MF registers and copies, JUMPs. The
 * executor is no proof of asynchronous GPU ordering (hardware probes are). */
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
int ps3dk_prof__auto_test_ring_admit(void *context, uint32_t draws);
void ps3dk_prof__auto_test_ring_request(void);
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_fifo_boundary(void *ctx);
int32_t ps3tc_fifo_wrap_callback(struct _gcmCtxData *,uint32_t);

#define WORDS 512u
#define PHYS 508u          /* the context's physical end; the JUMP may land at it */
#define IO 0x40000u        /* the IO offset of fifo[0] */
#define MAIN_IO 0x0c000000u
#define M(sub,m,n) (((uint32_t)(n)<<18)|((uint32_t)(sub)<<13)|(m))

static uint64_t reports[2048][2];
static uint32_t labels[256],fifo[WORDS];
static gpp_ctx context;
static uint8_t *main_ring;
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static void flush(gpp_ctx *c) {(void)c;}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};

static int ring_allocate(uint32_t bytes,uintptr_t *ea)
{
    main_ring=aligned_alloc(1u<<20,bytes);assert(main_ring);
    memset(main_ring,0xee,bytes);*ea=(uintptr_t)main_ring;return 0;
}
static int ring_map(uintptr_t ea,uint32_t bytes,uint32_t *offset) {(void)ea;(void)bytes;*offset=MAIN_IO;return 0;}
static const auto_ring_memory memory={ring_allocate,ring_map};

/* ---- the LV2 surface of ps3tc_fifo_wrap.c, and the executor behind PUT/GET ---- */
typedef struct { volatile uint32_t put,get,ref; } control;
static control ctrl;
static uint32_t m2mf[0x400],sem,copies,app_transfers,app_in,app_out,app_src,app_dst;
static uint64_t tb;
void *gcmGetControlRegister(void) {return &ctrl;}
int32_t gcmAddressToOffset(void *p,uint32_t *off)
{
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)fifo;
    if(a<b || a>b+4u*WORDS || ((a-b)&3u)) return -1;
    *off=IO+(uint32_t)(a-b);return 0;
}
uint64_t sysGetTimebaseFrequency(void) {return 79800000u;}
uint64_t auto_test_timebase(void) {return ++tb;}
static uint32_t *at(uint32_t off) {assert(off>=IO && off<IO+4u*WORDS);return &fifo[(off-IO)/4u];}
static void apply(uint32_t sub,uint32_t m,uint32_t v)
{
    if(sub==0) {
        if(m==0x1d6c || m==0x64) sem=v/16u;
        else if(m==0x1d70) st32(&labels[sem],(v&0xff00ff00u)|((v>>16)&255u)|((v&255u)<<16));
        else if(m==0x68) assert(ld32(&labels[sem])==v); /* in order: the release already executed */
        else if(m==0x1800) st64(&reports[(v&0xffffffu)/16u][0],++tb);
        return;
    }
    assert(sub==1 && m<0x1000);
    m2mf[m/4u]=v;
    if(m!=0x328) return;
    if(m2mf[0x184/4]==0x66626660u) {
        /* the profiler's copy: LOCAL report bytes to the MAIN extent */
        uint32_t src=m2mf[0x30c/4],dst=m2mf[0x310/4],len=m2mf[0x31c/4];
        assert(m2mf[0x188/4]==0xfeed0001u && m2mf[0x320/4]==1 && m2mf[0x324/4]==0x101);
        assert(dst>=MAIN_IO && dst-MAIN_IO+len<=(1u<<20) && src+len<=sizeof reports);
        memcpy(main_ring+(dst-MAIN_IO),(uint8_t *)reports+src,len);copies++;
    } else {
        app_transfers++;app_in=m2mf[0x184/4];app_out=m2mf[0x188/4];app_src=m2mf[0x30c/4];app_dst=m2mf[0x310/4];
    }
}
static void gpu_run(void)
{
    while(ctrl.get!=ctrl.put) {
        uint32_t *w=at(ctrl.get),h=w[0],n=(h>>18)&2047u,m=h&0x1ffcu,sub=(h>>13)&7u;
        if(h&0x20000000u) {ctrl.get=h&~0x20000000u;continue;}
        for(uint32_t i=0;i<n;i++) apply(sub,m+((h&0x40000000u) ? 0:4u*i),w[1+i]);
        ctrl.get+=4u*(1u+n);
    }
}
int sys_timer_usleep(unsigned long us) {(void)us;gpu_run();return 0;}

/* ---- the application ---- */
static int32_t custom(gpp_ctx *c,uint32_t n) {(void)c;(void)n;return -1;}
static uint32_t *reserve(uint32_t n)
{
    uint32_t *w;
    if(context.current+n>context.end) assert(context.callback(&context,n)==0);
    w=context.current;context.current+=n;  /* re-read after the callback */
    return w;
}
static void app_state(void)
{
    uint32_t *w=reserve(10);
    w[0]=M(1,0x184,2);w[1]=0x11110000u;w[2]=0x22220000u;
    w[3]=M(1,0x30c,6);w[4]=0x100;w[5]=0x200;w[6]=64;w[7]=64;w[8]=64;w[9]=2;
    /* the format register (0x324) is left untouched: the copy establishes it and nothing may restore or claim it */
}
static void app_transfer(void) {uint32_t *w=reserve(2);w[0]=M(1,0x328,1);w[1]=0;}
static void app_flush(void)
{
    uint32_t off;
    ps3dk_prof__auto_preflush((void *)&context); /* FlushBuffer's hook, before its offset */
    assert(gcmAddressToOffset(context.current,&off)==0);
    ctrl.put=off;gpu_run();
}
static void nops(uint32_t n) {uint32_t *w=reserve(n);for(uint32_t i=0;i<n;i++) w[i]=0;}
static void fill_to_end(void) {while(context.current<context.end) nops(1);}
static void block_reports(uint32_t local,uint32_t pairs,uint64_t base)
{
    for(uint32_t i=0;i<2u*pairs;i++) {
        st64(&reports[1544u+local*252u+i][0],base+i);st64(&reports[1544u+local*252u+i][1],~(base+i));
    }
}
static int copied(uint32_t slot,uint32_t local,uint32_t pairs)
{
    return !memcmp(main_ring+slot*4096u,reports[1544u+local*252u],pairs*32u);
}
static void app_regs_restored(void)
{
    assert(m2mf[0x184/4]==0x11110000u && m2mf[0x188/4]==0x22220000u && m2mf[0x30c/4]==0x100 &&
           m2mf[0x310/4]==0x200 && m2mf[0x314/4]==64 && m2mf[0x318/4]==64 && m2mf[0x31c/4]==64 &&
           m2mf[0x320/4]==2 && m2mf[0x324/4]==0x101);
    /* the eight registers the application wrote; never the copy's own format */
    assert(ps3dk_prof__auto_test_capture()->shadow.known==0xffu);
}

int main(void)
{
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;
    uint32_t instance,domain,ticket;
    const auto_capture *a;
    ps3tc_auto_call clear={3,0,0,0,0,0,0,0xf3};
    uint32_t *visible;
    memset(labels,0x5a,sizeof labels);
    context=(gpp_ctx){fifo,fifo+PHYS,fifo,(int32_t (*)(gpp_ctx *,uint32_t))(void (*)(void))ps3tc_fifo_wrap_callback};
    ctrl.put=ctrl.get=IO;
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof_test_hooks.auto_ring_memory=&memory;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof fifo,sizeof fifo,fifo,0);
    assert(ps3tc_fifo_wrap_native((void *)&context));
    o.ring_blocks=2;o.ring_bytes=1u<<20;o.ring_records=64;
    assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0);
    a=ps3dk_prof__auto_test_capture();
    assert(a->ring_plan.first==1544 && a->ring_offset==MAIN_IO && a->scan==(uintptr_t)fifo);
    ps3dk_prof__auto_fifo_boundary(&context);

    /* block 1: three pairs, flushed by the native wrap from the reserved physical tail */
    app_state();app_flush();
    block_reports(0,3,1000);
    assert(ps3dk_prof__auto_test_ring_admit(&context,3));
    assert(labels[245]==0 && a->tail.active);
    assert(context.end==fifo+PHYS-AUTO_RING_FLUSH_WORDS);
    visible=context.end;
    fill_to_end();assert(context.current==visible);
    nops(4);                               /* the real callback wraps: prewrap, flush, JUMP after it */
    assert(context.current==fifo+4 && context.end==fifo+PHYS && !a->tail.active);
    assert(visible[0]==M(0,0x1d6c,1) && visible[1]==245u*16u);  /* the flush, not the JUMP, at the old current */
    assert(fifo[PHYS-2]==(0x20000000u|IO));      /* 28 + 16 restore words, inside the 46-word tail */
    assert(copies==1 && copied(0,0,3) && labels[245]==2);
    assert(a->ring.slots[0].closed==1 && !a->ring.active && a->ring_flushes==1 && a->ring_injected_words==44);
    app_regs_restored();
    assert(a->scan==(uintptr_t)fifo);            /* the decoder restarts at begin, skipping the JUMP */

    /* block 2: two pairs, requested at a boundary, flushed by FlushBuffer's hook */
    block_reports(1,2,2000);
    assert(ps3dk_prof__auto_test_ring_admit(&context,2));
    nops(1);
    ps3dk_prof__auto_test_ring_request();
    app_flush();
    assert(!a->tail.active && context.end==fifo+PHYS && copies==2 && copied(1,1,2) && labels[245]==4);
    app_regs_restored();

    /* block 3 reuses local block 0 after the copy that preceded it; flushed by a second wrap */
    block_reports(0,1,3000);
    assert(ps3dk_prof__auto_test_ring_admit(&context,1));
    fill_to_end();nops(4);
    assert(copies==3 && copied(2,0,1) && labels[245]==6 && a->ring_flushes==3);
    app_regs_restored();
    /* the application's own transfer runs with its state, not the copy's */
    app_transfer();app_flush();
    assert(app_transfers==1 && app_in==0x11110000u && app_out==0x22220000u && app_src==0x100 && app_dst==0x200);

    /* block 4: an owner draw beside the reservation is clean; any other end is not */
    assert(ps3dk_prof__auto_test_ring_admit(&context,1));
    visible=context.end;
    ticket=ps3dk_prof__auto_begin((void *)&context,&clear,0);
    assert(ticket);
    for(unsigned i=0;i<6;i++) context.current[i]=0;
    context.current+=6;
    ps3dk_prof__auto_end((void *)&context,ticket);
    assert(!a->operations.stopped);
    context.end=visible-4;
    assert(!ps3dk_prof__auto_begin((void *)&context,&clear,0) && a->operations.stopped);
    context.end=visible;
    /* a replaced callback would wrap without prewrap: the block is quarantined and the tail given back */
    context.callback=custom;
    assert(!ps3tc_fifo_wrap_native((void *)&context));
    app_flush();
    assert(context.end==fifo+PHYS && !a->tail.active && a->ring.invalid && a->ring_refusal==AUTO_RING_REFUSED_CALLBACK);
    assert(a->ring.slots[3].closed==2 && a->ring_quarantined==1 && copies==3);
    assert(!ps3dk_prof__auto_test_ring_admit(&context,1));
    puts("AUTO ring physical tail and native pre-wrap flush PASS");
    return 0;
}
