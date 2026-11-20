/* Actual core selector/preflight only. Successful cases stop the TEST PROCESS
 * at READY with coherent OPEN bootstrap output, not a finalized capture.
 * Reuse the native provider's host port without manufacturing producer rows. */
#define TEST_RING_MODE 3
#define main preflight_unused_ring_fixture
#include "../ps3dk-prof-runtime/auto-ring-runtime-test.c"
#undef main
#include "prof_spu.h"
#include "prof_identity.h"
#include "roll_runtime_test.h"
#include "roll_runtime.h"
#include <inttypes.h>
#include <pthread.h>

const ps3dk_prof__identity_t ps3dk_prof__identity={PS3DK_PROF_IDENTITY_MARKER,{0x5b},463,1,12,"SCHEMA2-HOST"};
const uint8_t ps3dk_prof__auto_mechanism[32]={42};
const uint8_t ps3dk_prof__auto_build_binding[32]={99};
const char ps3dk_prof__auto_build_id[]="schema2-preflight";
const char ps3dk_prof__auto_app[]="schema2-preflight";
#define PF(x,m) do{if(!(x)){fprintf(stderr,"SCHEMA2_PREFLIGHT_ASSERT[%s]: %s\n",m,#x);exit(1);}}while(0)
static uint64_t allocations,allocated_bytes,accepted_chunks,accepted_bytes;
static uint32_t append_opens,rolling_opens,encoder_inits;
static char capture_path[1024];
static const char *output_dir;
static FILE *audit;
static pthread_mutex_t barrier_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_cond=PTHREAD_COND_INITIALIZER;
static int worker_parked;
static void park_ready_worker(void)
{
    PF(!pthread_mutex_lock(&barrier_lock),"worker-lock");worker_parked=1;
    PF(!pthread_cond_broadcast(&barrier_cond),"worker-ready-notify");
    for(;;)PF(!pthread_cond_wait(&barrier_cond,&barrier_lock),"worker-held-at-READY");
}
static void wait_ready_worker(void)
{
    struct timespec until;PF(!clock_gettime(CLOCK_REALTIME,&until),"deadline-clock");until.tv_sec+=20;
    PF(!pthread_mutex_lock(&barrier_lock),"owner-lock");
    while(!worker_parked)PF(!pthread_cond_timedwait(&barrier_cond,&barrier_lock,&until),"worker-ready-deadline");
    PF(!pthread_mutex_unlock(&barrier_lock),"owner-unlock");
}
static void path_for(char *p,size_t n,const char *leaf)
{int k=snprintf(p,n,"%s/%s",output_dir,leaf);PF(k>=0&&(size_t)k<n,"artifact-path");}
void *__real_ps3dk_prof__pal_alloc(size_t);
void *__wrap_ps3dk_prof__pal_alloc(size_t n)
{__atomic_add_fetch(&allocations,1,__ATOMIC_RELAXED);__atomic_add_fetch(&allocated_bytes,n,__ATOMIC_RELAXED);return __real_ps3dk_prof__pal_alloc(n);}
int __real_ps3dk_prof__pal_open_excl(const char *,int *,int *);
int __real_ps3dk_prof__pal_open_rw_excl(const char *,int *,int *);
static void save_path(const char *p){PF(strlen(p)<sizeof capture_path,"capture-path");strcpy(capture_path,p);}
int __wrap_ps3dk_prof__pal_open_excl(const char *p,int *fd,int *err)
{++append_opens;int rc=__real_ps3dk_prof__pal_open_excl(p,fd,err);if(!rc)save_path(p);return rc;}
int __wrap_ps3dk_prof__pal_open_rw_excl(const char *p,int *fd,int *err)
{++rolling_opens;int rc=__real_ps3dk_prof__pal_open_rw_excl(p,fd,err);if(!rc)save_path(p);return rc;}
int __real_ps3dk_prof__roll_combined2_init(void *,size_t,const pfm2_config *,const uint8_t[16],void *,size_t,void *,size_t,size_t *);
int __wrap_ps3dk_prof__roll_combined2_init(void *c,size_t cb,const pfm2_config *cfg,const uint8_t h[16],void *a,size_t ab,void *cp,size_t cap,size_t *n)
{
    ++encoder_inits;int rc=__real_ps3dk_prof__roll_combined2_init(c,cb,cfg,h,a,ab,cp,cap,n);
    if(!rc){char p[1200];path_for(p,sizeof p,"accepted-original.prof");PF(!audit,"single-encoder-init");audit=fopen(p,"wbx");
        PF(audit&&fwrite(h,1,16,audit)==16&&!fflush(audit),"actual-original-header");}
    return rc;
}
int __real_ps3dk_prof__roll_output_accept(pfo_output *,const void *,size_t);
int __wrap_ps3dk_prof__roll_output_accept(pfo_output *o,const void *p,size_t n)
{
    int rc=__real_ps3dk_prof__roll_output_accept(o,p,n);
    if(!rc){PF(audit&&accepted_bytes+n<=1u<<20,"actual-bootstrap-bound");
        PF(fwrite(p,1,n,audit)==n&&!fflush(audit),"actual-accepted-bootstrap");++accepted_chunks;accepted_bytes+=n;}
    return rc;
}
/* The shared build recipe wraps these genuine operations. This fixture does
 * not produce AUTO events; forwarding preserves the original functions. */
