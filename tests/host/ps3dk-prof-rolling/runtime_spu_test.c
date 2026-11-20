/* D6: actual core FILE output and actual SPU producer/drainer. Reuse the
 * unchanged I2 port and allocation wrappers, not a second memory model. */
#define main d6_unused_i2_main
#include "../ps3dk-prof-spu-int/i2_test.c"
#undef main
#include "runtime_spu_test_api.h"
#include "prof_identity.h"
#include <inttypes.h>
#include <sys/stat.h>

#define D6(x,m) do{if(!(x)){fprintf(stderr,"RUNTIME_SPU_ASSERT[%s]: %s\n",m,#x);exit(1);}}while(0)
const ps3dk_prof__identity_t ps3dk_prof__identity={PS3DK_PROF_IDENTITY_MARKER,{0x5a},1689,2,7,"D6-HOST"};
static prod d6_producers[2];
static ps3dk_prof_test_rolling_options d6_options;
static pfm_config d6_config;
static pfr_layout d6_layout;
static char d6_capture[1024];
static FILE *d6_audit;
static uint64_t d6_tick=1000000,d6_bytes,d6_chunks,d6_alloc_ready;
static uint64_t d6_refused_next,d6_refused_bytes;
static unsigned d6_refused_type,d6_refusals,d6_rw,d6_append;
static int d6_enabled;
static const ps3dk_prof_spu_desc d6_descs[]={{0x100,10,"spu-work","runtime-spu.c"}};
static uint64_t d6_clock(void){return d6_tick+=10;}
static void d6_path(char *out,size_t cap,const char *name)
{int n=snprintf(out,cap,"%s/%s",g_out,name);D6(n>=0&&(size_t)n<cap,"artifact-path");}
static void d6_copy(const char *name)
{
    char path[1200];unsigned char b[8192];size_t n;uint64_t total=0;FILE *in,*out;
    int count=atomic_exchange(&g_counting,0);d6_path(path,sizeof path,name);
    in=fopen(d6_capture,"rb");out=fopen(path,"wbx");D6(in&&out,"snapshot-open");
    while((n=fread(b,1,sizeof b,in))!=0){total+=n;D6(total<=16u*1024u*1024u,"snapshot-bound");D6(fwrite(b,1,n,out)==n,"snapshot-write");}
    D6(!ferror(in)&&!fclose(in)&&!fclose(out),"snapshot-close");atomic_store(&g_counting,count);
}
int __real_ps3dk_prof__pal_open_excl(const char *,int *,int *);
int __real_ps3dk_prof__pal_open_rw_excl(const char *,int *,int *);
int __wrap_ps3dk_prof__pal_open_excl(const char *path,int *fd,int *err)
{int rc=__real_ps3dk_prof__pal_open_excl(path,fd,err);if(!rc){++d6_append;D6(strlen(path)<sizeof d6_capture,"capture-path");strcpy(d6_capture,path);}return rc;}
int __wrap_ps3dk_prof__pal_open_rw_excl(const char *path,int *fd,int *err)
{int rc=__real_ps3dk_prof__pal_open_rw_excl(path,fd,err);if(!rc){++d6_rw;D6(strlen(path)<sizeof d6_capture,"capture-path");strcpy(d6_capture,path);}return rc;}
int __real_ps3dk_prof__roll_combined_init(void *,size_t,const pfm_config *,const uint8_t[16],void *,size_t,void *,size_t,size_t *);
int __wrap_ps3dk_prof__roll_combined_init(void *ctx,size_t cb,const pfm_config *cfg,const uint8_t h[16],void *a,size_t ab,void *cp,size_t cap,size_t *n)
{
    int rc=__real_ps3dk_prof__roll_combined_init(ctx,cb,cfg,h,a,ab,cp,cap,n);
    if(!rc){char path[1200];d6_path(path,sizeof path,"accepted-original.prof");D6(!d6_audit,"one-combined-init");
        d6_audit=fopen(path,"wbx");D6(d6_audit&&fwrite(h,1,16,d6_audit)==16&&!fflush(d6_audit),"audit-original-header");}
    return rc;
}
int __real_ps3dk_prof__roll_output_accept(pfo_output *,const void *,size_t);
int __wrap_ps3dk_prof__roll_output_accept(pfo_output *o,const void *raw,size_t n)
{
    int rc=__real_ps3dk_prof__roll_output_accept(o,raw,n);
    if(!rc){D6(d6_audit&&d6_bytes+n<=4u*1024u*1024u,"accepted-audit-bound");
        D6(fwrite(raw,1,n,d6_audit)==n&&!fflush(d6_audit),"accepted-audit-write");d6_bytes+=n;++d6_chunks;}
    else if(rc==PFO_RESOURCE){const uint8_t *b=raw;++d6_refusals;d6_refused_type=(unsigned)b[0]|(unsigned)b[1]<<8|(unsigned)b[2]<<16|(unsigned)b[3]<<24;
        d6_refused_next=d6_chunks;d6_refused_bytes=d6_bytes;}
    return rc;
}
static void d6_setup(int enabled,int resource)
{
    ps3dk_prof_config c;ps3dk_prof_ext_config x={0};pfm_layout semantic;
    ps3dk_prof_config_defaults(&c);c.mode=PS3DK_PROF_MODE_APP;c.max_threads=2;c.max_descriptors=8;c.ring_records=512;
    c.app="runtime-spu";c.build_id="private-D6-test";c.workload_tag="actual-spu-core";c.quiesce_timeout_us=100000;
    x.size=sizeof x;x.spu_lanes_max=2;x.spu_ring_slots=64;x.spu_image_bytes_max=65536;x.spu_mem_max=16u<<20;
    d6_options.size=sizeof d6_options;d6_options.version=1;d6_options.enabled=(uint32_t)enabled;
    d6_options.max_state_bytes=64u<<20;d6_options.store=(pfr_config){0};
    d6_options.store.max_bytes=UINT64_MAX;d6_options.store.retained_slots=6;
    d6_options.store.checkpoint_bytes=131072;d6_options.store.payload_bytes=P_CHUNK_CAP+20;
    memset(d6_options.store.session,0xd6,16);
    d6_config=(pfm_config){8,8,32,64,8,8,4,32,256,16,512,128,resource?1u:16u,128,32768,131072,UINT64_C(10000000)};
    D6(pfm_plan(&d6_config,&semantic)==0,"combined-capacities");D6(pfr_plan(&d6_options.store,&d6_layout)==0,"physical-capacities");
    d6_options.store.max_bytes=d6_layout.file_bytes;
    ps3dk_prof_test_hooks.clock=d6_clock;D6(ps3dk_prof_set_sink_path(g_out)==0,"sink-path");
    if(enabled){ps3dk_prof_test_rolling_options conflicting=d6_options;conflicting.ppu.metadata_bytes=1;
        D6(ps3dk_prof_test_rolling_combined_set(&conflicting,&d6_config)==PS3DK_PROF_EINVAL,"conflicting-capacities-refused");
        D6(ps3dk_prof_test_rolling_combined_set(&d6_options,NULL)==PS3DK_PROF_EINVAL,"missing-capacities-refused");}
    D6(ps3dk_prof_test_rolling_combined_set(&d6_options,&d6_config)==0,"private-combined-setter-unavailable");
    D6(ps3dk_prof_init_ex(&c,&x)==0,"real-combined-init");d6_enabled=enabled;
    D6(ps3dk_prof_test_rolling_combined_set(&d6_options,&d6_config)==PS3DK_PROF_EALREADY,"setter-after-init-refused");
    D6(d6_rw==(unsigned)enabled&&d6_append==(unsigned)!enabled,"actual-FILE-mode");
    if(enabled){D6(d6_chunks==3,"bootstrap-three-real-chunks");d6_copy("ready.profr");}
    d6_alloc_ready=(uint64_t)atomic_load(&g_allocs);atomic_store(&g_counting,1);
    D6(ps3dk_prof_thread_begin("owner")==0&&PS3DK_PROF_CAPTURE_WHOLE_RUN()==0,"owner-window");
}
static void d6_lane(unsigned i,uint32_t image)
{prod *p=&d6_producers[i];D6(ps3dk_prof_spu_lane_create(i?"interior":"crossing",image,1,&p->lane)==0,"real-lane-create");D6(prod_init(p,0)==PSP_OK,"real-producer-init-observe-only");}
static void d6_drain(void)
{
    D6(ps3dk_prof_drain()==0,"actual-app-drain");
    /* Fixture cuts deliberately rotate/censor spans. Real APP drain batches
     * until byte capacity; explicitly commit these private test boundaries. */
    if(d6_enabled)D6(p_roll_flush(g_prof)==0,"explicit-spu-fixture-cut");
}
static void d6_flush(prod *p){D6(psp_spu_flush(&p->s)==PSP_OK,"actual-producer-flush");}
static void d6_end(prod *p)
{ps3dk_prof_spu_lane_stats st;D6(psp_spu_fini(&p->s)==PSP_OK,"writer-fini");
    D6(ps3dk_prof_spu_lane_end(p->lane.handle,100000,&st)==0,"lane-end-after-fini");
    D6(st.final_written&&st.events==p->ntruth&&!st.lost,"accepted-lane-final-truth");}
