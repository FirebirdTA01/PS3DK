/* Report-ring producer, drainer and owner-only partial flush (PS3DK_PROF_AUTO_RING, host only). The REAL
 * ps3tc_fifo_wrap.c and runtime run against an in-order executor of the emitted words that the application drives a
 * frame behind its PUT: labels, waits, timestamp reports, the profiler's MAIN copies and the application's own M2MF
 * transfers (real bytes). The drainer thread is delayed, then held, then released. TEST_RING_MODE:
 *   0 run: >504 owner draws per frame, app transfers around block boundaries, ring full while the consumer is held,
 *          MAIN extent reuse after release, window END/reopen ABA, END during an admitted draw, a block left
 *          pending at seal (its records INCOMPLETE).
 *   1 meta: a small metadata budget; metadata-full is not latched for the rest of the frame.
 *   2 short: a failing sink acknowledges no ring ordinal (no MAIN extent is retired).
 *   3 fixture: a small complete capture for the reader (two frames, one over 504 draws).
 *   4 stop: a header the decoder rejects, after four sealed blocks and before any PUT: admission halts, the four
 *          blocks still publish and retire, the fifth is quarantined and held, the decoder names the word.
 *   5 label: a label anomaly after two published blocks stops the ring with both extents held.
 *   6 flip: the context begins 4 KiB into the cellGcmInit host buffer (IO 0); each flip CALLs a default-state routine
 *          at IO 0 (synthetic, the firmware's layout) that rebinds subchannel 1 to M2MF and sets IN/OUT, changed
 *          between two flips: the shadow stays valid, the flush restores the routine's IN/OUT, every draw is timed.
 *   7 segment: the same layout, a CALL into the command buffer (an application segment): refused, admission halts.
 *   8 rsxf: RSX frame capture selected (PS3_PROF_RSX_FRAME, 4 KiB staging blocks): a VP uploaded in the stream and an FP
 *          bound through its container, labels in a scope, hooked array draws and clears, raw draws the hooks never
 *          see, refusals (busy, flags), then a second capture with the drainer held that truncates when both staging
 *          blocks are full. auto-runtime-test.py reads the capture back with the reader. */
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "auto_internal.h"
#include "ps3tc_prof_auto.h"
#if TEST_RING_MODE==8
#include "rsxf.h"
PS3DK_PROF_DEFINE_AUTOMATIC_RSX_FRAME_OPTIONS(PS3DK_PROF_RSX_FRAME_OPTIONS_INIT(4u, 2u));
#endif

#ifndef TEST_RING_MODE
#define TEST_RING_MODE 0
#endif
const auto_capture *ps3dk_prof__auto_test_capture(void);
#define RETIRED(a) __atomic_load_n(&(a)->ring.retired,__ATOMIC_ACQUIRE)
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_fifo_boundary(void *ctx);
int32_t ps3tc_fifo_wrap_callback(struct _gcmCtxData *,uint32_t);

#define WORDS 8192u
#define PHYS (WORDS-4u)
#if (TEST_RING_MODE>=6 && TEST_RING_MODE<=7)
/* the cellGcmInit host buffer: IO offset 0, a reserved prefix, then the command buffer (the context) */
#define PREFIX 1024u
static uint32_t host[PREFIX+WORDS];
#define fifo (host+PREFIX)
#define IO (4u*PREFIX)
#else
static uint32_t fifo[WORDS];
#define IO 0x40000u
#endif
#define MAIN_IO 0x0c000000u
#define VRAM 65536u
#define M(sub,m,n) (((uint32_t)(n)<<18)|((uint32_t)(sub)<<13)|(m))

static uint64_t reports[2048][2];
static uint32_t labels[256];
static gpp_ctx context;
static uint8_t *main_ring;
static uint32_t main_bytes;
static uint8_t vram_a[VRAM],vram_b[VRAM],expect_b[VRAM];
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
    main_ring=aligned_alloc(1u<<20,bytes);assert(main_ring);main_bytes=bytes;
    memset(main_ring,0xee,bytes);*ea=(uintptr_t)main_ring;return 0;
}
static int ring_map(uintptr_t ea,uint32_t bytes,uint32_t *offset) {(void)ea;(void)bytes;*offset=MAIN_IO;return 0;}
static const auto_ring_memory memory={ring_allocate,ring_map};