int __real_ps3dk_prof__fifo_emit_chunk(prof *,uint32_t,const uint8_t *,size_t);
int __wrap_ps3dk_prof__fifo_emit_chunk(prof *p,uint32_t t,const uint8_t *b,size_t n)
{return __real_ps3dk_prof__fifo_emit_chunk(p,t,b,n);}
int __real_ps3dk_prof__auto_queue_ack(auto_queue *,uint64_t,int,int);
int __wrap_ps3dk_prof__auto_queue_ack(auto_queue *q,uint64_t n,int ready,int whole)
{return __real_ps3dk_prof__auto_queue_ack(q,n,ready,whole);}
int __real_ps3dk_prof__auto_ring_ack(auto_ring *,uint32_t,uint32_t,int);
int __wrap_ps3dk_prof__auto_ring_ack(auto_ring *r,uint32_t g,uint32_t n,int whole)
{return __real_ps3dk_prof__auto_ring_ack(r,g,n,whole);}
static void snapshot(const char *leaf)
{
    char p[1200];uint8_t b[8192];size_t n;uint64_t total=0;path_for(p,sizeof p,leaf);
    FILE *in=fopen(capture_path,"rb"),*out=fopen(p,"wbx");PF(in&&out,"snapshot-open");
    while((n=fread(b,1,sizeof b,in))){total+=n;PF(total<=16u<<20&&fwrite(b,1,n,out)==n,"snapshot-bound-write");}
    PF(!ferror(in)&&!fclose(in)&&!fclose(out),"snapshot-close");
}
int main(int argc,char **argv)
{
    ps3dk_prof_config c;ps3dk_prof_ext_config x={0};ps3dk_prof_test_rolling_options o={0};pfm2_config cfg={0};
    pfm2_layout enc;pfr_layout disk;int rc,failed_setter=0;uint32_t bound_kind=P_ROLL_NONE;char path[1200];FILE *f;
    if(argc!=3){fputs("usage: runtime-schema2-preflight CASE OUTPUT_DIRECTORY\n",stderr);return 2;}
    const char *case_name=argv[1];output_dir=argv[2];
    int disable=!strcmp(case_name,"disable"),reselect=!strcmp(case_name,"reselect-ppu"),mismatch=!strcmp(case_name,"family-mismatch");
    int preserve=!strcmp(case_name,"failed-preserves");PF(disable||reselect||mismatch||preserve||!strcmp(case_name,"baseline"),"known-case");
    ps3dk_prof_config_defaults(&c);c.mode=PS3DK_PROF_MODE_THREAD;c.max_threads=2;c.max_descriptors=8;c.ring_records=512;
    c.app="schema2-preflight";c.build_id="private-preflight";c.quiesce_timeout_us=100000;
    x.size=sizeof x;x.gpu_domains_max=1;x.gpu_report_slots_max=1024;
    o.size=sizeof o;o.version=1;o.enabled=1;o.max_state_bytes=64u<<20;o.store.max_bytes=UINT64_MAX;
    o.store.retained_slots=2;o.store.checkpoint_bytes=262144;o.store.payload_bytes=P_CHUNK_CAP+20;memset(o.store.session,0x53,16);
    cfg.shared=(pfm_config){8,8,32,128,8,8,4,32,256,16,512,128,16,128,65536,262144,UINT64_C(100000000)};
    cfg.rsx_domains=1;cfg.windows=8;cfg.fifo_membership_runs=64;cfg.accounting_groups=8;cfg.family_flags=192;
    cfg.auto_final_runs=64;cfg.auto_touched_runs=64;cfg.auto_build_entries=64;cfg.auto_clock_anchors=64;
    cfg.auto_bootstrap_bytes=32768;cfg.auto_support_bytes=65536;
    PF(pfm2_plan(&cfg,&enc)==0&&pfr_plan(&o.store,&disk)==0,"real-capacity-plans");o.store.max_bytes=disk.file_bytes;
    ps3dk_prof_test_hooks.drainer_after_snapshot=park_ready_worker;ps3dk_prof_test_hooks.write_fault=NULL;
    PF(ps3dk_prof_set_sink_path(output_dir)==0,"actual-sink-path");
    PF(ps3dk_prof_test_rolling_schema2_set(&o,&cfg)==0,"valid-prior-schema2-selection");
    if(preserve){pfm2_config invalid=cfg;invalid.family_flags=0;
        PF(pfm2_plan(&invalid,&enc)==PFR_INVALID,"independent-invalid-capacities");
        failed_setter=ps3dk_prof_test_rolling_schema2_set(&o,&invalid);
        PF(failed_setter==PS3DK_PROF_EINVAL,"failed-setter-is-EINVAL");
    }else if(disable){ps3dk_prof_test_rolling_options off={0};off.size=sizeof off;off.version=1;
        PF(ps3dk_prof_test_rolling_set(&off)==0,"generic-disable-replaces-schema2");
    }else if(reselect){pfp_layout planned;ps3dk_prof_test_rolling_options ppu=o;
        ppu.ppu=(pfp_config){8,8,32,128,65536,262144};PF(pfp_plan(&ppu.ppu,&planned)==0,"real-PPU-plan");
        PF(ps3dk_prof_test_rolling_set(&ppu)==0,"valid-PPU-reselection");
    }else if(mismatch){cfg.family_flags=128;cfg.fifo_membership_runs=0;cfg.accounting_groups=0;
        PF(pfm2_plan(&cfg,&enc)==0,"AUTO-only-config-is-plan-valid");
        PF(ps3dk_prof_test_rolling_schema2_set(&o,&cfg)==0,"AUTO-only-setter-succeeds");
    }
    PF(!LD_ACQ(&allocations)&&!append_opens&&!rolling_opens&&!encoder_inits,"setters-never-allocate-or-open");
    rc=ps3dk_prof_init_ex(&c,&x);
    if(reselect||mismatch){
        PF(rc==PS3DK_PROF_ENOTSUP,reselect?"reselected-PPU-refuses-actual-AUTO-archive":"family-mismatch-refuses-actual-archive");
        PF(!LD_ACQ(&allocations)&&!append_opens&&!rolling_opens&&!encoder_inits&&!accepted_chunks&&!capture_path[0],"preflight-refusal-before-allocation-or-FILE");
        ps3dk_prof_test_rolling_record record,sentinel;memset(&record,0xa5,sizeof record);sentinel=record;
        PF(ps3dk_prof_test_rolling_result(&record)==PS3DK_PROF_ENOTINIT&&!memcmp(&record,&sentinel,sizeof record),"refused-init-publishes-no-result");
    }else{
        PF(rc==0,"selected-core-init-succeeds");wait_ready_worker();
        bound_kind=g_prof->rolling?g_prof->rolling->selection.kind:P_ROLL_NONE;
        if(disable){PF(!rolling_opens&&append_opens==1&&!encoder_inits&&bound_kind==P_ROLL_NONE,"disable-selects-real-append");snapshot("ready.prof");}
        else{PF(rolling_opens==1&&!append_opens&&encoder_inits==1&&bound_kind==P_ROLL_SCHEMA2&&g_prof->rolling->buffers.prepare==pfm2_prepare,"prior-schema2-selection-survives-failure");
            PF(accepted_chunks==3,"real-committed-bootstrap");snapshot("ready.profr");}
    }
    if(audit){PF(!fclose(audit),"original-audit-close");audit=NULL;}
    path_for(path,sizeof path,"result.json");f=fopen(path,"wbx");PF(f,"manifest-open");
    PF(fprintf(f,"{\"schema\":1,\"case\":\"%s\",\"init_rc\":%d,\"failed_setter\":%d,\"allocations\":%" PRIu64 ",\"allocated_bytes\":%" PRIu64 ",\"append_opens\":%u,\"rolling_opens\":%u,\"encoder_inits\":%u,\"bound_kind\":%u,\"accepted_chunks\":%" PRIu64 ",\"accepted_bytes\":%" PRIu64 ",\"terminal_claim\":false}\n",case_name,rc,failed_setter,LD_ACQ(&allocations),LD_ACQ(&allocated_bytes),append_opens,rolling_opens,encoder_inits,bound_kind,accepted_chunks,accepted_bytes)>0,"manifest-write");
    PF(!fclose(f),"manifest-close");fflush(stdout);fflush(stderr);
    /* No shutdown, fake TRAILER, or final result is asserted. The process owns
     * the parked worker; these are READY/preflight observations only. */
    _Exit(0);
}
