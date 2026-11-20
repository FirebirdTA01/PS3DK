/* Reuse only test setup/audit/real PAL wrappers; the old seven cases stay frozen.
 * This target still calls the actual core worker and actual public lifecycle. */
#define main d5_initial_test_entry
#include "runtime_ppu_test.c"
#undef main
#include <errno.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t schedule_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_cv=PTHREAD_COND_INITIALIZER;
static unsigned prefix_ready,prefix_release,resource_seen,claim_ready,claim_release,public_seen,stop_wait_seen;
static unsigned claims,claim_owner,frontiers,enrollments,prefix_pause,hold_owner;
static uint32_t public_done;
static int public_rc,case_resource;
static _Thread_local unsigned pal_depth;
static unsigned writes,syncs,fail_write,fail_tail_write,fail_sync_after,inject_terminal_sync,inject_bootstrap_sync,faults;
static const char *case_name;
static prof *observed_prof; /* established by bootstrap flush before pthread_create */

static void lock_schedule(void){CHECK(!pthread_mutex_lock(&schedule_mutex),"schedule-lock");}
static void unlock_schedule(void){CHECK(!pthread_mutex_unlock(&schedule_mutex),"schedule-unlock");}
static void changed(void){CHECK(!pthread_cond_broadcast(&schedule_cv),"schedule-signal");}
static void wait_flag(unsigned *flag)
{
    struct timespec end;CHECK(!clock_gettime(CLOCK_REALTIME,&end),"schedule-clock");end.tv_sec+=10;
    lock_schedule();while(!*flag)CHECK(!pthread_cond_timedwait(&schedule_cv,&schedule_mutex,&end),"schedule-deadline");unlock_schedule();
}
static void release_flag(unsigned *flag){lock_schedule();*flag=1;changed();unlock_schedule();}
static void wait_finished(void)
{
    struct timespec start,now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&start),"phase-deadline-clock");
    /* The release/acquire phase publication establishes ordering; elapsed time
     * is solely a watchdog and never substitutes for that publication. */
    while(LD_ACQ(&g_prof->phase)!=3u){CHECK(!clock_gettime(CLOCK_MONOTONIC,&now)&&now.tv_sec-start.tv_sec<10,"phase-deadline");sched_yield();}
}

int __real_ps3dk_prof__pal_mutex_lock(pal_mutex *);
int __real_ps3dk_prof__pal_mutex_trylock(pal_mutex *);
int __real_ps3dk_prof__pal_mutex_unlock(pal_mutex *);
int __wrap_ps3dk_prof__pal_mutex_lock(pal_mutex *m)
{int rc=__real_ps3dk_prof__pal_mutex_lock(m);if(!rc)++pal_depth;return rc;}
int __wrap_ps3dk_prof__pal_mutex_trylock(pal_mutex *m)
{int rc=__real_ps3dk_prof__pal_mutex_trylock(m);if(!rc)++pal_depth;return rc;}
int __wrap_ps3dk_prof__pal_mutex_unlock(pal_mutex *m)
{int rc=__real_ps3dk_prof__pal_mutex_unlock(m);if(!rc){CHECK(pal_depth,"balanced-PAL-mutex");--pal_depth;}return rc;}

int __real_ps3dk_prof__roll_runtime_flush(prof *);
int __wrap_ps3dk_prof__roll_runtime_flush(prof *p)
{
    int rc;if(!observed_prof)observed_prof=p;
    if(inject_bootstrap_sync){CHECK(!g_prof,"bootstrap-fault-before-publication");inject_bootstrap_sync=0;fail_sync_after=1;}
    rc=__real_ps3dk_prof__roll_runtime_flush(p);
    return rc;
}
/* Size-triggered output no longer flushes on every worker pass. Observe a
 * resource refusal at accept, without parking while an emitter holds a lock. */
