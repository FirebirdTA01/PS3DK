/* D5: real core init/drain/shutdown, real positioned FILE output. No synthetic
 * accepted stream, Rust checkpoint input, replacement encoder or worker. */
#define _POSIX_C_SOURCE 200809L
#include "prof_internal.h"
#include "roll_runtime_test.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(x,m) do { if(!(x)){fprintf(stderr,"RUNTIME_PPU_ASSERT[%s]: %s\n",m,#x);exit(1);} } while(0)
static const char *directory;
static char capture[256];
static unsigned opens_append,opens_rw,closes,workers,callbacks;
static uint64_t allocated,clock_tick=1000000;
static FILE *original;
static uint64_t original_bytes,original_chunks;
static int capture_fd=-1,watch_worker;
static ps3dk_prof_test_rolling_options options;
static pfr_layout physical;
static pfp_layout semantic;
static ps3dk_prof_test_rolling_record result;
static ps3dk_prof_desc early={"evicted","","runtime-ppu.c",10,1,0,0};
static ps3dk_prof_desc crossing={"crossing","","runtime-ppu.c",11,1,0,0};
static ps3dk_prof_desc interior_a={"interior-a","","runtime-ppu.c",12,1,0,0};
static ps3dk_prof_desc interior_b={"interior-b","","runtime-ppu.c",13,1,0,0};
static ps3dk_prof_desc interior_c={"interior-c","","runtime-ppu.c",14,1,0,0};
static ps3dk_prof_desc window_name={"resource-window","","runtime-ppu.c",15,1,0,0};
static ps3dk_prof_desc first_zone={"accepted-before-stop","","runtime-ppu.c",16,1,0,0};
static ps3dk_prof_desc later_zone={"refused-new-descriptor","","runtime-ppu.c",17,1,0,0};

static void output_path(char *out,size_t cap,const char *name)
{int n=snprintf(out,cap,"%s/%s",directory,name);CHECK(n>=0&&(size_t)n<cap,"artifact-path");}
static uint64_t extent(const char *path)
{struct stat s;CHECK(!stat(path,&s)&&s.st_size>=0,"file-size");return (uint64_t)s.st_size;}
static void snapshot(const char *name)
{
    char out[1024];unsigned char buffer[8192];FILE *in,*to;size_t n;uint64_t total=0;
    output_path(out,sizeof out,name);in=fopen(capture,"rb");to=fopen(out,"wbx");CHECK(in&&to,"snapshot-open");
    while((n=fread(buffer,1,sizeof buffer,in))!=0){total+=n;CHECK(total<=UINT64_C(16777216),"snapshot-bound");CHECK(fwrite(buffer,1,n,to)==n,"snapshot-write");}
    CHECK(!ferror(in)&&!fclose(in)&&!fclose(to),"snapshot-close");
}
static void no_capture(void)
{
    ps3dk_prof_capture_record before,after;memset(&before,0xa5,sizeof before);after=before;
    CHECK(ps3dk_prof_get_capture_record(&after)==PS3DK_PROF_ENOTINIT&&!memcmp(&before,&after,sizeof before),"no-published-capture");
}
static void result_error(int expected)
{
    ps3dk_prof_test_rolling_record before,after;memset(&before,0xa5,sizeof before);after=before;
    CHECK(ps3dk_prof_test_rolling_result(&after)==expected&&!memcmp(&before,&after,sizeof before),"result-error-untouched");
}
static uint64_t clock_now(void){return __atomic_add_fetch(&clock_tick,10,__ATOMIC_RELAXED);}