static void d6_write(const char *kind,int resource,int expected)
{
    ps3dk_prof_test_rolling_record record;char path[1200];FILE *f;int rc=ps3dk_prof_shutdown();
    D6(rc==expected,"actual-public-shutdown");
    D6((uint64_t)atomic_load(&g_allocs)==d6_alloc_ready,"no-post-READY-allocation");atomic_store(&g_counting,0);
    memset(&record,0,sizeof record);
    if(d6_enabled){D6(ps3dk_prof_test_rolling_result(&record)==0,"same-private-result");
        D6(record.original_flags==519&&record.result==expected,"whole-original-flags-and-result");
        D6(record.accepted_next_chunk==d6_chunks&&record.committed_next_chunk==d6_chunks&&record.accepted_payload_bytes==d6_bytes&&record.committed_payload_bytes==d6_bytes,"accepted-frontiers");
        D6(record.state==(resource?PFR_STOPPED_RESOURCE:PFR_SEALED),"terminal-physical-state");
        D6(!record.io_result&&!record.close_result&&!record.join_result,"terminal-errors");
        D6(record.state_bytes<=d6_options.max_state_bytes,"whole-state-cap");d6_copy("capture.profr");}
    else d6_copy("capture.prof");
    if(d6_audit){D6(!fclose(d6_audit),"audit-close");d6_audit=NULL;}
    d6_path(path,sizeof path,"truth.txt");f=fopen(path,"wbx");D6(f,"truth-open");
    for(unsigned i=0;i<2;++i){prod *p=&d6_producers[i];for(uint64_t q=0;q<p->ntruth;++q){tev *t=&p->truth[q];
        D6(fprintf(f,"%u %" PRIu64 " %u %u %u %u\n",p->lane.handle,q,t->dec,t->desc,t->kind,t->depth)>0,"truth-row");}}
    D6(!fclose(f),"truth-close");d6_path(path,sizeof path,"result.json");f=fopen(path,"wbx");D6(f,"manifest-open");
    D6(fprintf(f,"{\"schema\":1,\"case\":\"%s\",\"flags\":519,\"state\":%u,\"reason\":%u,\"next\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"revision\":%" PRIu64 ",\"refusals\":%u,\"refused_type\":%u,\"refused_next\":%" PRIu64 ",\"refused_bytes\":%" PRIu64 ",\"post_ready_allocations\":0,\"events\":[%" PRIu64 ",%" PRIu64 "]}\n",
        kind,record.state,record.reason,record.accepted_next_chunk,record.accepted_payload_bytes,record.revision,d6_refusals,d6_refused_type,d6_refused_next,d6_refused_bytes,d6_producers[0].ntruth,d6_producers[1].ntruth)>0,"manifest-write");D6(!fclose(f),"manifest-close");
}
/* The public lane-end API is a third APP consumer boundary. Keep the first
 * accepted BEGIN separate; the second must be consumed only by lane_end. */