int __real_ps3dk_prof__roll_runtime_accept(prof *,const void *,size_t);
int __wrap_ps3dk_prof__roll_runtime_accept(prof *p,const void *data,size_t bytes)
{
    int rc=__real_ps3dk_prof__roll_runtime_accept(p,data,bytes);
    if(rc==PS3DK_PROF_TEST_EROLLRESOURCE){lock_schedule();resource_seen=1;changed();unlock_schedule();}
    return rc;
}
/* The worker's ordinary sleep is after its drain pass and all PAL locks.
 * Commit exactly the fixture's useful-prefix cut, then park the worker there.
 * Production still batches every other pass until capacity or shutdown. */
static void prefix_boundary(prof *p)
{
    if(!prefix_pause||prefix_ready||p->dr.written[0]<4||p->dr.desc_emitted<2)return;
    CHECK(!pal_depth,"outer-flush-has-no-runtime-locks");
    CHECK(__real_ps3dk_prof__roll_runtime_flush(p)==0,"explicit-lifecycle-prefix-cut");
    lock_schedule();prefix_ready=1;changed();
    while(!prefix_release)CHECK(!pthread_cond_wait(&schedule_cv,&schedule_mutex),"prefix-barrier");
    unlock_schedule();
}
static void stop_claimed(uint32_t owner)
{
    CHECK(!pal_depth,"automatic-stop-after-emitter-unlocks");
    CHECK(g_prof&&g_prof->rolling&&LD_ACQ(&g_prof->rolling->stop_prepared)==0,"claimed-before-prepared");
    lock_schedule();++claims;claim_owner=owner;claim_ready=1;changed();
    while(owner==hold_owner&&!claim_release)CHECK(!pthread_cond_wait(&schedule_cv,&schedule_mutex),"claim-barrier");
    unlock_schedule();
}
static void public_claimed(void){lock_schedule();public_seen=1;changed();unlock_schedule();}
static void frontier(void){CHECK(!pal_depth,"frontier-outside-runtime-locks");CHECK(LD_ACQ(&g_prof->rolling->stop_prepared),"frontier-requires-prepared-stop");lock_schedule();++frontiers;changed();unlock_schedule();}
static void *public_shutdown(void *unused)
{(void)unused;public_rc=ps3dk_prof_shutdown();ST_REL(&public_done,1u);return NULL;}
void __real_ps3dk_prof__pal_sleep_us(uint64_t);
void __wrap_ps3dk_prof__pal_sleep_us(uint64_t us)
{
    /* Observe the actual preparation-wait loop, not an elapsed sleep. The
     * public loser must not reach join and the resource loser must not reach
     * the frontier while the winning claim is held before preparation. */
    if(us==100&&observed_prof&&observed_prof->rolling){lock_schedule();
        if(hold_owner&&claim_ready&&LD_ACQ(&observed_prof->rolling->stop_owner)&&!LD_ACQ(&observed_prof->rolling->stop_prepared)){stop_wait_seen=1;changed();}
        unlock_schedule();}
    if(observed_prof&&g_prof==observed_prof&&g_tls.in_drain&&
       us==observed_prof->cfg.drain_interval_us)prefix_boundary(observed_prof);
    __real_ps3dk_prof__pal_sleep_us(us);
}
int __real_ps3dk_prof__pal_thread_join(pal_thread);
int __wrap_ps3dk_prof__pal_thread_join(pal_thread thread)
{CHECK(LD_ACQ(&g_prof->rolling->stop_prepared),"join-requires-prepared-stop");CHECK(!ps3dk_prof__finalization(),"join-before-publication");return __real_ps3dk_prof__pal_thread_join(thread);}

