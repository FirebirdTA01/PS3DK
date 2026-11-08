#define _POSIX_C_SOURCE 200809L
#include "prof_internal.h"
#include "prof_finalization.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { __atomic_fetch_add(&checks,1,__ATOMIC_RELAXED); if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static unsigned char bytes[65536];
static size_t used;
static const char *mode;
static unsigned calls, flushes, closes;
static unsigned observer_stop;
static int trailer_chunk, half_failed;
static uint32_t u32(const unsigned char *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static long put(void *ctx,const void *data,unsigned long n) {
    (void)ctx; ++calls;
    CHECK(ps3dk_prof__finalization()==NULL);
    if(!strcmp(mode,"overcount"))return (long)n+1;
    if(!strcmp(mode,"zero-limit"))return 0;
    if(!strcmp(mode,"zeros") && calls%3)return 0;
    if(!strcmp(mode,"partial")) {
        if(used>=21)return -71;
        if(n>21-used)n=(unsigned long)(21-used);
    }
    if(!strcmp(mode,"trailer-fail")) {
        if(trailer_chunk && half_failed)return -72;
        if(n>=16 && u32(data)==C_TRAILER) {trailer_chunk=1;half_failed=1;n/=2;}
    }
    if(!strcmp(mode,"short1") && n>1)n=1;
    if(!strcmp(mode,"short7") && n>7)n=7;
    CHECK(n<=sizeof bytes-used);memcpy(bytes+used,data,n);used+=n;return (long)n;
}
static int flush(void *ctx){(void)ctx;++flushes;CHECK(ps3dk_prof__finalization()==NULL);return !strcmp(mode,"flush-fail")?-73:0;}
static int close_sink(void *ctx){(void)ctx;++closes;CHECK(ps3dk_prof__finalization()==NULL);return !strcmp(mode,"close-fail")?-74:0;}
static int file_fault(unsigned long n,long *ret,int *err) {
    (void)n;CHECK(ps3dk_prof__finalization()==NULL);
    if(!calls++){*ret=-1;*err=PAL_EINTR;return 1;}return 0;
}
static void *observer(void *ctx) {
    (void)ctx;
    while(!ps3dk_prof__finalization() && !LD_ACQ(&observer_stop))sched_yield();
    const struct ps3dk_prof_finalization *f=ps3dk_prof__finalization();CHECK(f!=NULL);
    CHECK(f->io_finalized && f->trailer_written);
#ifdef PS3DK_PROF_WITH_ARCS
    CHECK(f->completed_chunks>0);
#endif
    return (void *)f;
}
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int arrived,released;
static void *worker(void *ctx) {
    (void)ctx;CHECK(ps3dk_prof_thread_begin("late") == 0);
    CHECK(pthread_mutex_lock(&mutex)==0);arrived=1;CHECK(pthread_cond_broadcast(&cond)==0);
    while(!released)CHECK(pthread_cond_wait(&cond,&mutex)==0);
    CHECK(pthread_mutex_unlock(&mutex)==0);ps3dk_prof_thread_end();return NULL;
}
static void *refused(void *ctx){(void)ctx;CHECK(ps3dk_prof_thread_begin("extra")==PS3DK_PROF_EFULL);return NULL;}
/* A shutdown the runtime refuses (APP mode, not the owner) claims nothing and publishes nothing. */
static void *foreign_shutdown(void *ctx){(void)ctx;CHECK(ps3dk_prof_shutdown()==PS3DK_PROF_EPERM);CHECK(ps3dk_prof__finalization()==NULL);return NULL;}
#ifdef PS3DK_PROF_WITH_ARCS
static void text_matches(struct ps3dk_prof_final_text t,const char *s){CHECK(t.length==strlen(s));CHECK(memcmp(t.data,s,t.length)==0);}
#endif
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    mode=argv[1];
#ifndef PS3DK_PROF_WITH_ARCS
    /* The core record (every shipped archive): no chunk metadata, so no chunk ceiling to test. */
    if(!strcmp(mode,"chunk-overflow")){fprintf(stderr,"chunk-overflow needs PS3DK_PROF_WITH_ARCS\n");return 2;}
#endif
    CHECK(ps3dk_prof__finalization()==NULL);
    ps3dk_prof_config cfg;ps3dk_prof_config_defaults(&cfg);
    cfg.max_threads=1;cfg.max_descriptors=8;cfg.ring_records=512;cfg.quiesce_timeout_us=0;
    char app[]="prefix",build[]="build",flags[]="host",tag[]="test";
    cfg.app=app;cfg.build_id=build;cfg.flags=flags;cfg.workload_tag=tag;
    if(!strcmp(mode,"thread") || !strcmp(mode,"join-fail") || !strcmp(mode,"observe"))cfg.mode=PS3DK_PROF_MODE_THREAD;
    ps3dk_prof_sink_callbacks cb={put,flush,close_sink,NULL};
    int file_mode=!strcmp(mode,"eintr");char dir[1024];
    if(file_mode) {
        CHECK(strlen(argv[2])<sizeof dir);strcpy(dir,argv[2]);char *slash=strrchr(dir,'/');CHECK(slash!=NULL);*slash=0;
        CHECK(ps3dk_prof_set_sink_path(dir)==0);ps3dk_prof_test_hooks.write_fault=file_fault;
    } else CHECK(ps3dk_prof_set_sink_callbacks(&cb)==0);
    CHECK(ps3dk_prof_init(&cfg)==0);
    memset(app,'x',sizeof app);memset(build,'x',sizeof build);memset(flags,'x',sizeof flags);memset(tag,'x',sizeof tag);
    CHECK(ps3dk_prof__finalization()==NULL);
    int length_overflow=!strcmp(mode,"length-overflow"),chunk_overflow=!strcmp(mode,"chunk-overflow");
    if(length_overflow)g_prof->prefix_bytes=UINT64_MAX-8; /* synthetic, no huge allocation */
    if(chunk_overflow)g_prof->dr.chunk_seq=UINT64_MAX;
    pthread_t reader;int observe=!strcmp(mode,"observe");
    if(observe)CHECK(pthread_create(&reader,NULL,observer,NULL)==0);
    pthread_t t;int late=!strcmp(mode,"late");
    if(late) {
        CHECK(pthread_create(&t,NULL,worker,NULL)==0);CHECK(pthread_mutex_lock(&mutex)==0);
        while(!arrived)CHECK(pthread_cond_wait(&cond,&mutex)==0);
        CHECK(pthread_mutex_unlock(&mutex)==0);
    } else {
        CHECK(ps3dk_prof_thread_begin("main")==0);
        ps3dk_prof_zone z=PS3DK_PROF_ZONE_BEGIN("test");PS3DK_PROF_ZONE_END(z);
    }
    if(!strcmp(mode,"unregistered")) {
        CHECK(pthread_create(&t,NULL,refused,NULL)==0);CHECK(pthread_join(t,NULL)==0);
    }
    if(!strcmp(mode,"refused")) {
        CHECK(pthread_create(&t,NULL,foreign_shutdown,NULL)==0);CHECK(pthread_join(t,NULL)==0);
        CHECK(ps3dk_prof__finalization()==NULL);
    }
    if(!strcmp(mode,"join-fail"))ps3dk_prof_test_hooks.join_fail=1;
    int rc=ps3dk_prof_shutdown();
    int expected=0;
    if(!strcmp(mode,"partial") || !strcmp(mode,"overcount") || !strcmp(mode,"zero-limit") ||
       !strcmp(mode,"trailer-fail") || !strcmp(mode,"flush-fail") || length_overflow || chunk_overflow)expected=PS3DK_PROF_ESINKFAIL;
    if(!strcmp(mode,"close-fail"))expected=PS3DK_PROF_ECLOSE;
    if(!strcmp(mode,"join-fail"))expected=PS3DK_PROF_ETHREAD;
    if(late)expected=PS3DK_PROF_ENOTQUIESCED;
    if(file_mode)expected=PS3DK_PROF_ESINKRECOVERED;
    CHECK(rc==expected);
    const struct ps3dk_prof_finalization *f=ps3dk_prof__finalization();CHECK(f!=NULL);
    if(file_mode) {
        FILE *in=fopen(g_prof->snk.path,"rb");CHECK(in!=NULL);used=fread(bytes,1,sizeof bytes,in);
        CHECK(!ferror(in) && feof(in));CHECK(fclose(in)==0);CHECK(g_prof->snk.error_count==1);
    }
    if(length_overflow)CHECK(f->accepted_bytes==UINT64_MAX-8 && used==0 && calls==0);
    else CHECK(f->accepted_bytes==used);
    CHECK(f->shutdown_result==rc);
    CHECK(f->io_finalized==(expected==0 || late || file_mode));CHECK(f->join_failed==(!strcmp(mode,"join-fail")));
#ifdef PS3DK_PROF_WITH_ARCS
    if(length_overflow)CHECK(f->completed_chunks==0);
    if(chunk_overflow)CHECK(f->completed_chunks==UINT64_MAX && used==16 && calls==1);
    CHECK(f->unregistered_refusals==(!strcmp(mode,"unregistered")));
    CHECK(f->abi==2 && f->consumer_mode==cfg.mode);CHECK(f->freq_hz==g_prof->tb_freq && f->tb_at_init==g_prof->tb_at_init);
    CHECK(f->session_id==f->tb_at_init);text_matches(f->app,"prefix");text_matches(f->build_id,"build");
    text_matches(f->flags,"host");text_matches(f->workload_tag,"test");text_matches(f->sdk_version,ps3dk_prof_sdk_version);
#if defined(__clang__)
    text_matches(f->compiler_identity,"clang " __clang_version__);
#else
    text_matches(f->compiler_identity,"gcc " __VERSION__);
#endif
#endif
    struct ps3dk_prof_finalization saved=*f;
    if(late) {
        CHECK(pthread_mutex_lock(&mutex)==0);released=1;CHECK(pthread_cond_broadcast(&cond)==0);
        CHECK(pthread_mutex_unlock(&mutex)==0);CHECK(pthread_join(t,NULL)==0);
    }
    CHECK(ps3dk_prof_shutdown()==PS3DK_PROF_EALREADY);CHECK(f==ps3dk_prof__finalization());CHECK(memcmp(&saved,f,sizeof saved)==0);
    if(observe) {void *observed=NULL;ST_REL(&observer_stop,1u);CHECK(pthread_join(reader,&observed)==0);CHECK(observed==f);}
    CHECK(g_prof->snk.closed && (file_mode || (closes==1 && flushes<=1)));
    FILE *out=fopen(argv[2],"wb");CHECK(out!=NULL);CHECK(fwrite(bytes,1,used,out)==used);CHECK(fclose(out)==0);
#ifdef PS3DK_PROF_WITH_ARCS
    printf("checks=%u bytes=%llu crc=%u chunks=%llu last=%llu trailer=%u io=%u unregistered=%llu synthetic=%d\n",
           checks,(unsigned long long)f->accepted_bytes,f->prefix_crc32,(unsigned long long)f->completed_chunks,
           (unsigned long long)f->last_chunk_sequence,f->trailer_written,f->io_finalized,(unsigned long long)f->unregistered_refusals,
           length_overflow || chunk_overflow);
#else
    printf("checks=%u bytes=%llu trailer=%u io=%u join=%u result=%d synthetic=%d\n",
           checks,(unsigned long long)f->accepted_bytes,f->trailer_written,f->io_finalized,f->join_failed,
           f->shutdown_result,length_overflow);
#endif
    return 0;
}
