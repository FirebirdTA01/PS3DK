#define _POSIX_C_SOURCE 200809L
#include "wait_internal.h"
#include <ps3dk/prof_wait.h>
#include <ps3dk/prof_wait_internal.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,failed;
#define CHECK(x) do {__atomic_fetch_add(&checks,1,__ATOMIC_RELAXED);if(!(x)){__atomic_fetch_add(&failed,1,__ATOMIC_RELAXED);fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);}}while(0)
extern void (*ps3dk_prof__wait_test_hook)(unsigned);
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int arrived,release_worker;
static unsigned selected;
static unsigned worker_done;
static unsigned stress_ready,stress_go;
static unsigned published,consume_done;
static ps3dk_prof_wait_cookie other_actor_cookie;
static unsigned counter_request,counter_done;
static _Thread_local unsigned counter_probe;
static const char *mode;
static unsigned char bytes[2097152];
static size_t used;
static long put(void *ctx,const void *p,unsigned long n){(void)ctx;if(n>sizeof bytes-used)return -1;memcpy(bytes+used,p,n);used+=n;return (long)n;}
static int close_sink(void *ctx){(void)ctx;return 0;}
static void stop_before_frontier(void)
{
    CHECK(LD_ACQ(&g_prof->stop_latch)==1);
    CHECK(LD_ACQ(&g_prof->phase)==1);
    CHECK(pthread_mutex_lock(&mu)==0);release_worker=1;
    CHECK(pthread_cond_broadcast(&cv)==0);CHECK(pthread_mutex_unlock(&mu)==0);
    while(!LD_ACQ(&worker_done))sched_yield();
    wait_actor *a=p_wait_actor(g_prof->waits,2);
    CHECK(a!=NULL);
    if(a){CHECK(LD_RLX(&a->seq)==1);CHECK(LD_RLX(&a->head)==1);CHECK(LD_ACQ(&a->ack)==1);}
}
static void hook(unsigned point)
{
    if(!strcmp(mode,"counter-retry") && point==40 && counter_probe){
        CHECK(pthread_mutex_lock(&mu)==0);
        unsigned request=++counter_request;CHECK(request<=4);
        if(request<=4){
            CHECK(pthread_cond_broadcast(&cv)==0);
            while(counter_done<request)CHECK(pthread_cond_wait(&cv,&mu)==0);
        }
        CHECK(pthread_mutex_unlock(&mu)==0);return;
    }
    if(!strcmp(mode,"publication") && point==12){
        /* Deliberately relaxed test rendezvous: only the production ring-head
         * release/acquire may order the payload read in the ordinary drain. */
        ST_RLX(&published,1);while(!LD_RLX(&consume_done))sched_yield();return;
    }
    if(!strcmp(mode,"stress") && (point==12 || point==22)){sched_yield();return;}
    if(point!=selected)return;
    CHECK(pthread_mutex_lock(&mu)==0);arrived=1;CHECK(pthread_cond_broadcast(&cv)==0);
    while(!release_worker)CHECK(pthread_cond_wait(&cv,&mu)==0);
    CHECK(pthread_mutex_unlock(&mu)==0);
}
static void *counter_competitor(void *arg)
{
    (void)arg;
    for(unsigned i=0;i<4;++i){
        CHECK(pthread_mutex_lock(&mu)==0);
        while(counter_request<=i)CHECK(pthread_cond_wait(&cv,&mu)==0);
        CHECK(pthread_mutex_unlock(&mu)==0);
        CHECK(ps3dk_prof__wait_begin(1,77,0,0,0).admitted==0);
        CHECK(pthread_mutex_lock(&mu)==0);++counter_done;
        CHECK(pthread_cond_broadcast(&cv)==0);CHECK(pthread_mutex_unlock(&mu)==0);
    }
    return NULL;
}
static void *worker(void *arg)
{
    (void)arg;
    if(!strcmp(mode,"preexisting"))hook(30);
    if(!strcmp(mode,"auto")){
        ps3dk_prof_zone z=PS3DK_PROF_ZONE_BEGIN("registration-trigger");PS3DK_PROF_ZONE_END(z);
        CHECK(g_tls.incarnation!=0);
    }else if(strcmp(mode,"unregistered") && strcmp(mode,"counter-retry"))CHECK(ps3dk_prof_thread_begin("existing-worker")==0);
    counter_probe=!strcmp(mode,"counter-retry");
    if(!strcmp(mode,"stress")){
        ST_REL(&stress_ready,1);while(!LD_ACQ(&stress_go))sched_yield();
        for(unsigned i=0;i<2000;++i){
            ps3dk_prof_wait_cookie c=ps3dk_prof__wait_begin(3,77,88,i,0);CHECK(c.admitted==1);
            ps3dk_prof__wait_end(c,-3,3);
        }
        ps3dk_prof_thread_end();ST_REL(&worker_done,1);return NULL;
    }
    ps3dk_prof_wait_cookie c=ps3dk_prof__wait_begin(1,77,0,0,0);
    CHECK(c.admitted==((!strcmp(mode,"unregistered") || !strcmp(mode,"capacity") || counter_probe)?0u:1u));
    if(!strcmp(mode,"cross-actor")){
        /* Both actors have live call 1 and generation 1; identity is the
         * discriminator, not a missing actor, inactive slot or stale sequence. */
        CHECK(c.call_id==other_actor_cookie.call_id);
        CHECK(c.generation==other_actor_cookie.generation);
        ps3dk_prof__wait_end(other_actor_cookie,0,0);
        CHECK(LD_RLX(&p_wait_actor(g_prof->waits,1)->seq)==1);
        CHECK(LD_RLX(&p_wait_actor(g_prof->waits,2)->seq)==1);
    }
    if(selected==30 && strcmp(mode,"preexisting"))hook(30);
    ps3dk_prof__wait_end(c,0,0);ps3dk_prof_thread_end();ST_REL(&worker_done,1);return NULL;
}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    mode=argv[1];
    if(!strncmp(mode,"stop-",5))selected=(unsigned)strtoul(mode+5,NULL,10);
    if(!strcmp(mode,"stop-live")){
        selected=30;ps3dk_prof_test_hooks.before_frontier=stop_before_frontier;
    }
    ps3dk_prof_config cfg;ps3dk_prof_config_defaults(&cfg);cfg.app="waitrace";
    cfg.max_threads=2;cfg.max_descriptors=8;cfg.ring_records=512;cfg.quiesce_timeout_us=0;cfg.auto_register=1;
    ps3dk_prof_wait_config w={sizeof w,1048576,!strcmp(mode,"capacity")?1:2,8};
    CHECK(ps3dk_prof_wait_configure(&w)==0);
    ps3dk_prof_sink_callbacks sink={put,NULL,close_sink,NULL};CHECK(ps3dk_prof_set_sink_callbacks(&sink)==0);
    ps3dk_prof__wait_test_hook=hook;
    pthread_t t,competitor;
    int preexisting=!strcmp(mode,"preexisting");
    if(preexisting){
        selected=30;CHECK(pthread_create(&t,NULL,worker,NULL)==0);
        CHECK(pthread_mutex_lock(&mu)==0);while(!arrived)CHECK(pthread_cond_wait(&cv,&mu)==0);CHECK(pthread_mutex_unlock(&mu)==0);
    }
    CHECK(ps3dk_prof_init(&cfg)==0);
    if(!strcmp(mode,"counter-retry"))CHECK(pthread_create(&competitor,NULL,counter_competitor,NULL)==0);
    if(!strcmp(mode,"cross-actor")){
        other_actor_cookie=ps3dk_prof__wait_begin(1,99,0,0,0);
        CHECK(other_actor_cookie.admitted==1);
    }
    if(preexisting){
        CHECK(pthread_mutex_lock(&mu)==0);release_worker=1;CHECK(pthread_cond_broadcast(&cv)==0);CHECK(pthread_mutex_unlock(&mu)==0);
    }else CHECK(pthread_create(&t,NULL,worker,NULL)==0);
    if(!strcmp(mode,"stress")){
        while(!LD_ACQ(&stress_ready))sched_yield();
        CHECK(ps3dk_prof_drain()==0);ST_REL(&stress_go,1);
        while(!LD_ACQ(&worker_done)){CHECK(ps3dk_prof_drain()==0);sched_yield();}
    }
    if(!strcmp(mode,"publication")){
        while(!LD_RLX(&published))sched_yield();
        CHECK(ps3dk_prof_drain()==0);
        /* Prove the ordinary drainer consumed the BEGIN before allowing the
         * producer to return, retire or supply any other publication edge. */
        wait_actor *a=p_wait_actor(g_prof->waits,2);
        CHECK(a!=NULL);
        if(a){CHECK(a->written==1);CHECK(a->last==1);CHECK(LD_RLX(&a->tail)==1);}
        ST_RLX(&consume_done,1);
    }
    if(selected && !preexisting){
        CHECK(pthread_mutex_lock(&mu)==0);while(!arrived)CHECK(pthread_cond_wait(&cv,&mu)==0);
        CHECK(pthread_mutex_unlock(&mu)==0);
        CHECK(ps3dk_prof_shutdown()==(!strcmp(mode,"stop-live")?0:PS3DK_PROF_ENOTQUIESCED));
        size_t frozen=used;
        CHECK(pthread_mutex_lock(&mu)==0);release_worker=1;CHECK(pthread_cond_broadcast(&cv)==0);CHECK(pthread_mutex_unlock(&mu)==0);
        CHECK(pthread_join(t,NULL)==0);CHECK(used==frozen);
    }else{
        CHECK(pthread_join(t,NULL)==0);
        if(!strcmp(mode,"counter-retry")){
            CHECK(pthread_join(competitor,NULL)==0);CHECK(counter_request==4);CHECK(counter_done==4);
            CHECK(LD_RLX(&g_prof->waits->refused)==4);
            CHECK(LD_RLX(&g_prof->waits->unregistered)==4);
            CHECK(LD_RLX(&g_prof->waits->saturated)==1);
        }
        if(!strcmp(mode,"cross-actor"))ps3dk_prof__wait_end(other_actor_cookie,0,0);
        CHECK(ps3dk_prof_shutdown()==0);
    }
    FILE *f=fopen(argv[2],"wb");CHECK(f!=NULL);if(f){CHECK(fwrite(bytes,1,used,f)==used);CHECK(fclose(f)==0);}
    printf("checks=%u failures=%u bytes=%zu\n",checks,failed,used);return failed?1:0;
}