ssize_t __real_pwrite(int,const void *,size_t,off_t);
ssize_t __real_pwrite64(int,const void *,size_t,off_t);
int __real_fsync(int);
static int write_fault(int fd,size_t n,off_t off)
{
    if(fd!=capture_fd)return 0;
    ++writes;
    if(fail_tail_write){
        /* Arm at the physical boundary, before shutdown. Gate86 proved that
         * hooking pfo_finish instead moved this fault after its caller's new
         * explicit tail flush and accidentally targeted the terminal root. */
        CHECK(off>=12288&&n>0,"tail-fault-targets-first-slot-write");fail_tail_write=0;++faults;errno=EIO;return 1;
    }
    if(fail_write){fail_write=0;++faults;errno=EIO;return 1;}
    return 0;
}
ssize_t __wrap_pwrite(int fd,const void *p,size_t n,off_t off)
{if(write_fault(fd,n,off))return -1;return __real_pwrite(fd,p,n,off);}
ssize_t __wrap_pwrite64(int fd,const void *p,size_t n,off_t off)
{if(write_fault(fd,n,off))return -1;return __real_pwrite64(fd,p,n,off);}
int __wrap_fsync(int fd)
{
    int rc=__real_fsync(fd);
    if(fd==capture_fd){++syncs;if(fail_sync_after){CHECK(!rc,"real-sync-before-injected-error");fail_sync_after=0;++faults;errno=EIO;return -1;}}
    return rc;
}
int __real_ps3dk_prof__roll_publish_state(pfr_store *,const pfr_transition *);
int __wrap_ps3dk_prof__roll_publish_state(pfr_store *s,const pfr_transition *t)
{if(inject_terminal_sync){CHECK(t->state==PFR_SEALED||t->state==PFR_STOPPED_RESOURCE,"fault-targets-terminal-root");inject_terminal_sync=0;fail_sync_after=1;}return __real_ps3dk_prof__roll_publish_state(s,t);}
int __real_ps3dk_prof__pal_retain_complete_kind(const char *,const char *,uint64_t,unsigned,uint32_t,const pfi_limits *);
int __wrap_ps3dk_prof__pal_retain_complete_kind(const char *path,const char *app,uint64_t session,unsigned keep,uint32_t format,const pfi_limits *limits)
{
    ++enrollments;CHECK(closes==1,"enrollment-after-close");
    if(g_prof->cfg.mode==PS3DK_PROF_MODE_THREAD)CHECK(ps3dk_prof_test_hooks.join_calls==1,"enrollment-after-join");
    return __real_ps3dk_prof__pal_retain_complete_kind(path,app,session,keep,format,limits);
}