/* ---- the LV2 surface of ps3tc_fifo_wrap.c, and the executor behind PUT/GET ---- */
typedef struct { volatile uint32_t put,get,ref; } control;
static control ctrl;
static uint32_t m2mf[0x400],sem,copies,app_transfers;
static uint64_t tb,gpu_clock;
void *gcmGetControlRegister(void) {return &ctrl;}
int32_t gcmAddressToOffset(void *p,uint32_t *off)
{
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)fifo;
    if(a<b || a>b+4u*WORDS || ((a-b)&3u)) return -1;
    *off=IO+(uint32_t)(a-b);return 0;
}
uint64_t sysGetTimebaseFrequency(void) {return 79800000u;}
uint64_t auto_test_timebase(void) {return __atomic_add_fetch(&tb,1u,__ATOMIC_RELAXED);}
static uint32_t *at(uint32_t off) {assert(off>=IO && off<IO+4u*WORDS);return &fifo[(off-IO)/4u];}
static void apply(uint32_t sub,uint32_t m,uint32_t v)
{
    if(sub==0) {
        if(m==0x1d6c || m==0x64) sem=v/16u;
        else if(m==0x1d70) st32(&labels[sem],(v&0xff00ff00u)|((v>>16)&255u)|((v&255u)<<16));
        else if(m==0x68) assert(ld32(&labels[sem])==v); /* in order: the release already executed */
        else if(m==0x1800) st64(&reports[(v&0xffffffu)/16u][0],++gpu_clock);
        return;
    }
    if(sub>1 && (TEST_RING_MODE>=6 && TEST_RING_MODE<=7)) return;   /* the routine's other subchannels */
    assert(sub==1 && m<0x1000);
    m2mf[m/4u]=v;
    if(m!=0x328) return;
    if(m2mf[0x184/4]==0x66626660u) {
        /* the profiler's copy: LOCAL report bytes to the MAIN extent */
        uint32_t src=m2mf[0x30c/4],dst=m2mf[0x310/4],len=m2mf[0x31c/4];
        assert(m2mf[0x188/4]==0xfeed0001u && m2mf[0x320/4]==1 && m2mf[0x324/4]==0x101);
        assert(dst>=MAIN_IO && dst-MAIN_IO+len<=main_bytes && src+len<=sizeof reports);
        memcpy(main_ring+(dst-MAIN_IO),(uint8_t *)reports+src,len);copies++;
    } else {
        /* the application's transfer: its own objects, pitches, lengths and format, never the copy's */
        uint32_t src=m2mf[0x30c/4],dst=m2mf[0x310/4],pin=m2mf[0x314/4],pout=m2mf[0x318/4];
        uint32_t len=m2mf[0x31c/4],rows=m2mf[0x320/4];
        assert(m2mf[0x184/4]==0x11110000u && m2mf[0x188/4]==0x22220000u && m2mf[0x324/4]==0x101);
        for(uint32_t r=0;r<rows;r++) {
            assert(src+r*pin+len<=VRAM && dst+r*pout+len<=VRAM);
            memcpy(vram_b+dst+r*pout,vram_a+src+r*pin,len);
        }
        app_transfers++;
    }
}
/* one CALL level: a routine in the host buffer's reserved prefix, run to its RET */
static void gpu_call(uint32_t target)
{
#if (TEST_RING_MODE>=6 && TEST_RING_MODE<=7)
    const uint32_t *w=host+target/4u;
    assert(target<IO);
    for(;;) {
        uint32_t h=*w++,n=(h>>18)&2047u,m=h&0x1ffcu,sub=(h>>13)&7u;
        if(h==0x00020000u) return;
        assert(!(h&0xa0000003u) && w+n<=host+PREFIX);
        for(uint32_t i=0;i<n;i++) apply(sub,m+((h&0x40000000u) ? 0:4u*i),w[i]);
        w+=n;
    }
#else
    (void)target;abort();
#endif
}
static void gpu_run_to(uint32_t target)
{
    while(ctrl.get!=target) {
        uint32_t *w=at(ctrl.get),h=w[0],n=(h>>18)&2047u,m=h&0x1ffcu,sub=(h>>13)&7u;
        if((h&3u)==2u) {gpu_call(h&~3u);ctrl.get+=4u;continue;}
        if(h&0x20000000u) {ctrl.get=h&~0x20000000u;continue;}
        for(uint32_t i=0;i<n;i++) apply(sub,m+((h&0x40000000u) ? 0:4u*i),w[1+i]);
        ctrl.get+=4u*(1u+n);
    }
}
static void gpu_run(void) {gpu_run_to(ctrl.put);}
/* the wrap protocol waits for GET to reach PUT: the GPU is then ahead of any older PUT the application lags to */
static uint32_t synced;
int sys_timer_usleep(unsigned long us) {(void)us;gpu_run();synced=1;return 0;}