static void d6_inline_resource(int append)
{
    prod *a=&d6_producers[0];ps3dk_prof_spu_lane_stats before,after;
    ps3dk_prof_test_rolling_record pending,sentinel,published,repeated;
    ev(a,1);d6_flush(a);d6_drain();
    D6(ps3dk_prof_spu_lane_get_stats(a->lane.handle,&before)==0&&
       before.events==1&&before.next_seq==1&&!before.lost,"inline-accepted-first-BEGIN");
    ev(a,1);
    D6(psp_spu_fini(&a->s)==PSP_OK,"inline-producer-fini-before-lane-end");
    D6(ps3dk_prof_spu_lane_end(a->lane.handle,100000,&after)==0,"inline-lane-end-releases-with-OK");
    /* lane_end has freed control/ring memory: only returned stats and local
     * producer truth may be inspected from here onward. */
    if(!append){
        D6(LD_ACQ(&g_prof->phase)==3u&&LD_ACQ(&g_prof->rolling->stop_prepared)==1u,
           "inline-resource-automatic-finish");
        D6(LD_ACQ(&g_prof->rolling->stop_owner)==2u&&!LD_ACQ(&g_prof->shutdown_once)&&
           !ps3dk_prof__finalization(),"inline-resource-preserves-public-shutdown");
        D6(!g_tls.in_drain&&!LD_ACQ(&g_prof->drain_busy),"inline-resource-releases-consumer-ownership");
        D6(!after.final_written&&!after.fin_state&&after.events==before.events&&
           after.next_seq==before.next_seq&&after.lost==before.lost,"inline-resource-before-event-ledger-ack");
        D6(d6_refusals==1&&d6_refused_type==13&&d6_refused_next==d6_chunks&&
           d6_refused_bytes==d6_bytes,"inline-resource-refused-original-frontier");
        D6(!g_prof->prefix_trailer,"inline-resource-no-fake-trailer");
        memset(&pending,0xa5,sizeof pending);sentinel=pending;
        D6(ps3dk_prof_test_rolling_result(&pending)==PS3DK_PROF_EBUSY&&
           !memcmp(&pending,&sentinel,sizeof pending),"inline-resource-result-unpublished");
        D6((uint64_t)atomic_load(&g_allocs)==d6_alloc_ready,"inline-resource-no-post-READY-allocation");
        d6_copy("automatic-before-public.profr");
    }else{
        D6(after.final_written&&after.fin_state==PSP_FIN_FINALIZED&&after.events==2&&
           after.next_seq==2&&!after.lost,"inline-append-final-accepted-truth");
    }
    D6(after.have_post_join&&after.post_state==PSP_ST_CLOSED&&after.post_next_seq==2&&
       !after.post_older_lost&&!after.post_refused,"inline-returned-producer-closed-truth");
    D6(!g_bad,"unchanged-I2-port-checks");
    d6_write(g_case,!append,append?0:PS3DK_PROF_TEST_EROLLRESOURCE);
    if(!append){
        D6(ps3dk_prof_test_rolling_result(&published)==0,"inline-first-public-result");
        D6(ps3dk_prof_shutdown()==PS3DK_PROF_EALREADY,"inline-second-public-shutdown");
        D6(ps3dk_prof_test_rolling_result(&repeated)==0&&!memcmp(&published,&repeated,sizeof repeated),
           "inline-public-result-immutable");
    }
}
#ifndef RUNTIME_SPU_ENTRY
#define RUNTIME_SPU_ENTRY main
#endif
int RUNTIME_SPU_ENTRY(int argc,char **argv)
{
    uint32_t image;int append,resource,crossing,inline_resource;prod *a=&d6_producers[0],*b=&d6_producers[1];
    if(argc!=3){fputs("usage: runtime-spu CASE OUTPUT_DIRECTORY\n",stderr);return 2;}
    g_case=argv[1];g_out=argv[2];append=!strncmp(g_case,"append-",7);
    const char *recipe=append?g_case+7:g_case;inline_resource=!strcmp(recipe,"inline-resource");
    resource=!strcmp(recipe,"resource")||inline_resource;crossing=!strcmp(recipe,"crossing");
    D6(resource||crossing||!strcmp(recipe,"rotation"),"known-case");d6_setup(!append,resource);
    D6(ps3dk_prof_spu_image_register_table((const uint8_t *)"\x02" "D6-image",9,d6_descs,1,&image)==0,"real-image-table");
    d6_lane(0,image);if(!resource)d6_lane(1,image);d6_drain();
    if(inline_resource){d6_inline_resource(append);return 0;}
    if(resource){
        ev(a,1);d6_flush(a);d6_drain();
        uint64_t read=psp_rd64(at(a->lane.ea_ctl)+PSP_A_READ_IDX);ps3dk_prof_spu_lane_stats before,after;
        D6(ps3dk_prof_spu_lane_get_stats(a->lane.handle,&before)==0&&before.events==1,"accepted-before-resource");
        ev(a,1);d6_flush(a);
        if(!append){D6(ps3dk_prof_drain()==PS3DK_PROF_TEST_EROLLRESOURCE,"SPU-resource-not-sink-failure");
            D6(d6_refusals==1&&d6_refused_type==13,"real-SPU-event-refused");
            D6(psp_rd64(at(a->lane.ea_ctl)+PSP_A_READ_IDX)==read,"resource-before-read-ack");
            D6(ps3dk_prof_spu_lane_get_stats(a->lane.handle,&after)==0,"accepted-stats-after-resource");
            D6(before.next_seq==after.next_seq&&before.events==after.events&&before.lost==after.lost,"resource-before-event-ledger-ack");
            D6(psp_spu_fini(&a->s)==PSP_OK,"resource-producer-quiescence");d6_write(g_case,1,PS3DK_PROF_TEST_EROLLRESOURCE);return 0;}
        d6_drain();ev(a,0);ev(a,0);d6_flush(a);d6_drain();d6_end(a);
    }else{
        if(crossing){ev(a,1);d6_flush(a);d6_drain();}
        for(unsigned i=0;i<10;++i){
            if(!crossing){ev(a,1);ev(a,0);d6_flush(a);}
            ev(b,1);ev(b,0);d6_flush(b);ps3dk_prof_frame();d6_drain();
        }
        if(crossing){ev(a,0);d6_flush(a);d6_drain();}d6_end(a);d6_end(b);
    }
    D6(!g_bad,"unchanged-I2-port-checks");d6_write(g_case,0,0);return 0;
}