static void lifecycle_manifest(int rc)
{
    char path[1024];FILE *f;output_path(path,sizeof path,"lifecycle.json");f=fopen(path,"wx");CHECK(f,"lifecycle-manifest");
    CHECK(fprintf(f,"{\"claims\":%u,\"owner\":%u,\"frontiers\":%u,\"enrollments\":%u,\"writes\":%u,\"syncs\":%u,\"faults\":%u,\"result\":%d,\"io\":%d,\"close\":%d,\"join\":%d,\"reason\":%u,\"public_done\":%u,\"finalization\":%u}\n",
        claims,claim_owner,frontiers,enrollments,writes,syncs,faults,rc,result.io_result,result.close_result,result.join_result,result.reason,
        LD_ACQ(&public_done),ps3dk_prof__finalization()!=NULL)>0,"lifecycle-manifest-write");CHECK(!fclose(f),"lifecycle-manifest-close");
    manifest(case_name,rc);
}
static void setup(ps3dk_prof_config *c,int threaded,int resource)
{
    config(c);case_resource=resource;if(resource)options.ppu.descriptors=2;
    if(threaded){c->mode=PS3DK_PROF_MODE_THREAD;watch_worker=1;prefix_pause=1;}
    CHECK(ps3dk_prof_set_capture_retention(1)==0,"retention-configured-for-negative-controls");
    ps3dk_prof_test_hooks.rolling_stop_claimed=stop_claimed;
    ps3dk_prof_test_hooks.after_shutdown_claim=public_claimed;
    ps3dk_prof_test_hooks.before_frontier=frontier;
    start(c);CHECK(ps3dk_prof_thread_begin("lifecycle-owner")==0,"lifecycle-owner");
    CHECK(ps3dk_prof_capture_begin(&window_name,PS3DK_PROF_WINDOW_MANUAL,0)==0,"lifecycle-window");ps3dk_prof_frame();zone(&first_zone);
    if(threaded)wait_flag(&prefix_ready);else drain_ok();
    snapshot("known-good.profr");
}
static void check_result(int expected,int io,int close,int join)
{
    ps3dk_prof_test_rolling_record again;
    CHECK(ps3dk_prof_test_rolling_result(&result)==0,"lifecycle-result-published-after-public-shutdown");
    CHECK(result.result==expected&&result.io_result==io&&result.close_result==close&&result.join_result==join,"typed-failure-precedence");
    CHECK(result.original_flags==512&&closes==1,"whole-flags-and-close-once");
    CHECK(result.accepted_next_chunk==original_chunks&&result.accepted_payload_bytes==original_bytes,"result-accepted-audit");
    CHECK(claims==1&&frontiers==1,"one-stop-owner-one-Ft-frontier");
    CHECK(enrollments==(expected==0?1u:0u),"no-enrollment-on-failure-or-resource");
    if(case_resource)CHECK(result.reason==PFR_IDENTITIES&&!g_prof->prefix_trailer,"resource-reason-no-trailer");
    CHECK(ps3dk_prof_shutdown()==PS3DK_PROF_EALREADY,"public-claim-consumed-once");
    CHECK(ps3dk_prof_test_rolling_result(&again)==0&&!memcmp(&again,&result,sizeof result),"lifecycle-result-immutable");
    snapshot("capture.profr");lifecycle_manifest(expected);
}
static void thread_case(ps3dk_prof_config *c,int race)
{
    pthread_t public_thread;setup(c,1,1);zone(&later_zone);ps3dk_prof_thread_end();
    if(race==1){
        hold_owner=2;release_flag(&prefix_release);wait_flag(&claim_ready);
        CHECK(claim_owner==2&&!LD_ACQ(&g_prof->rolling->stop_prepared),"resource-wins-unprepared");
        CHECK(!pthread_create(&public_thread,NULL,public_shutdown,NULL),"public-thread-create");wait_flag(&public_seen);
    }else if(race==2){
        hold_owner=1;CHECK(!pthread_create(&public_thread,NULL,public_shutdown,NULL),"public-thread-create");wait_flag(&claim_ready);
        CHECK(claim_owner==1&&!LD_ACQ(&g_prof->rolling->stop_prepared),"public-wins-unprepared");
        release_flag(&prefix_release);wait_flag(&resource_seen);
    }else{
        release_flag(&prefix_release);wait_finished();
        CHECK(claim_owner==2&&!ps3dk_prof_test_hooks.join_calls&&!LD_ACQ(&public_done)&&!ps3dk_prof__finalization(),"automatic-worker-stop-preserves-public-claim");
        result_error(PS3DK_PROF_EBUSY);snapshot("automatic-before-public.profr");
        CHECK(ps3dk_prof_shutdown()==PS3DK_PROF_TEST_EROLLRESOURCE,"public-retrieves-automatic-result");ST_REL(&public_done,1u);
        CHECK(ps3dk_prof_test_hooks.join_calls==1,"first-public-shutdown-joins-worker");
        check_result(PS3DK_PROF_TEST_EROLLRESOURCE,0,0,0);return;
    }
    wait_flag(&stop_wait_seen);
    CHECK(!LD_ACQ(&public_done)&&!ps3dk_prof__finalization()&&!LD_ACQ(&g_prof->rolling->stop_prepared),"public-waits-for-stop-preparation");
    result_error(PS3DK_PROF_EBUSY);release_flag(&claim_release);
    CHECK(!pthread_join(public_thread,NULL)&&public_rc==PS3DK_PROF_TEST_EROLLRESOURCE,"racing-public-result");
    CHECK(ps3dk_prof_test_hooks.join_calls==1,"race-joins-worker-once");check_result(PS3DK_PROF_TEST_EROLLRESOURCE,0,0,0);
}
static void fault_case(ps3dk_prof_config *c)
{
    int resource=!strncmp(case_name,"resource-",9);int threaded=strcmp(case_name,"io-tail-close")&&strcmp(case_name,"io-terminal-sync");
    int close_fail=!strcmp(case_name,"io-tail-close")||!strcmp(case_name,"close-join")||!strcmp(case_name,"resource-close-join");
    int join_fail=threaded&&strcmp(case_name,"eligible-thread");int io_fail=!strncmp(case_name,"io-",3);
    int expected=io_fail?PS3DK_PROF_ESINKFAIL:close_fail?PS3DK_PROF_ECLOSE:resource?PS3DK_PROF_TEST_EROLLRESOURCE:join_fail?PS3DK_PROF_ETHREAD:0;
    setup(c,threaded,resource);
    if(resource)zone(&later_zone);else zone(&first_zone);
    ps3dk_prof_thread_end();
    if(close_fail)ps3dk_prof_test_hooks.close_fail=EIO;
    if(join_fail)ps3dk_prof_test_hooks.join_fail=1;
    if(!strcmp(case_name,"io-tail-close"))fail_tail_write=1;
    if(!strcmp(case_name,"io-terminal-sync"))inject_terminal_sync=1;
    if(threaded)release_flag(&prefix_release);
    CHECK(ps3dk_prof_shutdown()==expected,"fault-primary-result");ST_REL(&public_done,1u);
    CHECK(faults==(io_fail?1u:0u),"fault-injection-reached-real-IO");
    check_result(expected,io_fail?PFR_IO:0,close_fail?PS3DK_PROF_ECLOSE:0,join_fail?PS3DK_PROF_ETHREAD:0);
}
static void quick_observer(void)
{
    CHECK(!ps3dk_prof__finalization()&&!g_prof->prefix_trailer&&!enrollments&&!closes&&!claims&&!frontiers,"quick-exit-no-normal-finalization");
    result_error(PS3DK_PROF_EBUSY);snapshot("capture.profr");lifecycle_manifest(0);
}
int main(int argc,char **argv)
{
    ps3dk_prof_config c;if(argc!=3)return 2;case_name=argv[1];directory=argv[2];
    if(!strncmp(case_name,"init-",5)){
        config(&c);c.mode=PS3DK_PROF_MODE_THREAD;watch_worker=1;
        CHECK(ps3dk_prof_test_rolling_set(&options)==0,"init-fault-config");
        if(!strcmp(case_name,"init-format-write"))fail_write=1;
        else {CHECK(!strcmp(case_name,"init-bootstrap-sync"),"known-init-fault");inject_bootstrap_sync=1;}
        CHECK(ps3dk_prof_init(&c)==PS3DK_PROF_ESINKFAIL,"bootstrap-failure-primary");
        CHECK(faults==1&&closes==1&&!workers&&!ps3dk_prof_test_hooks.join_calls&&!enrollments,"bootstrap-failure-before-worker-close-once");
        no_capture();result_error(PS3DK_PROF_ENOTINIT);snapshot("capture.profr");
        CHECK(ps3dk_prof_init(&c)==PS3DK_PROF_EALREADY,"bootstrap-failure-terminal-init-gate");lifecycle_manifest(PS3DK_PROF_ESINKFAIL);return 0;
    }
    if(!strcmp(case_name,"thread-auto")){thread_case(&c,0);return 0;}
    if(!strcmp(case_name,"race-resource-first")){thread_case(&c,1);return 0;}
    if(!strcmp(case_name,"race-public-first")){thread_case(&c,2);return 0;}
    if(!strncmp(case_name,"quick-",6)){
        int resource=!strcmp(case_name,"quick-resource");CHECK(!at_quick_exit(quick_observer),"quick-observer-before-runtime-handler");
        setup(&c,0,resource);if(resource)zone(&later_zone);else zone(&first_zone);
        if(!strcmp(case_name,"quick-busy"))ST_REL(&g_prof->drain_busy,1u);
        quick_exit(0);
    }
    CHECK(!strcmp(case_name,"io-tail-close")||!strcmp(case_name,"io-terminal-sync")||!strcmp(case_name,"close-join")||
        !strcmp(case_name,"resource-close-join")||!strcmp(case_name,"resource-join")||!strcmp(case_name,"join-only")||!strcmp(case_name,"eligible-thread"),"known-lifecycle-case");
    fault_case(&c);return 0;
}
