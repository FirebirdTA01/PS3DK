/* Private runtime schema2 acceptance: the existing real-word ring executor and
 * native FIFO provider, with actual THREAD/core output. No encoder replay and
 * no successful stand-in for the missing runtime selector. */
#define TEST_RING_MODE 3
#define main schema2_existing_ring_fixture
#include "../ps3dk-prof-runtime/auto-ring-runtime-test.c"
#undef main
#include "prof_identity.h"
#include "prof_spu.h"
#include "roll_runtime_test.h"
#include "roll_runtime.h"
#include "roll_combined2.h"
#include <inttypes.h>
#include <pthread.h>

extern int ps3dk_prof_test_rolling_schema2_set(const ps3dk_prof_test_rolling_options *,
                                             const pfm2_config *) __attribute__((weak));
const ps3dk_prof__identity_t ps3dk_prof__identity={PS3DK_PROF_IDENTITY_MARKER,{0x5b},463,1,12,"SCHEMA2-HOST"};
const uint8_t ps3dk_prof__auto_mechanism[32]={42};
const uint8_t ps3dk_prof__auto_build_binding[32]={99};
const char ps3dk_prof__auto_build_id[]="schema2-host";
const char ps3dk_prof__auto_app[]="schema2-runtime";
#define S2(x,m) do {if(!(x)){fprintf(stderr,"RUNTIME_SCHEMA2_ASSERT[%s]: %s\n",m,#x);exit(1);}}while(0)
static const char *s2_out;
static char s2_capture[1024];
static FILE *s2_audit;
static ps3dk_prof_test_rolling_options s2_options;
static pfm2_config s2_config;
static pfr_layout s2_layout;
static uint64_t s2_bytes,s2_chunks,s2_allocs,s2_alloc_bytes,s2_init_allocs,s2_init_bytes;
static uint64_t s2_attach_allocs,s2_attach_bytes,s2_steady_allocs,s2_steady_bytes;
static uint64_t s2_refused_next,s2_refused_bytes,s2_ack_count,s2_ring_acks;
static uint64_t s2_refused_read;
static uint32_t s2_refused_retired;
static uint32_t s2_refused_type,s2_refusals,s2_rw,s2_append,s2_main_allocs,s2_maps,s2_mapped_bytes;
static pthread_mutex_t s2_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s2_cond=PTHREAD_COND_INITIALIZER;
static int s2_hold=1,s2_parked;
static uint64_t s2_cycles;
static uint32_t s2_r32(const uint8_t *b){return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
static uint64_t s2_r64(const uint8_t *b){return s2_r32(b)|(uint64_t)s2_r32(b+4)<<32;}
static void s2_path(char *b,size_t cap,const char *name)
{int n=snprintf(b,cap,"%s/%s",s2_out,name);S2(n>=0&&(size_t)n<cap,"artifact-path");}
static uint64_t s2_now(void)
{struct timespec t;S2(!clock_gettime(CLOCK_MONOTONIC,&t),"monotonic-clock");return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
static void s2_app_flush(int behind)
{
    uint32_t off,frontier;
    ps3dk_prof__auto_preflush((void *)&context);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    S2(gcmAddressToOffset(context.current,&off)==0,"actual-PUT-offset");
    frontier=ps3dk_prof__auto_publish_before((void *)&context);
    ctrl.put=off;__atomic_thread_fence(__ATOMIC_SEQ_CST);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    /* Match rsx_legacy/buffer_impl.h: the report serial is NOT a PUT offset.
     * The reused ring-only fixture predates FIFO accounting and omits this
     * separate exact-offset observation. Its omission correctly yields FLOW. */
    ps3dk_prof__fifo_account_native_publish((void *)&context,off);
    if(!behind)gpu_run();
    else if(!synced)gpu_run_to(lag_put);
    synced=0;lag_put=off;
}
static void s2_pace(void)
{
    S2(!pthread_mutex_lock(&s2_lock),"worker-lock");++s2_cycles;
    if(s2_hold){s2_parked=1;S2(!pthread_cond_broadcast(&s2_cond),"worker-notify");}
    while(s2_hold)S2(!pthread_cond_wait(&s2_cond,&s2_lock),"worker-wait");
    s2_parked=0;S2(!pthread_cond_broadcast(&s2_cond),"worker-released");
    S2(!pthread_mutex_unlock(&s2_lock),"worker-unlock");
}
static void s2_pause(void)
{
    struct timespec until;S2(!clock_gettime(CLOCK_REALTIME,&until),"barrier-clock");until.tv_sec+=20;
    S2(!pthread_mutex_lock(&s2_lock),"owner-lock");s2_hold=1;
    while(!s2_parked)S2(!pthread_cond_timedwait(&s2_cond,&s2_lock,&until),"worker-barrier-deadline");
    S2(!pthread_mutex_unlock(&s2_lock),"owner-unlock");
}
static void s2_resume(void)
{
    struct timespec until;S2(!clock_gettime(CLOCK_REALTIME,&until),"release-clock");until.tv_sec+=20;
    S2(!pthread_mutex_lock(&s2_lock),"release-lock");s2_hold=0;
    S2(!pthread_cond_broadcast(&s2_cond),"release-worker");
    while(s2_parked)S2(!pthread_cond_timedwait(&s2_cond,&s2_lock,&until),"worker-release-deadline");
    S2(!pthread_mutex_unlock(&s2_lock),"release-unlock");
}
static void s2_settle(const auto_capture *a)
{
    uint64_t end=s2_now()+UINT64_C(20000000000);s2_resume();
    while(LD_ACQ(&a->operations.read_idx)!=LD_ACQ(&a->operations.write_idx)){
        S2(s2_now()<end,"accepted-queue-deadline");sched_yield();
    }
    /* The next snapshot is after the previous consumer pass and physical flush.
     * This is a handshake with the actual worker, not a sleep-based ordering. */
    s2_pause();
    S2(RETIRED(a)==LD_ACQ(&a->ring.issued),"accepted-blocks-retire");
}
void *__real_ps3dk_prof__pal_alloc(size_t);
void *__wrap_ps3dk_prof__pal_alloc(size_t n)
{
    void *p=__real_ps3dk_prof__pal_alloc(n);
    if(p){__atomic_add_fetch(&s2_allocs,1,__ATOMIC_RELAXED);__atomic_add_fetch(&s2_alloc_bytes,n,__ATOMIC_RELAXED);}
    return p;
}
static int s2_ring_allocate(uint32_t n,uintptr_t *ea)
{++s2_main_allocs;return ring_allocate(n,ea);}
static int s2_ring_map(uintptr_t ea,uint32_t n,uint32_t *off)
{++s2_maps;s2_mapped_bytes=n;return ring_map(ea,n,off);}
static const auto_ring_memory s2_memory={s2_ring_allocate,s2_ring_map};
int __real_ps3dk_prof__pal_open_excl(const char *,int *,int *);
int __real_ps3dk_prof__pal_open_rw_excl(const char *,int *,int *);
static void s2_opened(const char *p){S2(strlen(p)<sizeof s2_capture,"capture-path");strcpy(s2_capture,p);}
int __wrap_ps3dk_prof__pal_open_excl(const char *p,int *fd,int *err)
{int rc=__real_ps3dk_prof__pal_open_excl(p,fd,err);if(!rc){++s2_append;s2_opened(p);}return rc;}
int __wrap_ps3dk_prof__pal_open_rw_excl(const char *p,int *fd,int *err)
{int rc=__real_ps3dk_prof__pal_open_rw_excl(p,fd,err);if(!rc){++s2_rw;s2_opened(p);}return rc;}
static void s2_copy(const char *name)
{
    char p[1200];uint8_t b[8192];size_t n;uint64_t total=0;FILE *in,*out;
    s2_path(p,sizeof p,name);in=fopen(s2_capture,"rb");out=fopen(p,"wbx");S2(in&&out,"snapshot-open");
    while((n=fread(b,1,sizeof b,in))!=0){total+=n;S2(total<=16u*1024u*1024u,"snapshot-bound");S2(fwrite(b,1,n,out)==n,"snapshot-write");}
    S2(!ferror(in)&&!fclose(in)&&!fclose(out),"snapshot-close");
}
int __real_ps3dk_prof__roll_combined2_init(void *,size_t,const pfm2_config *,const uint8_t[16],void *,size_t,void *,size_t,size_t *);
int __wrap_ps3dk_prof__roll_combined2_init(void *ctx,size_t cb,const pfm2_config *cfg,const uint8_t h[16],void *a,size_t ab,void *cp,size_t cap,size_t *n)
{
    int rc=__real_ps3dk_prof__roll_combined2_init(ctx,cb,cfg,h,a,ab,cp,cap,n);
    if(!rc){char p[1200];S2(!s2_audit,"one-real-schema2-init");S2(s2_r32(h+12)==707,"original-header-unmasked");
        s2_path(p,sizeof p,"accepted-original.prof");s2_audit=fopen(p,"wbx");
        S2(s2_audit&&fwrite(h,1,16,s2_audit)==16&&!fflush(s2_audit),"original-header-audit");}
    return rc;
}
int __real_ps3dk_prof__roll_output_accept(pfo_output *,const void *,size_t);
int __wrap_ps3dk_prof__roll_output_accept(pfo_output *o,const void *raw,size_t n)
{
    int rc=__real_ps3dk_prof__roll_output_accept(o,raw,n);
    if(!rc){S2(s2_audit&&s2_bytes+n<=4u*1024u*1024u,"accepted-original-bound");
        S2(fwrite(raw,1,n,s2_audit)==n&&!fflush(s2_audit),"accepted-original-audit");s2_bytes+=n;++s2_chunks;}
    else if(rc==PFO_RESOURCE){const auto_capture *capture_at_refusal=ps3dk_prof__auto_test_capture();
        ++s2_refusals;s2_refused_type=s2_r32(raw);s2_refused_next=s2_chunks;s2_refused_bytes=s2_bytes;
        S2(capture_at_refusal,"live-producer-at-refusal");
        s2_refused_read=LD_ACQ(&capture_at_refusal->operations.read_idx);s2_refused_retired=RETIRED(capture_at_refusal);}
    return rc;
}
/* This bounded ledger contains only the current genuinely accepted OPS chunk.
 * Wraps delegate the original queue/ring algorithms; no ack is manufactured. */
static struct {uint64_t id;uint32_t generation,ordinal;} s2_ledger[6000];
static uint32_t s2_ledger_count,s2_ledger_at,s2_pending_generation,s2_pending_ordinal;
int __real_ps3dk_prof__fifo_emit_chunk(prof *,uint32_t,const uint8_t *,size_t);
int __wrap_ps3dk_prof__fifo_emit_chunk(prof *p,uint32_t ty,const uint8_t *b,size_t bytes)
{
    int rc=__real_ps3dk_prof__fifo_emit_chunk(p,ty,b,bytes);
    if(!rc&&ty==35){uint32_t n=s2_r32(b+8),width=s2_r32(b+12);
        S2(s2_ledger_at==s2_ledger_count&&!s2_pending_generation,"prior-OPS-ack-complete");
        S2(width==176&&n<=6000&&bytes==16+(size_t)n*width,"real-ring-OPS-layout");
        for(uint32_t i=0;i<n;++i){const uint8_t *q=b+16+(size_t)i*width;
            s2_ledger[i].id=s2_r64(q);s2_ledger[i].generation=s2_r32(q+160);s2_ledger[i].ordinal=(uint32_t)q[168]|(uint32_t)q[169]<<8;}
        s2_ledger_at=0;s2_ledger_count=n;
    }
    return rc;
}
int __real_ps3dk_prof__auto_queue_ack(auto_queue *,uint64_t,int,int);
int __wrap_ps3dk_prof__auto_queue_ack(auto_queue *q,uint64_t id,int ready,int whole)
{
    S2(whole&&s2_ledger_at<s2_ledger_count&&s2_ledger[s2_ledger_at].id==id&&!s2_pending_generation,"OPS-accepted-before-queue-ack");
    s2_pending_generation=s2_ledger[s2_ledger_at].generation;s2_pending_ordinal=s2_ledger[s2_ledger_at].ordinal;
    ++s2_ledger_at;++s2_ack_count;return __real_ps3dk_prof__auto_queue_ack(q,id,ready,whole);
}
int __real_ps3dk_prof__auto_ring_ack(auto_ring *,uint32_t,uint32_t,int);
int __wrap_ps3dk_prof__auto_ring_ack(auto_ring *r,uint32_t g,uint32_t ordinal,int whole)
{
    S2(whole&&g&&g==s2_pending_generation&&ordinal==s2_pending_ordinal,"OPS-accepted-before-ring-retirement");
    s2_pending_generation=0;++s2_ring_acks;return __real_ps3dk_prof__auto_ring_ack(r,g,ordinal,whole);
}
static void s2_setup(int enabled,int resource)
{
    ps3dk_prof_config c;ps3dk_prof_ext_config x={0};pfm2_layout semantic;
    ps3dk_prof_config_defaults(&c);c.mode=PS3DK_PROF_MODE_THREAD;c.max_threads=2;c.max_descriptors=8;c.ring_records=512;
    c.app="schema2-runtime";c.build_id="schema2-host";c.flags="automatic-rsx";c.quiesce_timeout_us=1000000;
    x.size=sizeof x;x.gpu_domains_max=1;x.gpu_report_slots_max=1024;
    s2_options.size=sizeof s2_options;s2_options.version=1;s2_options.enabled=(uint32_t)enabled;
    s2_options.max_state_bytes=64u<<20;s2_options.store.max_bytes=UINT64_MAX;s2_options.store.retained_slots=6;
    s2_options.store.checkpoint_bytes=262144;s2_options.store.payload_bytes=P_CHUNK_CAP+20;memset(s2_options.store.session,0x52,16);
    s2_config.shared=(pfm_config){8,8,32,128,8,8,4,32,256,16,512,128,16,128,65536,262144,UINT64_C(100000000)};
    s2_config.rsx_domains=1;s2_config.windows=8;s2_config.fifo_membership_runs=64;s2_config.accounting_groups=8;s2_config.family_flags=192;
    s2_config.auto_final_runs=64;s2_config.auto_touched_runs=64;s2_config.auto_build_entries=resource?1:64;
    s2_config.auto_clock_anchors=64;s2_config.auto_bootstrap_bytes=32768;s2_config.auto_support_bytes=65536;
    S2(pfm2_plan(&s2_config,&semantic)==PFR_OK,"actual-schema2-plan");S2(pfr_plan(&s2_options.store,&s2_layout)==PFR_OK,"actual-physical-plan");
    s2_options.store.max_bytes=s2_layout.file_bytes;
    ps3dk_prof_test_hooks.drainer_after_snapshot=s2_pace;ps3dk_prof_test_hooks.write_fault=NULL;
    ps3dk_prof_test_hooks.clock=auto_test_timebase;
    S2(ps3dk_prof_set_sink_path(s2_out)==0,"sink-path");
    if(enabled){S2(ps3dk_prof_test_rolling_schema2_set!=NULL,"missing-private-schema2-selector");
        S2(ps3dk_prof_test_rolling_schema2_set(&s2_options,&s2_config)==0,"real-schema2-selector");}
    S2(ps3dk_prof_init_ex(&c,&x)==0,"real-schema2-core-init");s2_pause();
    S2(s2_rw==(unsigned)enabled&&s2_append==(unsigned)!enabled,"actual-FILE-mode");
    s2_init_allocs=LD_ACQ(&s2_allocs);s2_init_bytes=LD_ACQ(&s2_alloc_bytes);
    if(enabled){S2(s2_chunks==3,"bootstrap-before-worker");s2_copy("ready.profr");}
    S2(ps3dk_prof_thread_begin("schema2-owner")==0&&PS3DK_PROF_CAPTURE_WHOLE_RUN()==0,"real-owner-window");
}
static const auto_capture *s2_attach(void)
{
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;uint32_t instance,domain;
    for(uint32_t i=0;i<VRAM;++i)vram_a[i]=(uint8_t)(i*31u+7u);
    memset(labels,0x5a,sizeof labels);
    context=(gpp_ctx){fifo,fifo+PHYS,fifo,(int32_t (*)(gpp_ctx *,uint32_t))(void (*)(void))ps3tc_fifo_wrap_callback};ctrl.put=ctrl.get=IO;
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;ps3dk_prof_test_hooks.auto_ring_memory=&s2_memory;
    ps3dk_prof__auto_gcm_init_result(&context,4u*WORDS,4u*WORDS,fifo,0);
    o.ring_blocks=2;o.ring_bytes=1u<<20;o.frame_records=256;o.ring_records=65536;
    S2(ps3dk_prof_auto_attach((void *)&context,&o,&instance,&domain)==0,"actual-native-ring-attach");
    const auto_capture *a=ps3dk_prof__auto_test_capture();S2(a&&a->fifo_instance&&a->ring_plan.slots==256&&a->ring_plan.pairs==126,"actual-ring-and-FIFO-owner");
    ps3dk_prof__auto_fifo_boundary(&context);
    s2_attach_allocs=LD_ACQ(&s2_allocs)-s2_init_allocs;s2_attach_bytes=LD_ACQ(&s2_alloc_bytes)-s2_init_bytes;
    s2_steady_allocs=LD_ACQ(&s2_allocs);s2_steady_bytes=LD_ACQ(&s2_alloc_bytes);
    S2(s2_attach_allocs&&s2_attach_bytes&&s2_main_allocs==1&&s2_maps==1&&main_bytes==a->ring_plan.total&&s2_mapped_bytes==(1u<<20),"separate-attach-allocation-facts");return a;
}
static void s2_plain_after_stop(void)
{
    uint32_t ticket,*w,n;uint64_t ack=s2_ack_count,ring_ack=s2_ring_acks;
    /* Only the application's context/device and test-owned counters are read
     * here. The FIFO observer/control block may already have been freed. */
    S2(context.end==fifo+PHYS,"stop-owner-restores-physical-tail");
    n=(uint32_t)(context.end-context.current);
    if(n){w=reserve(n);memset(w,0,(size_t)n*4);}
    ticket=ps3dk_prof__auto_begin((void *)&context,&clear,0);
    S2(!ticket,"retired-owner-AUTO-inert");w=reserve(6);
    S2(w==fifo&&context.current==fifo+6&&context.end==fifo+PHYS,"ordinary-native-wrap-after-resource-stop");
    w[0]=M(0,0x1d90,1);w[1]=0;w[2]=M(0,0x1d8c,1);w[3]=0;w[4]=M(0,0x1d94,1);w[5]=0xf3;
    ps3dk_prof__auto_end((void *)&context,ticket);s2_app_flush(0);
    S2(ctrl.get==ctrl.put&&ctrl.put==IO+24&&context.current==fifo+6,"ordinary-commands-execute-after-resource-stop");
    S2(s2_ack_count==ack&&s2_ring_acks==ring_ack,"continuation-does-not-ack-or-reuse-profiler-pool");
}
int main(int argc,char **argv)
{
    int enabled,resource,rc;const char *recipe;const auto_capture *a;uint32_t retired_before=0,issued=0;
    uint64_t read_before=0,draws=0;ps3dk_prof_test_rolling_record result={0};char p[1200];FILE *f;
    if(argc!=3){fputs("usage: runtime-schema2 CASE OUTPUT_DIRECTORY\n",stderr);return 2;}
    setvbuf(stdout,NULL,_IONBF,0);s2_out=argv[2];enabled=strncmp(argv[1],"append-",7)!=0;recipe=enabled?argv[1]:argv[1]+7;
    resource=!strcmp(recipe,"resource");S2(resource||!strcmp(recipe,"rotation"),"known-case");s2_setup(enabled,resource);a=s2_attach();
    if(enabled){uint32_t rejected=UINT32_MAX;
        S2(ps3dk_prof_gpu_attach((void *)&context,0,2,100,&rejected)==PS3DK_PROF_ENOTSUP&&rejected==UINT32_MAX,
           "manual-GPU-not-admitted-by-AUTO-profile");}
    if(resource){
        frame(10);draws+=10;s2_app_flush(0);ps3dk_prof_frame();s2_app_flush(0);s2_app_flush(0);s2_settle(a);
        retired_before=RETIRED(a);read_before=LD_ACQ(&a->operations.read_idx);S2(retired_before&&read_before,"useful-accepted-prefix-before-refusal");
        /* The worker is parked. These real operations occupy later build frames
         * and two copied MAIN extents; capacity is exhausted by actual OPS. */
        for(unsigned i=0;i<10;++i){draw();}
        frame(10);draws+=20;ps3dk_prof_frame();s2_app_flush(0);s2_app_flush(0);
        issued=LD_ACQ(&a->ring.issued);S2(issued>=retired_before+2&&!a->tail.active,"two-real-ready-extents-before-refusal");
        draw();++draws;S2(a->tail.active&&context.end!=fifo+PHYS,"resource-stop-has-live-reserved-tail");
        if(enabled){uint64_t end=s2_now()+UINT64_C(20000000000);s2_resume();
            while(!LD_ACQ(&g_prof->stop_latch)){S2(s2_now()<end,"resource-stop-deadline");sched_yield();}
            ps3dk_prof_thread_end();
            S2(context.end==fifo+PHYS,"retirement-settles-reserved-tail");
            while(LD_ACQ(&g_prof->phase)!=3u){S2(s2_now()<end,"automatic-finalization-deadline");sched_yield();}
            S2(!LD_ACQ(&g_prof->shutdown_once)&&LD_ACQ(&g_prof->rolling->stop_prepared),"automatic-stop-before-public-shutdown");
            /* Inspect only the snapshot taken inside real output refusal and
             * the test ACK ledger after cleanup; never a freed FIFO block. */
            S2(s2_refused_retired==retired_before&&s2_refused_read==read_before&&s2_ack_count==read_before,"resource-before-queue-or-ring-ack");
            S2(s2_refusals==1&&s2_refused_type==35,"resource-is-real-OPS-refusal");s2_copy("automatic-before-public.profr");
            s2_plain_after_stop();
        }else{ps3dk_prof_frame();s2_app_flush(0);s2_app_flush(0);s2_settle(a);}
    }else{
        for(unsigned i=0;i<12;++i){uint32_t n=i?40:520;frame(n);draws+=n;s2_app_flush(0);ps3dk_prof_frame();s2_app_flush(0);s2_app_flush(0);s2_settle(a);}
        issued=LD_ACQ(&a->ring.issued);S2(s2_ring_acks==draws&&issued>4,"more-than-504-real-ring-draws");
    }
    if(!enabled||!resource){
        S2(ps3dk_prof_capture_end()==0,"real-window-END");ps3dk_prof_frame();s2_app_flush(0);s2_app_flush(0);s2_settle(a);
        s2_resume();ps3dk_prof_thread_end();
    }
    S2(app_transfers==xfers&&!memcmp(vram_b,expect_b,VRAM),"application-M2MF-bytes-preserved");
    rc=ps3dk_prof_shutdown();S2(rc==(enabled&&resource?PS3DK_PROF_TEST_EROLLRESOURCE:0),"public-shutdown-result");
    S2(LD_ACQ(&s2_allocs)==s2_steady_allocs&&LD_ACQ(&s2_alloc_bytes)==s2_steady_bytes&&s2_main_allocs==1&&s2_maps==1,"no-steady-state-PAL-or-MAIN-allocation");
    S2(s2_ledger_at==s2_ledger_count&&!s2_pending_generation,"accepted-OPS-ledger-exhausted");
    if(enabled){S2(ps3dk_prof_test_rolling_result(&result)==0,"immutable-private-result");
        S2(result.original_flags==707&&result.result==rc&&result.state==(resource?PFR_STOPPED_RESOURCE:PFR_SEALED),"original-flags-and-terminal-state");
        S2(result.accepted_next_chunk==s2_chunks&&result.committed_next_chunk==s2_chunks&&result.accepted_payload_bytes==s2_bytes&&result.committed_payload_bytes==s2_bytes,"original-accepted-committed-frontiers");
        S2(result.state_bytes<=s2_options.max_state_bytes&&result.file_bytes==s2_layout.file_bytes,"explicit-core-state-and-physical-caps");
        S2(!result.io_result&&!result.close_result&&!result.join_result,"no-hidden-terminal-error");s2_copy("capture.profr");
        ps3dk_prof_test_rolling_record repeat;S2(ps3dk_prof_shutdown()==PS3DK_PROF_EALREADY&&ps3dk_prof_test_rolling_result(&repeat)==0&&!memcmp(&result,&repeat,sizeof result),"public-result-immutable");
    }else s2_copy("capture.prof");
    if(s2_audit){S2(!fclose(s2_audit),"original-audit-close");s2_audit=NULL;}
    s2_path(p,sizeof p,"result.json");f=fopen(p,"wbx");S2(f,"manifest-open");
    S2(fprintf(f,"{\"schema\":1,\"case\":\"%s\",\"flags\":707,\"draws\":%" PRIu64 ",\"issued\":%u,\"acks\":%" PRIu64 ",\"ring_acks\":%" PRIu64 ",\"before_read\":%" PRIu64 ",\"before_retired\":%u,\"next\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"revision\":%" PRIu64 ",\"state\":%u,\"reason\":%u,\"refusals\":%u,\"refused_type\":%u,\"refused_next\":%" PRIu64 ",\"refused_bytes\":%" PRIu64 ",\"init_PAL_calls\":%" PRIu64 ",\"init_PAL_bytes\":%" PRIu64 ",\"attach_PAL_calls\":%" PRIu64 ",\"attach_PAL_bytes\":%" PRIu64 ",\"MAIN_allocation_bytes\":%u,\"MAIN_mapped_bytes\":%u,\"steady_PAL_calls\":0,\"steady_MAIN_calls\":0,\"global_memory_cap_claim\":false}\n",
        argv[1],draws,issued,s2_ack_count,s2_ring_acks,read_before,retired_before,result.accepted_next_chunk,result.accepted_payload_bytes,result.revision,result.state,result.reason,s2_refusals,s2_refused_type,s2_refused_next,s2_refused_bytes,s2_init_allocs,s2_init_bytes,s2_attach_allocs,s2_attach_bytes,main_bytes,s2_mapped_bytes)>0,"manifest-write");
    S2(!fclose(f),"manifest-close");puts("actual schema2 runtime case PASS");return 0;
}