/* ---- the drainer's pace: delayed every iteration, or held ---- */
static uint32_t hold,delay,parked;
static void pace(void)
{
    if(__atomic_load_n(&delay,__ATOMIC_ACQUIRE)) {struct timespec t={0,200000};nanosleep(&t,NULL);}
    if(__atomic_load_n(&hold,__ATOMIC_ACQUIRE)) __atomic_add_fetch(&parked,1u,__ATOMIC_ACQ_REL);
    while(__atomic_load_n(&hold,__ATOMIC_ACQUIRE)) sched_yield();
}
/* hold the drainer and wait until it is parked at its hook: nothing it does afterwards precedes the release */
__attribute__((unused)) static void park_drainer(void)
{
    uint32_t was=__atomic_load_n(&parked,__ATOMIC_ACQUIRE);
    __atomic_store_n(&hold,1u,__ATOMIC_RELEASE);
    for(int i=0;i<200000 && __atomic_load_n(&parked,__ATOMIC_ACQUIRE)==was;i++) {struct timespec t={0,100000};nanosleep(&t,NULL);}
    assert(__atomic_load_n(&parked,__ATOMIC_ACQUIRE)!=was);
}
static uint32_t sink_broken,sink_faults;
static int fault(unsigned long n,long *ret,int *err)
{
    /* only OPS-sized chunks fail: the first one the drainer writes after release is the batch under test */
    if(!__atomic_load_n(&sink_broken,__ATOMIC_ACQUIRE) || n<2000u) return 0;
    __atomic_add_fetch(&sink_faults,1u,__ATOMIC_ACQ_REL);
    *ret=-1;*err=5;return 1;
}
__attribute__((unused)) static void wait_inflight(const auto_capture *a,uint64_t n)
{
    for(int i=0;i<200000 && __atomic_load_n(&a->operations.write_idx,__ATOMIC_ACQUIRE)-
                            __atomic_load_n(&a->operations.read_idx,__ATOMIC_ACQUIRE)>n;i++) {
        struct timespec t={0,100000};nanosleep(&t,NULL);
    }
}
__attribute__((unused)) static void settle_drainer(const auto_capture *a)
{
    /* every committed record accepted and acknowledged: the consumer caught up */
    for(int i=0;i<200000 && __atomic_load_n(&a->operations.read_idx,__ATOMIC_ACQUIRE)!=
                            __atomic_load_n(&a->operations.write_idx,__ATOMIC_ACQUIRE);i++) {
        struct timespec t={0,100000};nanosleep(&t,NULL);
    }
    assert(__atomic_load_n(&a->operations.read_idx,__ATOMIC_ACQUIRE)==
           __atomic_load_n(&a->operations.write_idx,__ATOMIC_ACQUIRE));
}

/* ---- the application ---- */
static uint32_t *reserve(uint32_t n)
{
    uint32_t *w;
    if(context.current+n>context.end) assert(context.callback(&context,n)==0);
    w=context.current;context.current+=n;  /* re-read after the callback */
    return w;
}
static uint32_t lag_put=IO;
/* FlushBuffer's order: the pre-flush hook, the publication snapshot, PUT, its release. The GPU then runs to the PUT
 * of the PREVIOUS flush (a frame behind) or, behind=0, catches up. */
__attribute__((unused)) static void app_flush(int behind)
{
    uint32_t off,frontier;
    ps3dk_prof__auto_preflush((void *)&context);
    frontier=ps3dk_prof__auto_publish_before((void *)&context);
    assert(gcmAddressToOffset(context.current,&off)==0);
    ctrl.put=off;__atomic_thread_fence(__ATOMIC_SEQ_CST);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    if(!behind) gpu_run();
    else if(!synced) gpu_run_to(lag_put);
    synced=0;lag_put=off;
}
static const ps3tc_auto_call clear={3,0,0,0,0,0,0,0xf3};
__attribute__((unused)) static void draw(void)
{
    uint32_t t,*w;
    /* the application keeps a whole draw and the profiler's room in its extent: the native wrap first */
    if((uint32_t)(context.end-context.current)<18u) {w=reserve(18);memset(w,0,18u*4u);}
    t=ps3dk_prof__auto_begin((void *)&context,&clear,0);
    w=reserve(6);
    w[0]=M(0,0x1d90,1);w[1]=0;w[2]=M(0,0x1d8c,1);w[3]=0;w[4]=M(0,0x1d94,1);w[5]=0xf3;
    ps3dk_prof__auto_end((void *)&context,t);
}
static uint32_t xfers,xfer_at;
/* split: Mode/Format now; offsets and the trigger after the next draw (which may flush a block between them) */
__attribute__((unused)) static void xfer_mode(uint32_t pitch,uint32_t len,uint32_t rows)
{
    uint32_t *w=reserve(9);
    w[0]=M(1,0x184,2);w[1]=0x11110000u;w[2]=0x22220000u;
    w[3]=M(1,0x314,5);w[4]=pitch;w[5]=pitch;w[6]=len;w[7]=rows;w[8]=0x101;
}
__attribute__((unused)) static void xfer_go(uint32_t pitch,uint32_t len,uint32_t rows)
{
    uint32_t src=(xfers*97u)%(VRAM/2u),dst=(xfer_at=(xfer_at+4096u)%(VRAM/2u)),*w=reserve(5);
    w[0]=M(1,0x30c,2);w[1]=src;w[2]=dst;w[3]=M(1,0x328,1);w[4]=0;
    for(uint32_t r=0;r<rows;r++) memcpy(expect_b+dst+r*pitch,vram_a+src+r*pitch,len);
    xfers++;
}
__attribute__((unused)) static void xfer(void) {xfer_mode(256,64+(xfers%5u)*16u,3);xfer_go(256,64+(xfers%5u)*16u,3);}
__attribute__((unused)) static void frame(uint32_t draws)
{
    ps3dk_prof_frame();
    for(uint32_t i=0;i<draws;i++) {
        int split=i%126u==125u;   /* around the last pair of a frame-aligned block */
        if(split) xfer_mode(128,48,2);
        draw();
        if(split) xfer_go(128,48,2);
        if(i%63u==10u) xfer();
    }
}

