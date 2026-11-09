#define _POSIX_C_SOURCE 200809L
#include "prof_internal.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PS3DK_PROF_WITH_WAITS
#include <ps3dk/prof_wait.h>
#include <ps3dk/prof_wait_internal.h>
#else
/* The absent-feature baseline still runs the real core and sink. */
typedef struct { uint32_t size,max_bytes,max_actors,ring_records; } ps3dk_prof_wait_config;
typedef struct { uint64_t session,call_id; uint32_t actor,generation,admitted,reserved; } ps3dk_prof_wait_cookie;
static int ps3dk_prof_wait_configure(const ps3dk_prof_wait_config *c) {(void)c;return 0;}
static ps3dk_prof_wait_cookie ps3dk_prof__wait_begin(uint32_t op,uint64_t a,uint64_t b,uint64_t t,uint64_t s) {
    (void)op;(void)a;(void)b;(void)t;(void)s;ps3dk_prof_wait_cookie c={0};return c;
}
static void ps3dk_prof__wait_end(ps3dk_prof_wait_cookie c,int32_t raw,int32_t api) {(void)c;(void)raw;(void)api;}
#endif
static unsigned checks,failed;
#define CHECK(x) do {++checks;if(!(x)){++failed;fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}}while(0)
static unsigned char bytes[262144];
static size_t used;
static const char *mode;
static int observe_pal;
static pthread_mutex_t sink_mutex=PTHREAD_MUTEX_INITIALIZER;
int __real_pthread_mutex_lock(pthread_mutex_t *m);
int __wrap_pthread_mutex_lock(pthread_mutex_t *m) {
    if(!observe_pal)return __real_pthread_mutex_lock(m);
    ps3dk_prof_wait_cookie c=ps3dk_prof__wait_begin(1,(uint64_t)(uintptr_t)m,0,0,99);
    int rc=__real_pthread_mutex_lock(m);
    if(c.admitted)ps3dk_prof__wait_end(c,rc,rc);
    return rc;
}
static int close_sink(void *ctx){(void)ctx;return 0;}
static long put(void *ctx,const void *p,unsigned long n) {
    (void)ctx;
    if(observe_pal){CHECK(pthread_mutex_lock(&sink_mutex)==0);CHECK(pthread_mutex_unlock(&sink_mutex)==0);}
    if(!strcmp(mode,"short") && n>7)n=7;
    if(n>sizeof bytes-used)return -1;
    memcpy(bytes+used,p,n);used+=n;return (long)n;
}
static uint32_t u32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t u64(const unsigned char *p){return u32(p)|((uint64_t)u32(p+4)<<32);}
static void one(void) {
    ps3dk_prof_wait_cookie c=ps3dk_prof__wait_begin(3,41,42,1234,0);
    CHECK(c.admitted==1);ps3dk_prof__wait_end(c,-17,17);
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    mode=argv[1];
    observe_pal=!strcmp(mode,"internal");
    ps3dk_prof_config cfg;ps3dk_prof_config_defaults(&cfg);
    cfg.app="waits";cfg.max_threads=2;cfg.max_descriptors=8;cfg.ring_records=512;cfg.quiesce_timeout_us=0;
    ps3dk_prof_wait_config wc={sizeof wc,1048576,2,8};
    CHECK(ps3dk_prof_wait_configure(&wc)==0);
    ps3dk_prof_sink_callbacks sink={put,NULL,close_sink,NULL};CHECK(ps3dk_prof_set_sink_callbacks(&sink)==0);
    CHECK(ps3dk_prof_init(&cfg)==0);
    int rc=ps3dk_prof_thread_begin("main");CHECK(rc==0 || rc==PS3DK_PROF_EALREADY);
    ps3dk_prof_wait_cookie held={0};
    if(!strcmp(mode,"internal")) {
        pal_mutex m;CHECK(pal_mutex_init(&m)==0);
        g_tls.wait_busy=1;
        CHECK(pal_mutex_lock(&m)==0);CHECK(pal_mutex_unlock(&m)==0);CHECK(pal_mutex_destroy(&m)==0);
        CHECK(g_tls.wait_busy==1);g_tls.wait_busy=0;
        one();CHECK(ps3dk_prof_drain()==0);
    }
    else if(!strcmp(mode,"normal") || !strcmp(mode,"short"))one();
    else if(!strcmp(mode,"open")) {held=ps3dk_prof__wait_begin(1,41,0,0,99);CHECK(held.admitted==1);}
    else if(!strcmp(mode,"nested")) {
        held=ps3dk_prof__wait_begin(1,41,0,0,0);CHECK(held.admitted==1);
        ps3dk_prof_wait_cookie c=ps3dk_prof__wait_begin(1,42,0,0,0);CHECK(c.admitted==0);
        ps3dk_prof__wait_end(c,0,0);ps3dk_prof__wait_end(held,0,0);
    } else if(!strcmp(mode,"lost-begin")) {
        for(unsigned i=0;i<4;++i)one();
        held=ps3dk_prof__wait_begin(1,41,0,0,0);CHECK(held.admitted==1);
        CHECK(ps3dk_prof_drain()==0);ps3dk_prof__wait_end(held,0,0);
    } else if(!strcmp(mode,"both-lost")) {
        for(unsigned i=0;i<3;++i)one();
        held=ps3dk_prof__wait_begin(1,41,0,0,0);CHECK(held.admitted==1);
        ps3dk_prof__wait_end(held,0,0);
        held=ps3dk_prof__wait_begin(1,41,0,0,0);CHECK(held.admitted==1);
        ps3dk_prof__wait_end(held,0,0);
    } else return 2;
    CHECK(ps3dk_prof_shutdown()==0);
    size_t before=used;ps3dk_prof__wait_end(held,0,0);CHECK(used==before);
    CHECK(used>=16 && (bytes[12]&16));
    unsigned configs=0,records=0,statuses=0;uint64_t last=0,dropped=0,attempted=0,nested=0;
    for(size_t at=16;at+20<=used;) {
        unsigned kind=u32(bytes+at),len=u32(bytes+at+4);CHECK(at+20+len<=used);
        if(at+20+len>used)break;
        const unsigned char *p=bytes+at+16;
        if(kind==22){++configs;CHECK(len==56);CHECK(u32(p+16)==6144);}
        if(kind==23){CHECK(configs==1);unsigned count=u32(p+4);CHECK(len==8+96*count);
            for(unsigned i=0;i<count;++i){const unsigned char *r=p+8+96*i;++records;last=u64(r+8);CHECK(u32(r)==1);}
        }
        if(kind==24){++statuses;CHECK(len>=136);const unsigned char *a=p+72;CHECK(u32(a)==1);
            if(observe_pal){CHECK(u64(p+8+40)>=3);CHECK(u64(p+8+48)==0);CHECK(u64(a+40)==0);}
            CHECK(u64(a+16)==records);CHECK(u64(a+8)==last);dropped=u64(a+24);nested=u64(a+40);attempted=u64(a+56);
        }
        at+=20+len;
    }
    CHECK(configs==1 && statuses==1);
    if(!strcmp(mode,"open")){CHECK(records==1);CHECK(attempted==1);}
    else if(!strcmp(mode,"lost-begin")){CHECK(records==9);CHECK(last==10);CHECK(dropped==1);CHECK(attempted==10);}
    else if(!strcmp(mode,"both-lost")){CHECK(records==8);CHECK(dropped==2);CHECK(attempted==10);}
    else {CHECK(records==2);CHECK(attempted==2);CHECK(dropped==0);}
    if(!strcmp(mode,"nested"))CHECK(nested==1);
    FILE *f=fopen(argv[2],"wb");CHECK(f!=NULL);if(f){CHECK(fwrite(bytes,1,used,f)==used);CHECK(fclose(f)==0);}
    printf("checks=%u failures=%u records=%u dropped=%llu attempted=%llu\n",checks,failed,records,(unsigned long long)dropped,(unsigned long long)attempted);
    return failed?1:0;
}