int __real_ps3dk_prof__pal_open_excl(const char *,int *,int *);
int __real_ps3dk_prof__pal_open_rw_excl(const char *,int *,int *);
int __real_ps3dk_prof__pal_close(int,int *);
int __real_ps3dk_prof__pal_thread_create(pal_thread *,void *(*)(void *),void *,size_t);
void *__real_ps3dk_prof__pal_alloc(size_t);
int __real_ps3dk_prof__roll_ppu_init(void *,size_t,const pfp_config *,const uint8_t [16],void *,size_t,void *,size_t,size_t *);
int __real_ps3dk_prof__roll_output_accept(pfo_output *,const void *,size_t);
int __wrap_ps3dk_prof__roll_ppu_init(void *ctx,size_t cb,const pfp_config *cfg,const uint8_t head[16],void *arena,size_t ab,void *cp,size_t cap,size_t *written)
{
    int rc=__real_ps3dk_prof__roll_ppu_init(ctx,cb,cfg,head,arena,ab,cp,cap,written);
    if(rc==PFR_OK){char path[1024];CHECK(!original,"one-real-encoder-init");output_path(path,sizeof path,"accepted-original.prof");
        original=fopen(path,"wbx");CHECK(original&&fwrite(head,1,16,original)==16&&!fflush(original),"audit-actual-original-header");}
    return rc;
}
int __wrap_ps3dk_prof__roll_output_accept(pfo_output *out,const void *chunk,size_t bytes)
{
    int rc=__real_ps3dk_prof__roll_output_accept(out,chunk,bytes);
    if(rc==PFR_OK){CHECK(original&&original_bytes+bytes<=UINT64_C(4194304),"bounded-original-audit");
        CHECK(fwrite(chunk,1,bytes,original)==bytes&&!fflush(original),"audit-accepted-whole-chunk");original_bytes+=bytes;++original_chunks;}
    return rc;
}
static int remember_open(const char *path,int *fd,int rc)
{if(!rc){CHECK(strlen(path)<sizeof capture,"capture-path");strcpy(capture,path);capture_fd=*fd;}return rc;}
int __wrap_ps3dk_prof__pal_open_excl(const char *path,int *fd,int *err)
{++opens_append;return remember_open(path,fd,__real_ps3dk_prof__pal_open_excl(path,fd,err));}
int __wrap_ps3dk_prof__pal_open_rw_excl(const char *path,int *fd,int *err)
{++opens_rw;return remember_open(path,fd,__real_ps3dk_prof__pal_open_rw_excl(path,fd,err));}
int __wrap_ps3dk_prof__pal_close(int fd,int *err)
{if(fd==capture_fd)++closes;return __real_ps3dk_prof__pal_close(fd,err);}
void *__wrap_ps3dk_prof__pal_alloc(size_t n)
{void *p=__real_ps3dk_prof__pal_alloc(n);if(p){CHECK(UINT64_MAX-allocated>=n,"allocation-sum");allocated+=n;}return p;}
int __wrap_ps3dk_prof__pal_thread_create(pal_thread *t,void *(*fn)(void *),void *arg,size_t stack)
{
    ++workers;
    if(watch_worker){
        CHECK(opens_rw==1&&!opens_append&&extent(capture)==physical.file_bytes,"bootstrap-before-worker-create");
        no_capture();snapshot("bootstrap-before-worker.profr");
    }
    return __real_ps3dk_prof__pal_thread_create(t,fn,arg,stack);
}
static void config(ps3dk_prof_config *c)
{
    ps3dk_prof_config_defaults(c);c->mode=PS3DK_PROF_MODE_APP;c->max_threads=2;c->max_descriptors=16;c->ring_records=512;
    c->app="runtime-ppu";c->build_id="private-D5-test";c->workload_tag="actual-core";
    c->quiesce_timeout_us=100000;c->drain_interval_us=1000;
    memset(&options,0,sizeof options);options.size=sizeof options;options.version=1;options.enabled=1;
    options.max_state_bytes=UINT64_C(64)*1024*1024;
    options.ppu=(pfp_config){8,16,32,64,32768,65536};
    options.store.max_bytes=UINT64_MAX;options.store.retained_slots=3;
    options.store.checkpoint_bytes=options.ppu.checkpoint_bytes;options.store.payload_bytes=P_CHUNK_CAP+20;
    memset(options.store.session,0xd5,sizeof options.store.session);
    CHECK(pfp_plan(&options.ppu,&semantic)==PFR_OK,"real-semantic-plan");
    CHECK(pfr_plan(&options.store,&physical)==PFR_OK,"real-physical-plan");
    options.store.max_bytes=physical.file_bytes;
    CHECK(ps3dk_prof_set_sink_path(directory)==PS3DK_PROF_OK,"set-output-directory");
    ps3dk_prof_test_hooks.clock=clock_now;result_error(PS3DK_PROF_ENOTINIT);no_capture();
}
static void start(ps3dk_prof_config *c)
{
    ps3dk_prof_capture_record record;
    CHECK(pfp_plan(&options.ppu,&semantic)==PFR_OK,"current-semantic-plan");
    CHECK(ps3dk_prof_test_rolling_set(&options)==PS3DK_PROF_OK,"private-rolling-setter-unavailable");
    CHECK(ps3dk_prof_init(c)==PS3DK_PROF_OK,"real-runtime-rolling-init");
    CHECK(opens_rw==1&&!opens_append,"exclusive-positioned-output");
    CHECK(ps3dk_prof_get_capture_record(&record)==PS3DK_PROF_OK&&!strcmp(record.path,capture),"capture-record-real-file");
    CHECK(strlen(capture)>=6&&!strcmp(capture+strlen(capture)-6,".profr"),"rolling-suffix");
    CHECK(extent(capture)==physical.file_bytes&&allocated<=options.max_state_bytes,"whole-output-cap");
    result_error(PS3DK_PROF_EBUSY);
}
static void zone(ps3dk_prof_desc *desc)
{
    ps3dk_prof_zone z=ps3dk_prof_zone_begin(desc,PS3DK_PROF_TOKEN_NONE);
    CHECK(z.kind==ZK_PUSHED,"real-zone-admitted");ps3dk_prof_zone_end(&z);
}
static void drain_ok(void)
{CHECK(ps3dk_prof_drain()==PS3DK_PROF_OK,"real-app-drain");CHECK(extent(capture)==physical.file_bytes,"fixed-file-extent-after-drain");}
static void finished(int expected,uint32_t state,uint32_t reason)
{
    ps3dk_prof_test_rolling_record again;
    CHECK(ps3dk_prof_shutdown()==expected,"public-shutdown-result");
    CHECK(ps3dk_prof_test_rolling_result(&result)==PS3DK_PROF_OK,"public-result-published");
    CHECK(result.size==sizeof result&&result.version==1&&!result.reserved&&result.original_flags==512,"whole-original-flags");
    CHECK(result.result==expected&&result.state==state&&result.reason==reason&&!result.io_result&&!result.close_result&&!result.join_result,"typed-terminal-result");
    CHECK(result.accepted_next_chunk==result.committed_next_chunk&&result.accepted_payload_bytes==result.committed_payload_bytes,"terminal-accepted-tail-committed");
    CHECK(result.accepted_next_chunk==original_chunks&&result.accepted_payload_bytes==original_bytes,"result-matches-real-accepted-audit");
    if(expected==PS3DK_PROF_TEST_EROLLRESOURCE)CHECK(!g_prof->prefix_trailer&&!ps3dk_prof__finalization()->trailer_written,"resource-never-fabricates-trailer");
    CHECK(result.file_bytes==physical.file_bytes&&result.state_bytes>=allocated&&result.state_bytes<=options.max_state_bytes,"actual-state-budget");
    CHECK(result.state_bytes>=semantic.context_bytes+2*semantic.arena_bytes+3*(uint64_t)options.ppu.checkpoint_bytes+physical.scratch_bytes+options.store.payload_bytes,"derived-state-regions-counted");
    CHECK(closes==1&&extent(capture)==physical.file_bytes,"one-close-fixed-extent");
    CHECK(ps3dk_prof_shutdown()==PS3DK_PROF_EALREADY,"public-shutdown-once");
    CHECK(ps3dk_prof_test_rolling_result(&again)==PS3DK_PROF_OK&&!memcmp(&result,&again,sizeof result),"result-immutable");
    snapshot("capture.profr");
}
static long callback(void *ctx,const void *p,unsigned long n)
{(void)ctx;(void)p;++callbacks;return (long)n;}
static int close_callback(void *ctx){(void)ctx;++callbacks;return 0;}
#ifdef PS3DK_PROF_WITH_ARCS
static int arc_consume(void *ctx,const struct ps3dk_prof_arc_view *view)
{(void)ctx;(void)view;++callbacks;return 0;}
static int arc_finish(void *ctx,uint64_t count)
{(void)ctx;(void)count;++callbacks;return 0;}
#endif
static void manifest(const char *kind,int observed)
{
    char path[1024];FILE *f;if(original){CHECK(!fclose(original),"audit-close");original=NULL;}output_path(path,sizeof path,"result.json");f=fopen(path,"wx");CHECK(f,"manifest-create");
    CHECK(fprintf(f,"{\"schema\":1,\"case\":\"%s\",\"observed\":%d,\"open_append\":%u,\"open_rw\":%u,\"closes\":%u,\"workers\":%u,\"joins\":%u,\"callbacks\":%u,\"allocated\":%" PRIu64 ",\"file_bytes\":%" PRIu64 ",\"state_cap\":%" PRIu64 ",\"original_flags\":%u,\"state\":%u,\"reason\":%u,\"revision\":%" PRIu64 ",\"accepted_next\":%" PRIu64 ",\"accepted_bytes\":%" PRIu64 ",\"committed_next\":%" PRIu64 ",\"committed_bytes\":%" PRIu64 ",\"ppu\":[%u,%u,%u,%u,%u,%u]}\n",
        kind,observed,opens_append,opens_rw,closes,workers,ps3dk_prof_test_hooks.join_calls,callbacks,allocated,physical.file_bytes,options.max_state_bytes,
        result.original_flags,result.state,result.reason,result.revision,result.accepted_next_chunk,result.accepted_payload_bytes,result.committed_next_chunk,result.committed_payload_bytes,
        options.ppu.incarnations,options.ppu.descriptors,options.ppu.stack_rows,options.ppu.frame_identities,options.ppu.metadata_bytes,options.ppu.checkpoint_bytes)>0,"manifest-write");
    CHECK(!fclose(f),"manifest-close");
}
int main(int argc,char **argv)
{
    ps3dk_prof_config c;const char *kind;int rc;
    if(argc!=3){fputs("usage: runtime-ppu CASE EXISTING_OUTPUT_DIRECTORY\n",stderr);return 2;}
    kind=argv[1];directory=argv[2];config(&c);
    if(!strcmp(kind,"append")){
        memset(&options,0,sizeof options);options.size=sizeof options;options.version=1;
        CHECK(ps3dk_prof_test_rolling_set(&options)==0,"disabled-leaves-append");
        CHECK(ps3dk_prof_init(&c)==0,"append-init");result_error(PS3DK_PROF_ENOTSUP);
        CHECK(ps3dk_prof_thread_begin("owner")==0&&PS3DK_PROF_CAPTURE_WHOLE_RUN()==0,"append-owner-window");
        ps3dk_prof_frame();zone(&early);CHECK(ps3dk_prof_drain()==0&&ps3dk_prof_shutdown()==0,"append-drain-shutdown");
        CHECK(opens_append==1&&!opens_rw&&closes==1&&!workers,"append-original-IO");snapshot("capture.prof");manifest(kind,0);return 0;
    }
    if(!strcmp(kind,"gate-off")){
        CHECK(ps3dk_prof_test_rolling_set(&options)==PS3DK_PROF_ENOTSUP,"private-build-gate-required");
        CHECK(!opens_append&&!opens_rw&&!workers,"gate-off-no-output");no_capture();manifest(kind,PS3DK_PROF_ENOTSUP);return 0;
    }
    if(!strncmp(kind,"invalid-",8)||!strcmp(kind,"unsupported-arcs")){
        int expected=PS3DK_PROF_EINVAL;
        if(!strcmp(kind,"invalid-cap"))--options.store.max_bytes;
        else if(!strcmp(kind,"invalid-payload"))--options.store.payload_bytes;
        else if(!strcmp(kind,"invalid-checkpoint"))options.ppu.checkpoint_bytes=131;
        else if(!strcmp(kind,"invalid-state"))options.max_state_bytes=1;
        else if(!strcmp(kind,"invalid-overflow"))options.ppu.stack_rows=UINT32_MAX;
        else if(!strcmp(kind,"invalid-callback")){
            ps3dk_prof_sink_callbacks cb={callback,NULL,close_callback,NULL};expected=PS3DK_PROF_ENOTSUP;
            CHECK(ps3dk_prof_set_sink_callbacks(&cb)==0,"callback-config");
        }else if(!strcmp(kind,"unsupported-arcs")){
#ifdef PS3DK_PROF_WITH_ARCS
            struct ps3dk_prof_arc_options arcs={4,arc_consume,arc_finish,NULL};
            CHECK(ps3dk_prof__configure_arcs(&arcs)==0,"active-arc-contract");expected=PS3DK_PROF_ENOTSUP;
#else
            CHECK(0,"arc-witness-requires-arc-runtime");
#endif
        }
        else CHECK(0,"unknown-invalid-case");
        rc=ps3dk_prof_test_rolling_set(&options);if(rc==0)rc=ps3dk_prof_init(&c);
        CHECK(rc==expected,"invalid-config-specific-refusal");
        CHECK(!opens_append&&!opens_rw&&!workers&&!callbacks,"invalid-config-no-output");no_capture();result_error(PS3DK_PROF_ENOTINIT);manifest(kind,rc);return 0;
    }
    if(!strcmp(kind,"worker-failure")||!strcmp(kind,"bootstrap")){
        c.mode=PS3DK_PROF_MODE_THREAD;watch_worker=1;
        if(!strcmp(kind,"worker-failure")){
            ps3dk_prof_test_hooks.thread_create_fail=1;
            CHECK(ps3dk_prof_test_rolling_set(&options)==0,"private-rolling-setter-unavailable");
            CHECK(ps3dk_prof_init(&c)==PS3DK_PROF_ETHREAD,"worker-failure-primary");
            CHECK(workers==1&&closes==1&&!ps3dk_prof_test_hooks.join_calls,"worker-failure-close-once");
            no_capture();result_error(PS3DK_PROF_ENOTINIT);snapshot("capture.profr");
            CHECK(ps3dk_prof_init(&c)==PS3DK_PROF_EALREADY,"created-file-failure-terminal-gate");manifest(kind,PS3DK_PROF_ETHREAD);return 0;
        }
        start(&c);finished(0,PFR_SEALED,0);CHECK(workers==1&&ps3dk_prof_test_hooks.join_calls==1,"real-worker-joined");manifest(kind,0);return 0;
    }
    if(!strcmp(kind,"resource")||!strcmp(kind,"resource-shutdown"))options.ppu.descriptors=2;
    CHECK(!strcmp(kind,"resource")||!strcmp(kind,"resource-shutdown")||!strcmp(kind,"rotation"),"known-case");
    start(&c);CHECK(ps3dk_prof_thread_begin("owner")==0,"registered-owner");
    if(!strcmp(kind,"resource")||!strcmp(kind,"resource-shutdown")){
        uint64_t accepted_seq,read,written;uint32_t slot_index;
        CHECK(ps3dk_prof_capture_begin(&window_name,PS3DK_PROF_WINDOW_MANUAL,0)==0,"named-window-admitted");
        ps3dk_prof_frame();zone(&first_zone);drain_ok();snapshot("useful-prefix.profr");
        accepted_seq=g_prof->dr.chunk_seq;slot_index=g_tls.slot1-1;
        read=LD_ACQ(&g_prof->slots[slot_index].rg->read_idx);written=g_prof->dr.written[slot_index];
        CHECK(g_prof->dr.desc_emitted==2&&read>0&&written>0,"meaningful-prefix-before-resource");
        zone(&later_zone);
        if(!strcmp(kind,"resource-shutdown")){
            CHECK(!ps3dk_prof__shutdown_claimed(),"resource-first-occurs-inside-shutdown");result_error(PS3DK_PROF_EBUSY);
            finished(PS3DK_PROF_TEST_EROLLRESOURCE,PFR_STOPPED_RESOURCE,PFR_IDENTITIES);
            CHECK(g_prof->dr.desc_emitted==2&&g_prof->dr.written[slot_index]==written,"shutdown-refusal-preserves-accepted-ledgers");
            manifest(kind,PS3DK_PROF_TEST_EROLLRESOURCE);return 0;
        }
        rc=ps3dk_prof_drain();
        CHECK(rc==PS3DK_PROF_TEST_EROLLRESOURCE,"resource-is-not-sink-failure");
        CHECK(g_prof->dr.chunk_seq==accepted_seq&&g_prof->dr.desc_emitted==2&&LD_ACQ(&g_prof->slots[slot_index].rg->read_idx)==read&&g_prof->dr.written[slot_index]==written,"refused-new-descriptor-preserves-accepted-ledgers");
        CHECK(ps3dk_prof__shutdown_claimed()&&!ps3dk_prof__finalization(),"automatic-stop-not-public-finalization");
        result_error(PS3DK_PROF_EBUSY);snapshot("stopped-before-public.profr");
        finished(PS3DK_PROF_TEST_EROLLRESOURCE,PFR_STOPPED_RESOURCE,PFR_IDENTITIES);manifest(kind,rc);return 0;
    }
    CHECK(PS3DK_PROF_CAPTURE_WHOLE_RUN()==0,"whole-window");ps3dk_prof_frame();
    for(unsigned i=0;i<9;++i){zone(&early);ps3dk_prof_frame();drain_ok();}
    {
        ps3dk_prof_zone z=ps3dk_prof_zone_begin(&crossing,PS3DK_PROF_TOKEN_NONE);
        CHECK(z.kind==ZK_PUSHED,"crossing-begin");ps3dk_prof_frame();drain_ok();
        ps3dk_prof_zone_end(&z);zone(&interior_a);ps3dk_prof_frame();drain_ok();
        zone(&interior_b);zone(&interior_c);ps3dk_prof_frame();drain_ok();
    }
    finished(0,PFR_SEALED,0);CHECK(!workers&&!ps3dk_prof_test_hooks.join_calls,"app-has-no-worker");manifest(kind,0);return 0;
}