/* ---- modes 6, 7: a default-state routine at IO 0 (synthetic, the firmware's layout, not its words) and the flip ---- */
#if (TEST_RING_MODE>=6 && TEST_RING_MODE<=7)
static void install_routine(uint32_t in,uint32_t out)
{
    const uint32_t r[]={M(0,0,1),0x31337000u,M(0,0x180,2),0x66604200u,0xfeed0000u,M(7,0,1),0xcafebabeu,
                        M(1,0,1),0x31337303u,M(1,0x180,3),0x66604200u,in,out,
                        M(3,0,1),0x313371c3u,M(3,0x180,2),0x66604200u,0xfeed0000u,0x00020000u};
    memset(host,0,4u*PREFIX);memcpy(host,r,sizeof r);
}
/* the flip's owner words: buffer id, NOPs, CALL IO 0, then (decoding resumes) an application offset pair */
static void flip_words(uint32_t id,uint32_t target,uint32_t src,uint32_t dst)
{
    uint32_t *w=reserve(12);
    w[0]=M(7,0x944,1);w[1]=id;w[2]=0;w[3]=0;w[4]=target|2u;
    w[5]=M(1,0x30c,2);w[6]=src;w[7]=dst;w[8]=M(7,0x924,1);w[9]=0x80000100u|id;w[10]=0;w[11]=0;
}
#endif
/* before the profiler's startup constructor (100) starts the drain thread that reads these hooks */
__attribute__((constructor(99))) static void install_hooks(void)
{
    ps3dk_prof_test_hooks.drainer_after_snapshot=pace;
    ps3dk_prof_test_hooks.write_fault=fault;
}

