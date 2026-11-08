#define _POSIX_C_SOURCE 200809L
#include "prof_internal.h"
#include "../hooks/arc_runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { __atomic_fetch_add(&checks,1,__ATOMIC_RELAXED); if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
/* Each scenario runs in a new process because the real runtime is one-shot. */
static unsigned char bytes[65536];
static size_t used;
static int sink_failure, consume_failure, finish_failure, probe_capacity;
static unsigned nviews, nfinish;
static uint64_t final_unreg;
static struct ps3dk_prof_arc_view views[32];
static struct ps3prof_arc_row saved_rows[32][4];
static char saved_names[32][64];
static long put(void *ctx, const void *data, unsigned long size) {
    (void)ctx;
    if (sink_failure || size > sizeof bytes - used) return -1;
    memcpy(bytes + used, data, size); used += size; return (long)size;
}
static int close_sink(void *ctx) { (void)ctx; return 0; }
static void *try_register(void *ctx) {
    int *rc = ctx; *rc = ps3dk_prof_thread_begin("capacity-probe"); return NULL;
}
static int consume(void *ctx, const struct ps3dk_prof_arc_view *v) {
    (void)ctx;
    CHECK(nviews < 32 && v->thread.row_count <= 4);
    CHECK(ps3dk_prof__record_arc(4, 8, 0) == PS3DK_PROF_EBUSY);
    CHECK(ps3dk_prof_drain() == PS3DK_PROF_EBUSY);
    if (probe_capacity) {
        pthread_t probe; int rc = 99;
        CHECK(pthread_create(&probe, NULL, try_register, &rc) == 0);
        CHECK(pthread_join(probe, NULL) == 0);
        CHECK(rc == PS3DK_PROF_EFULL);
    }
    views[nviews] = *v;
    snprintf(saved_names[nviews], sizeof saved_names[nviews], "%s", v->thread.name);
    views[nviews].thread.name = saved_names[nviews];
    if (v->thread.row_count) memcpy(saved_rows[nviews], v->thread.rows, v->thread.row_count * sizeof *v->thread.rows);
    views[nviews].thread.rows = saved_rows[nviews];
    ++nviews;
    return consume_failure ? -71 : 0;
}
static int finish(void *ctx, uint64_t unreg) {
    (void)ctx; ++nfinish; final_unreg = unreg;
    return finish_failure ? -72 : 0;
}
static struct ps3dk_prof_arc_options options = {4, consume, finish, NULL};
static void start(unsigned slots, unsigned mode, int enable) {
    ps3dk_prof_config cfg;
    ps3dk_prof_config_defaults(&cfg);
    cfg.max_threads=slots; cfg.max_descriptors=8; cfg.ring_records=512;
    cfg.mode=mode; cfg.auto_register=1; cfg.quiesce_timeout_us=0;
    ps3dk_prof_sink_callbacks cb = {put, NULL, close_sink, NULL};
    CHECK(ps3dk_prof_set_sink_callbacks(&cb) == 0);
    CHECK(ps3dk_prof__configure_arcs(enable ? &options : NULL) == 0);
    CHECK(ps3dk_prof_init(&cfg) == 0);
    CHECK(ps3dk_prof__configure_arcs(&options) == PS3DK_PROF_EALREADY);
}
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int arrived, released, wait_worker, pause_retire, stop_on_resume;
static pthread_t stopped_worker;
static _Thread_local int worker_thread;
static void barrier(void) {
    CHECK(pthread_mutex_lock(&mutex) == 0);
    arrived=1; CHECK(pthread_cond_broadcast(&cond) == 0);
    while (!released) CHECK(pthread_cond_wait(&cond,&mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
}
static void before_retire(void) { if(worker_thread && pause_retire) barrier(); }
static void register_at_frontier(void) {
    pthread_t t; int rc=99;
    CHECK(pthread_create(&t,NULL,try_register,&rc)==0);
    CHECK(pthread_join(t,NULL)==0); CHECK(rc==PS3DK_PROF_ESTOPPED);
}
static void await_worker(void) {
    CHECK(pthread_mutex_lock(&mutex) == 0);
    while (!arrived) CHECK(pthread_cond_wait(&cond,&mutex) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
}
static void release_worker(void) {
    CHECK(pthread_mutex_lock(&mutex) == 0);
    released=1; CHECK(pthread_cond_broadcast(&cond) == 0);
    CHECK(pthread_mutex_unlock(&mutex) == 0);
}
static void *worker(void *arg) {
    worker_thread=1;
    CHECK(ps3dk_prof_thread_begin("worker") == 0);
    for (unsigned i=0;i<10;++i) {
        CHECK(ps3dk_prof__record_arc(4,8,0) == 0);
        CHECK(ps3dk_prof__record_arc(8,12,0) == 0);
        CHECK(ps3dk_prof__record_arc(8,12,0) == 0);
    }
    if(wait_worker) barrier();
    if(stop_on_resume) CHECK(ps3dk_prof__record_arc(4,8,0)==PS3DK_PROF_ESTOPPED);
    if(arg == NULL) ps3dk_prof_thread_end(); /* non-NULL simulates raw exit */
    if(arg == NULL) CHECK(ps3dk_prof__record_arc(4,8,0) == PS3DK_PROF_ESTOPPED);
    return NULL;
}
static void *stale_worker(void *arg) {
    worker(arg); barrier();
    CHECK(ps3dk_prof__record_arc(4,8,0)==PS3DK_PROF_ESTOPPED);
    return NULL;
}
static void acknowledge_at_frontier(void) {
    release_worker(); CHECK(pthread_join(stopped_worker,NULL)==0);
}
static void counts(unsigned i) {
    CHECK(views[i].thread.recorded == 30 && views[i].thread.attempted == 30);
    CHECK(views[i].thread.row_count == 2 && saved_rows[i][0].count == 10 && saved_rows[i][1].count == 20);
}
static int write_arcs(void *ctx, const void *data, size_t n) {
    return fwrite(data,1,n,ctx)==n ? 0 : -1;
}
static void serialize_views(const char *path) {
    /* These host-only independent arc files test the delivered data against
     * the real serializer/reader. They are NOT the later paired exporter. */
    char file[1024]; struct ps3prof_arc_thread ts[32];
    struct ps3prof_arc_snapshot s={0};
    CHECK(snprintf(file,sizeof file,"%s.arcs",path)>0);
    for(unsigned i=0;i<nviews;++i) ts[i]=views[i].thread;
    s.app="lifecycle"; s.build_id=(const unsigned char *)"test"; s.build_id_len=4;
    s.sdk_version="test"; s.compiler_identity="host"; s.flags=""; s.workload_tag="";
    s.abi=1; s.mode=1; s.snapshot_flags=1; s.session_id=1; s.snapshot_id=1; s.timing_chunk_seq=UINT64_MAX;
    s.max_threads=g_prof->nslots; s.max_arcs_per_thread=options.max_arcs_per_thread;
    s.freq_hz=1000000; s.threads=ts; s.thread_count=nviews;
    FILE *f=fopen(file,"wb"); CHECK(f!=NULL);
    CHECK(ps3prof_arcs_write(&s,write_arcs,f)==0); CHECK(fclose(f)==0);
}
int main(int argc,char **argv) {
    if(argc != 3) return 2;
    const char *name=argv[1];
    int wanted=0;
    if(!strcmp(name,"config")) {
        ST_REL(&g_gate,G_CONFIGURING);
        CHECK(ps3dk_prof__configure_arcs(&options)==PS3DK_PROF_EBUSY);
        ST_REL(&g_gate,G_UNINIT);
        struct ps3dk_prof_arc_options bad=options;
        bad.max_arcs_per_thread=0; CHECK(ps3dk_prof__configure_arcs(&bad) == PS3DK_PROF_EINVAL);
        bad=options; bad.consume=NULL; CHECK(ps3dk_prof__configure_arcs(&bad) == PS3DK_PROF_EINVAL);
        bad=options; bad.finish=NULL; CHECK(ps3dk_prof__configure_arcs(&bad) == PS3DK_PROF_EINVAL);
        bad=options; bad.max_arcs_per_thread=65537; CHECK(ps3dk_prof__configure_arcs(&bad) == PS3DK_PROF_EINVAL);
        start(1,PS3DK_PROF_MODE_APP,0);
        CHECK(ps3dk_prof__record_arc(4,8,0) == PS3DK_PROF_ENOTINIT);
        CHECK(ps3dk_prof_shutdown()==0); CHECK(nviews==0 && nfinish==0);
    } else if(!strcmp(name,"init-fail")) {
        ps3dk_prof_config cfg; ps3dk_prof_config_defaults(&cfg);
        cfg.max_threads=1024; cfg.ring_records=512;
        struct ps3dk_prof_arc_options big=options; big.max_arcs_per_thread=65536;
        CHECK(ps3dk_prof__configure_arcs(&big)==0);
        CHECK(ps3dk_prof_init(&cfg)==PS3DK_PROF_EINVAL);
        CHECK(g_prof==NULL && LD_ACQ(&g_gate)==G_UNINIT);
        cfg.max_threads=1;
        ps3dk_prof_test_hooks.alloc_fail=1;
        CHECK(ps3dk_prof_init(&cfg)==PS3DK_PROF_ENOMEM);
        CHECK(g_prof==NULL && LD_ACQ(&g_gate)==G_UNINIT);
        ps3dk_prof_test_hooks.alloc_fail=0;
        start(1,PS3DK_PROF_MODE_APP,1);
        CHECK(ps3dk_prof_shutdown()==0); CHECK(nviews==0 && nfinish==1);
    } else if(!strcmp(name,"limit") || !strcmp(name,"refused")) {
        options.max_arcs_per_thread=1; start(1,PS3DK_PROF_MODE_APP,1);
        CHECK(ps3dk_prof__record_arc(4,8,0)==0);
        if(!strcmp(name,"limit")) {
            /* Synthetic near-MAX state, preserving both collector equations. */
            struct ps3dk_prof_arc_collector *a=&g_prof->slots[0].arcs;
            a->attempted=a->recorded=a->rows[0].count=UINT64_MAX-1;
            CHECK(ps3dk_prof__record_arc(4,8,0)==0);
            CHECK(ps3dk_prof__record_arc(4,8,0)==PS3DK_PROF_ARC_LIMIT);
        } else CHECK(ps3dk_prof__record_arc(8,12,0)==PS3DK_PROF_ARC_REFUSED);
        CHECK(ps3dk_prof_shutdown()==0); CHECK(nviews==1);
        if(!strcmp(name,"limit")) {
            CHECK(views[0].thread.limit_flags==1 && views[0].thread.recorded==UINT64_MAX);
            CHECK(saved_rows[0][0].count==UINT64_MAX && views[0].thread.attempted==UINT64_MAX);
        } else CHECK(views[0].thread.attempted==2 && views[0].thread.recorded==1 && views[0].thread.refused==1);
    } else if(!strcmp(name,"stale-tls")) {
        start(1,PS3DK_PROF_MODE_APP,1);
        pthread_t old, fresh;
        CHECK(pthread_create(&old,NULL,stale_worker,NULL)==0); await_worker();
        CHECK(ps3dk_prof_drain()==0); CHECK(nviews==1);
        CHECK(pthread_create(&fresh,NULL,worker,NULL)==0); CHECK(pthread_join(fresh,NULL)==0);
        CHECK(ps3dk_prof_drain()==0); CHECK(nviews==2);
        release_worker(); CHECK(pthread_join(old,NULL)==0);
        counts(0); counts(1); CHECK(views[1].generation==2);
        CHECK(ps3dk_prof_shutdown()==0); CHECK(nviews==2 && nfinish==1);
    } else if(!strcmp(name,"stop-owner")) {
        start(1,PS3DK_PROF_MODE_APP,1); wait_worker=1; stop_on_resume=1;
        CHECK(pthread_create(&stopped_worker,NULL,worker,NULL)==0); await_worker();
        ps3dk_prof_test_hooks.before_frontier=acknowledge_at_frontier;
        CHECK(ps3dk_prof_shutdown()==0); CHECK(nviews==1 && views[0].thread.state==ST_ACKED);
        counts(0); CHECK(nfinish==1 && final_unreg==0);
    } else if(!strcmp(name,"reuse")) {
        start(1,PS3DK_PROF_MODE_APP,1); probe_capacity=1;
        for(unsigned i=0;i<4;++i) {
            pthread_t t; CHECK(pthread_create(&t,NULL,worker,NULL)==0); CHECK(pthread_join(t,NULL)==0);
            CHECK(ps3dk_prof_drain()==0); CHECK(nviews==i+1);
            counts(i); CHECK(views[i].generation==i+1 && views[i].thread.incarnation==i+1 && views[i].thread.slot==0);
        }
        CHECK(ps3dk_prof_shutdown()==0); CHECK(nviews==4 && nfinish==1 && final_unreg==4);
        for(unsigned i=0;i<4;++i) counts(i);
    } else if(!strcmp(name,"ack") || !strcmp(name,"thread") || !strcmp(name,"stop-registration")) {
        start(1,!strcmp(name,"thread")?PS3DK_PROF_MODE_THREAD:PS3DK_PROF_MODE_APP,1);
        if(!strcmp(name,"stop-registration")) ps3dk_prof_test_hooks.before_frontier=register_at_frontier;
        CHECK(ps3dk_prof__record_arc(4,8,0)==0);
        CHECK(ps3dk_prof_shutdown()==0); CHECK(nviews==1 && views[0].thread.state==ST_ACKED);
        CHECK(views[0].thread.recorded==1 && nfinish==1 && final_unreg==0);
        CHECK(ps3dk_prof__record_arc(4,8,0)==PS3DK_PROF_ESTOPPED);
    } else if(!strcmp(name,"paused-retire") || !strcmp(name,"late") || !strcmp(name,"raw")) {
        start(1,PS3DK_PROF_MODE_APP,1); pthread_t t;
        pause_retire=strcmp(name,"raw") != 0;
        ps3dk_prof_test_hooks.before_retire_store=before_retire;
        CHECK(pthread_create(&t,NULL,worker,!strcmp(name,"raw")?(void *)1:NULL)==0);
        if(pause_retire) {await_worker(); CHECK(ps3dk_prof_drain()==0); CHECK(nviews==0);}
        if(!strcmp(name,"paused-retire")) {
            release_worker(); CHECK(pthread_join(t,NULL)==0);
            CHECK(ps3dk_prof_drain()==0); CHECK(nviews==1); counts(0);
            CHECK(ps3dk_prof_shutdown()==0); CHECK(nviews==1 && nfinish==1);
        } else {
            if(!pause_retire) CHECK(pthread_join(t,NULL)==0);
            CHECK(ps3dk_prof_shutdown()==PS3DK_PROF_ENOTQUIESCED); wanted=1;
            CHECK(nviews==1 && views[0].thread.state==ST_NOT_QUIESCED);
            CHECK(views[0].thread.row_count==0 && views[0].thread.limit_flags==2);
            CHECK(views[0].thread.attempted==UINT64_MAX && views[0].thread.recorded==UINT64_MAX);
            CHECK(views[0].thread.refused==UINT64_MAX && views[0].thread.lost==UINT64_MAX);
            size_t before=used;
            if(pause_retire) {release_worker();CHECK(pthread_join(t,NULL)==0);}
            CHECK(used==before && nviews==1 && nfinish==1);
        }
    } else if(!strcmp(name,"consumer-fail") || !strcmp(name,"finish-fail") || !strcmp(name,"sink-fail")) {
        start(1,PS3DK_PROF_MODE_APP,1);
        consume_failure=!strcmp(name,"consumer-fail"); finish_failure=!strcmp(name,"finish-fail"); sink_failure=!strcmp(name,"sink-fail");
        pthread_t t; CHECK(pthread_create(&t,NULL,worker,NULL)==0); CHECK(pthread_join(t,NULL)==0);
        if(!finish_failure) {
            CHECK(ps3dk_prof_drain()==PS3DK_PROF_ESINKFAIL);
            unsigned before=nviews; CHECK(ps3dk_prof_drain()==PS3DK_PROF_ESINKFAIL); CHECK(nviews==before);
            int rc; CHECK(pthread_create(&t,NULL,try_register,&rc)==0); CHECK(pthread_join(t,NULL)==0); CHECK(rc==PS3DK_PROF_EFULL);
        }
        CHECK(ps3dk_prof_shutdown()==PS3DK_PROF_ESINKFAIL); wanted=1;
    } else if(!strcmp(name,"os-id")) {
        start(1,PS3DK_PROF_MODE_APP,1);
        ps3dk_prof_test_hooks.os_id_fail=1;
        CHECK(ps3dk_prof__record_arc(4,8,0)==PS3DK_PROF_ETHREAD);
        ps3dk_prof_test_hooks.os_id_fail=0;
        CHECK(ps3dk_prof_shutdown()==PS3DK_PROF_ESINKFAIL); CHECK(nviews==0 && nfinish==0); wanted=1;
        CHECK(LD_RLX(&g_prof->unregistered_refusals)==0);
        CHECK(g_prof->slots[0].state==SLOT_FREE && g_prof->slots[0].generation==0);
        CHECK(g_prof->next_incarnation==1 && g_tls.slot1==0);
    } else return 2;
    FILE *f=fopen(argv[2],"wb"); CHECK(f!=NULL); CHECK(fwrite(bytes,1,used,f)==used); CHECK(fclose(f)==0);
    if(nfinish && !finish_failure) serialize_views(argv[2]);
    printf("lifecycle checks=%u EXPECT=%d unregistered=%llu\n",checks,wanted,(unsigned long long)final_unreg);
    return 0;
}