int main(void)
{
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;
    uint32_t instance,domain;
    const auto_capture *a;
    setvbuf(stdout,NULL,_IONBF,0);   /* progress survives a failed assertion */
    for(uint32_t i=0;i<VRAM;i++) vram_a[i]=(uint8_t)(i*31u+7u);
    memset(labels,0x5a,sizeof labels);
    context=(gpp_ctx){fifo,fifo+PHYS,fifo,(int32_t (*)(gpp_ctx *,uint32_t))(void (*)(void))ps3tc_fifo_wrap_callback};
    ctrl.put=ctrl.get=IO;
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof_test_hooks.auto_ring_memory=&memory;
#if (TEST_RING_MODE>=6 && TEST_RING_MODE<=7)
    ps3dk_prof__auto_gcm_init_result(&context,4u*WORDS,(uint32_t)sizeof host,host,0);
#else
    ps3dk_prof__auto_gcm_init_result(&context,4u*WORDS,4u*WORDS,fifo,0);
#endif
    o.ring_blocks=2;o.ring_bytes=1u<<20;o.frame_records=256;
    o.ring_records=TEST_RING_MODE==1 ? 1000u:65536u;    assert(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0);
    a=ps3dk_prof__auto_test_capture();
    assert(a->ring_plan.slots==256 && a->ring_plan.pairs==126 && a->ring.capacity==256);
    ps3dk_prof__auto_fifo_boundary(&context);

#if TEST_RING_MODE==0
    /* 1: eight frames of 600 draws, the GPU a frame behind, the consumer delayed every iteration */
    __atomic_store_n(&delay,1u,__ATOMIC_RELEASE);
    for(uint32_t f=0;f<8;f++) {
        frame(600);
        /* the frame's last block settles at the next boundary; a full block flushed right after its last pair */
        assert(a->ring.active==0 || a->ring.slots[(a->ring.active-1u)%256u].count<126u);
        app_flush(1);
    }
    ps3dk_prof_frame();
    assert(!a->tail.active && !a->ring.active);   /* the frame boundary settled the open block */
    app_flush(0);app_flush(0);
    settle_drainer(a);
    assert(RETIRED(a)==a->ring.issued && !a->ring.invalid);
    __atomic_store_n(&delay,0u,__ATOMIC_RELEASE);
    printf("phase 1: %u blocks issued and retired, %u copies, %u app transfers\n",a->ring.issued,copies,app_transfers);

    /* 2: the consumer held: MAIN extents run out (ring_full), never reused before their acknowledgement */
    {
        uint32_t frames=0,full=0;
        __atomic_store_n(&hold,1u,__ATOMIC_RELEASE);
        while(!full && frames<120) {
            frame(600);app_flush(1);frames++;
            full=a->coverage.live.ring_full[2]!=0;
        }
        assert(full && a->ring.issued-RETIRED(a)==256u);
        for(uint32_t i=0;i<3;i++) {frame(600);app_flush(1);}
        assert(a->ring.issued-RETIRED(a)==256u);
        printf("phase 2: ring full after %u held frames, %u blocks issued, %u retired\n",frames,a->ring.issued,RETIRED(a));
        ps3dk_prof_frame();app_flush(0);app_flush(0);
        __atomic_store_n(&hold,0u,__ATOMIC_RELEASE);
        settle_drainer(a);
        assert(RETIRED(a)==a->ring.issued);
    }
    /* 3: released: every draw timed again, MAIN extents reused (generations beyond the 256 extents) */
    for(uint32_t f=0;f<120 && a->ring.issued<=2u*256u;f++) {frame(600);app_flush(1);}
    assert(a->ring.issued>2u*256u);
    ps3dk_prof_frame();app_flush(0);app_flush(0);settle_drainer(a);

    /* a full block flushes right after its last pair */
    for(uint32_t i=0;i<126;i++) draw();
    assert(a->ring.active==0 || a->ring.slots[(a->ring.active-1u)%256u].count<126u);
    ps3dk_prof_frame();app_flush(0);app_flush(0);settle_drainer(a);
    /* 4: window END and reopen seen only as two ids: the block of the first window is settled, never shared */
    {
        uint32_t id=__atomic_load_n(&g_prof->window_open,__ATOMIC_ACQUIRE),g,flushes;
        assert(id);
        draw();g=a->ring.active;flushes=(uint32_t)a->ring_flushes;
        assert(g && a->tail.active);
        __atomic_store_n(&g_prof->window_open,id+1000u,__ATOMIC_RELEASE);
        draw();
        assert(a->ring.active==g+1u && a->ring_flushes==flushes+1u);   /* window ABA: a new block */
        __atomic_store_n(&g_prof->window_open,id,__ATOMIC_RELEASE);
        /* END during an admitted draw: the draw keeps its token; the owner settles the block at its next boundary */
        {
            uint32_t t=ps3dk_prof__auto_begin((void *)&context,&clear,0),*w;
            assert(t);
            __atomic_store_n(&g_prof->window_open,0u,__ATOMIC_RELEASE);
            w=reserve(6);memset(w,0,24);
            ps3dk_prof__auto_end((void *)&context,t);
            assert(a->tail.active);
            ps3dk_prof_frame();   /* outside the window: the owner flushes, no row opens */
            assert(!a->tail.active && !a->ring.active);
            __atomic_store_n(&g_prof->window_open,id,__ATOMIC_RELEASE);
        }
        app_flush(0);app_flush(0);settle_drainer(a);
    }
    {
        uint32_t bad=0,first=VRAM;
        for(uint32_t i=0;i<VRAM;i++) if(vram_b[i]!=expect_b[i]) {bad++;if(first==VRAM) first=i;}
        printf("app transfers %u of %u issued, %u bytes differ (first at %u)\n",app_transfers,xfers,bad,first);
    }
    assert(app_transfers==xfers && !memcmp(vram_b,expect_b,VRAM));
    /* 5: a block left pending at seal: flushed by the owner's retirement, never executed; its 5 records INCOMPLETE */
    frame(5);
    printf("AUTO ring producer, drainer, partial flush: %u blocks, %u copies, %u app transfers PASS\n",
           a->ring.issued,copies,app_transfers);
#elif TEST_RING_MODE==1
    /* metadata-full: 1000 records while the consumer is held; after release the same frame times again */
    __atomic_store_n(&hold,1u,__ATOMIC_RELEASE);
    ps3dk_prof_frame();
    for(uint32_t i=0;i<1200;i++) draw();
    {
        /* the frame marks hold metadata too: at most two of the 1000 records are not draws */
        uint64_t timed=a->coverage.live.counts[2][1],meta=a->coverage.live.counts[2][4];
        assert(timed+meta==1200u && meta>=200u && meta<=202u && a->ring_flushes>=7u);
        app_flush(0);app_flush(0);
        __atomic_store_n(&hold,0u,__ATOMIC_RELEASE);
        wait_inflight(a,126u);   /* everything published was accepted; the open block is not published yet */
        for(uint32_t i=0;i<10;i++) draw();
        assert(a->coverage.live.counts[2][1]==timed+10u && a->pool_tail_column==0);  /* no frame-tail latch */
    }
    ps3dk_prof_frame();app_flush(0);app_flush(0);settle_drainer(a);
    puts("AUTO ring metadata-full without a frame-tail latch PASS");
#elif TEST_RING_MODE==2
    /* a failing sink: no ring ordinal is acknowledged, no MAIN extent retires */
    frame(130);ps3dk_prof_frame();app_flush(0);app_flush(0);settle_drainer(a);
    {
        /* two whole 10-draw blocks published while the consumer is held; its first batch then fails */
        uint32_t retired=RETIRED(a);
        __atomic_store_n(&hold,1u,__ATOMIC_RELEASE);
        __atomic_store_n(&sink_broken,1u,__ATOMIC_RELEASE);
        frame(10);frame(10);ps3dk_prof_frame();app_flush(0);app_flush(0);
        assert(a->ring.issued>=retired+2u && !a->tail.active);
        __atomic_store_n(&hold,0u,__ATOMIC_RELEASE);
        for(int i=0;i<200;i++) {struct timespec t={0,1000000};nanosleep(&t,NULL);}
        assert(__atomic_load_n(&sink_faults,__ATOMIC_ACQUIRE) && RETIRED(a)==retired);
    }
    puts("AUTO ring short sink retires nothing PASS");
#elif TEST_RING_MODE==4
    /* RPCS3 r1: the first cellGcmSetFlip wrote a word the M2MF decoder rejects after four sealed blocks and before the
     * first PUT. Block 5's flush is refused with nothing stored: it is quarantined and new admission halts, while
     * blocks 1-4, whose copies were emitted with known state, still publish, drain and retire. */
    {
        uint32_t *w,at;
        ps3dk_prof_frame();
        for(uint32_t i=0;i<4u*126u;i++) draw();
        assert(a->ring.issued==4 && !a->ring.active && a->ring_flushes==4 && ctrl.put==IO);
        for(uint32_t i=0;i<10;i++) draw();
        assert(a->ring.active==5 && a->tail.active);
        w=reserve(2);w[0]=M(1,0,1);w[1]=0xfeed0000u;   /* an object binding on the M2MF subchannel */
        at=(uint32_t)((uintptr_t)w-(uintptr_t)fifo);
        ps3dk_prof_frame();   /* the frame boundary settles block 5: its scan rejects the header */
        assert(a->shadow.invalid && a->ring_quarantined==1 && a->ring.slots[4].closed==2 && !a->tail.active);
        for(uint32_t i=0;i<5;i++) draw();   /* refused: ring_state */
        assert(a->coverage.live.ring_state[2]==5 && a->ring.issued==5);
        app_flush(0);app_flush(0);   /* the first PUT: blocks 1-4 publish */
        settle_drainer(a);
        printf("ring stop: %u blocks issued, %u retired, header written at +%#x\n",a->ring.issued,RETIRED(a),at);
        assert(RETIRED(a)==4);
        assert(a->ring.halted && a->ring_refusal==AUTO_RING_REFUSED_EMIT);
        assert(a->ring_reject[2]==AUTO_RING_REJECT_BINDING && a->ring_reject[0]==M(1,0,1) && a->ring_reject[1]==at);
    }
    puts("AUTO ring producer-only stop drains the sealed blocks PASS");
#elif TEST_RING_MODE==5
    /* a future ring label after two published blocks: the consumer stops the ring, nothing is read or retired and
     * both extents stay held at seal */
    park_drainer();
    frame(130);ps3dk_prof_frame();app_flush(0);app_flush(0);
    assert(a->ring.issued==2 && !a->tail.active && RETIRED(a)==0);
    st32(&labels[a->ring_label],2u*a->ring.issued+2u);
    __atomic_store_n(&hold,0u,__ATOMIC_RELEASE);
    settle_drainer(a);
    assert(a->ring.invalid && RETIRED(a)==0);
    puts("AUTO ring stopped with held extents PASS");
#elif TEST_RING_MODE==6
    /* two flips, each CALLing the routine at IO 0, the routine changed between them; every block restores the
     * routine's IN/OUT and the application's offsets written after the CALL */
    {
        static const uint32_t in[2]={0xfeed0001u,0xaaaa0001u},out[2]={0xfeed0000u,0xbbbb0000u};
        uint32_t draws=0;
        xfer_mode(256,64,3);   /* the application's own IN/OUT and format before the first flip */
        for(uint32_t f=0;f<2;f++) {
            uint32_t restored=(uint32_t)a->ring_restored;
            install_routine(in[f],out[f]);
            ps3dk_prof_frame();
            for(uint32_t i=0;i<130;i++) {draw();draws++;}   /* a full block, then one open across the flip */
            flip_words(f,0,0x40u+f,0x80u+f);
            for(uint32_t i=0;i<10;i++) {draw();draws++;}
            ps3dk_prof_frame();   /* settles the open block: its flush restores the state after the routine */
            app_flush(0);app_flush(0);
            settle_drainer(a);
            printf("flip %u: shadow invalid %u reason %u, IN %08x OUT %08x src %#x, restored %u\n",f,a->shadow.invalid,
                   a->ring_reject[2],m2mf[0x184/4],m2mf[0x188/4],m2mf[0x30c/4],(uint32_t)a->ring_restored-restored);
            assert(!a->shadow.invalid && !a->ring_reject[2] && !a->ring.halted && !a->ring.invalid);
            assert(m2mf[0x184/4]==in[f] && m2mf[0x188/4]==out[f] && m2mf[0x30c/4]==0x40u+f && m2mf[0x310/4]==0x80u+f);
            assert(a->ring_restored>restored && RETIRED(a)==a->ring.issued);
        }
        printf("%u draws, %u blocks issued, %u retired, ring_state %llu\n",draws,a->ring.issued,RETIRED(a),
               (unsigned long long)a->coverage.live.ring_state[2]);
        assert(a->coverage.live.ring_state[2]==0 && a->ring.issued==4);
    }
    puts("AUTO ring flip CALL into the reserved prefix PASS");
#elif TEST_RING_MODE==7
    /* a CALL into the command buffer is an application segment, never decoded: refused, admission halts */
    {
        uint32_t at;
        install_routine(0xfeed0001u,0xfeed0000u);
        ps3dk_prof_frame();
        for(uint32_t i=0;i<10;i++) draw();
        at=(uint32_t)(4u*(context.current-context.begin))+16u;
        flip_words(0,IO+0x100u,0x40u,0x80u);   /* its target: 0x100 bytes into the context */
        ps3dk_prof_frame();
        assert(a->shadow.invalid && a->ring_quarantined==1 && a->ring.halted);
        printf("segment CALL: reason %u header %08x at +%#x (written at +%#x)\n",a->ring_reject[2],a->ring_reject[0],
               a->ring_reject[1],at);
        assert(a->ring_reject[2]==AUTO_RING_REJECT_CALL_TARGET && a->ring_reject[0]==((IO+0x100u)|2u) &&
               a->ring_reject[1]==at);
    }
    puts("AUTO ring CALL into an application segment refused PASS");
#elif TEST_RING_MODE==8
    {
        static const ps3tc_auto_call array={1,5,0,36,0,0,0,0};
        /* FP container ucode: inst 0 reads a constant (its 16-byte block follows), inst 1 ends (halfword-swapped) */
        static const uint8_t fp_ucode[48]={0x3e,0x80,0x00,0x00, 0xc8,0x02,0x00,0x00, 0xc8,0x00,0x00,0x00,
            0xc8,0x00,0x00,0x00, 1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16, 0x3e,0x81,0x00,0x00,
            0xc8,0x00,0x00,0x00, 0xc8,0x00,0x00,0x00, 0xc8,0x00,0x00,0x00};
        const rsxf_live_ops *ops=ps3dk_prof__rsxf_live_ops();
        rsxf_status st;
        uint32_t *w,lid=0;
        int id,rc;
        assert(ops && ops->available());
        /* the program state before the capture: a VP uploaded at slot 5 (3 instructions), an FP bound at 0x2000 */
        w=reserve(19);
        w[0]=M(0,0x1e9c,2);w[1]=5;w[2]=5;w[3]=M(0,0x0b80,12);
        for(uint32_t i=0;i<12;i++) w[4+i]=0x10000000u+(i<<8);   /* bit 0 clear: only the last instruction ends */
        w[15]|=1u;                                         /* word 3 of the last instruction: last */
        w[16]=M(0,0x08e4,1);w[17]=0x2001u;w[18]=0;           /* FP_ADDRESS (location 0 + 1 | offset), a NOP word */
        ps3dk_prof__rsxf_fp_bind((void *)&context,fp_ucode,sizeof fp_ucode,0x2000u);
        ps3dk_prof_frame();
        draw();
        assert(ps3dk_prof_rsx_frame_capture("bad",8u)==PS3DK_PROF_EINVAL);   /* an unknown flag */
        assert(ps3dk_prof_rsx_frame_capture("bad",5u)==PS3DK_PROF_EINVAL);   /* ISOLATED with BOTH */
        id=ps3dk_prof_rsx_frame_capture("first",0u);
        assert(id>0);
        assert(ps3dk_prof_rsx_frame_capture("again",0u)==PS3DK_PROF_EBUSY);
        ps3dk_prof_frame();                                  /* the window is open: armed for this frame */
        ops->status(&st);
        assert(st.id==0 && st.state==0);                     /* an app request never touches the live status */
        ps3dk_prof_draw_scope_begin("terrain");
        ps3dk_prof_draw_label("rock");
        for(uint32_t i=0;i<10;i++) {
            uint32_t t=ps3dk_prof__auto_begin((void *)&context,&array,0x1234);
            w=reserve(10);
            w[0]=M(0,0x100,3)|0x40000000u;w[1]=w[2]=w[3]=0;
            w[4]=M(0,0x1808,1);w[5]=5;w[6]=M(0,0x1814,1);w[7]=(35u<<24)|0u;w[8]=M(0,0x1808,1);w[9]=0;
            ps3dk_prof__auto_end((void *)&context,t);
        }
        ps3dk_prof_draw_label("water");
        for(uint32_t i=0;i<10;i++) draw();
        ps3dk_prof_draw_scope_end();
        /* two raw draws: the hooks never see them, the decoder does */
        for(uint32_t i=0;i<2;i++) {
            w=reserve(6);
            w[0]=M(0,0x1808,1);w[1]=4;w[2]=M(0,0x1814,1);w[3]=(5u<<24)|7u;w[4]=M(0,0x1808,1);w[5]=0;
        }
        ps3dk_prof__auto_preflush((void *)&context);         /* the decoder catches up before the boundary */
        ps3dk_prof_frame();                                  /* closes the capture */
        app_flush(0);app_flush(0);settle_drainer(a);
        /* the next request is accepted once the first capture is written */
        for(int i=0;i<20000;i++) {
            rc=ops->request(9u,0u,"second",6u,&lid);
            if(rc!=PS3DK_PROF_EBUSY) break;
            struct timespec t={0,100000};nanosleep(&t,NULL);
        }
        assert(rc==PS3DK_PROF_OK && lid==(uint32_t)id+1u);
        printf("capture %d written\n",id);
        /* a second capture, live, with the drainer held: both 4 KiB blocks fill and it truncates, never waits */
        park_drainer();
        ops->status(&st);
        assert(st.id==lid && st.request==9u && st.state==5u);
        ps3dk_prof_frame();
        ops->status(&st);
        assert(st.id==lid && st.state==1u && st.frame);
        for(uint32_t i=0;i<200;i++) draw();
        ps3dk_prof_frame();
        ops->status(&st);
        assert(st.id==lid && st.state==2u && st.reason==RSXF_R_STAGING && st.draws==200u);
        __atomic_store_n(&hold,0u,__ATOMIC_RELEASE);
        app_flush(0);app_flush(0);settle_drainer(a);
        for(int i=0;i<20000;i++) {ops->status(&st);if(st.state==3u) break;struct timespec t={0,100000};nanosleep(&t,NULL);}
        assert(st.state==3u);
        printf("capture %u: truncated, %u draws seen\n",lid,st.draws);
        /* an app request after it leaves the live link's final state in place (nothing written: never armed) */
        assert(ps3dk_prof_rsx_frame_capture("third",PS3DK_PROF_RSX_FRAME_ISOLATED)==(int)lid+1);
        ops->status(&st);
        assert(st.id==lid && st.state==3u);
        /* the ISOLATED capture: armed at the next boundary, each draw's markers behind WAIT_FOR_IDLE (room for the
         * four extra words kept in the extent like the draw's own) */
        ps3dk_prof_frame();
        for(uint32_t i=0;i<10;i++) {
            if((uint32_t)(context.end-context.current)<22u) {w=reserve(22);memset(w,0,22u*4u);}
            draw();
        }
        ps3dk_prof_frame();
        app_flush(0);app_flush(0);settle_drainer(a);
    }
    puts("RSX frame capture runtime PASS");
#else
    /* fixture: one frame over 504 draws and a small one, all timed and published */
    frame(520);app_flush(1);
    frame(40);
    ps3dk_prof_frame();app_flush(0);app_flush(0);settle_drainer(a);
    assert(app_transfers==xfers && !memcmp(vram_b,expect_b,VRAM));
    puts("AUTO ring fixture PASS");
#endif
    return 0;
}
