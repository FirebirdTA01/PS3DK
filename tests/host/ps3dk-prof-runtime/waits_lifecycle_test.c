#define _POSIX_C_SOURCE 200809L
#include "wait_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,failed,writes;
#define CHECK(x) do {++checks;if(!(x)){++failed;fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);}}while(0)
static unsigned char bytes[262144];static size_t used;static const char *mode;
static long put(void *ctx,const void *p,unsigned long n)
{
    (void)ctx;++writes;
    if(!strcmp(mode,"sink-zero"))return 0;
    if(!strcmp(mode,"sink-over"))return (long)n+1;
    if(!strcmp(mode,"sink-error") && used>=21)return -7;
    if(!strcmp(mode,"sink-error") && n>21-used)n=(unsigned long)(21-used);
    if(n>sizeof bytes-used)return -8;
    memcpy(bytes+used,p,n);used+=n;return (long)n;
}
static int close_sink(void *ctx){(void)ctx;return 0;}
static void call(void){ps3dk_prof_wait_cookie c=ps3dk_prof__wait_begin(1,9,0,0,0);CHECK(c.admitted==1);ps3dk_prof__wait_end(c,0,0);}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    mode=argv[1];
    ps3dk_prof_config cfg;ps3dk_prof_config_defaults(&cfg);cfg.app="waitlife";
    cfg.max_threads=2;cfg.max_descriptors=8;cfg.ring_records=512;cfg.quiesce_timeout_us=0;
    ps3dk_prof_wait_config w={sizeof w,6144,2,8};
    if(!strcmp(mode,"config")){
        ps3dk_prof_wait_config bad=w;bad.max_bytes=6143;CHECK(ps3dk_prof_wait_configure(&bad)==PS3DK_PROF_EINVAL);
        bad.max_actors=UINT32_MAX;bad.ring_records=UINT32_MAX;bad.max_bytes=UINT32_MAX;
        CHECK(ps3dk_prof_wait_configure(&bad)==PS3DK_PROF_EINVAL);
        bad=w;bad.ring_records=9;CHECK(ps3dk_prof_wait_configure(&bad)==PS3DK_PROF_EINVAL);
        bad=w;bad.size=0;CHECK(ps3dk_prof_wait_configure(&bad)==PS3DK_PROF_EINVAL);
        CHECK(ps3dk_prof_wait_configure(NULL)==PS3DK_PROF_EINVAL);
    }
    if(strcmp(mode,"defaults"))CHECK(ps3dk_prof_wait_configure(&w)==0);
    ps3dk_prof_sink_callbacks sink={put,NULL,close_sink,NULL};CHECK(ps3dk_prof_set_sink_callbacks(&sink)==0);
    CHECK(ps3dk_prof_init(&cfg)==0);CHECK(ps3dk_prof_wait_configure(&w)==PS3DK_PROF_EALREADY);
    CHECK(g_prof->waits->allocated==(!strcmp(mode,"defaults")?29184u:6144u));
    if(!strcmp(mode,"wrap")){
        for(unsigned i=0;i<40;++i){call();if(i%4==3)CHECK(ps3dk_prof_drain()==0);}
    }else if(!strcmp(mode,"reuse") || !strcmp(mode,"stale")){
        ps3dk_prof_wait_cookie old=ps3dk_prof__wait_begin(1,9,0,0,0);CHECK(old.admitted==1);
        if(!strcmp(mode,"reuse"))ps3dk_prof__wait_end(old,0,0);
        ps3dk_prof_thread_end();CHECK(ps3dk_prof_drain()==0);
        CHECK(ps3dk_prof_thread_begin("second")==0);CHECK(g_tls.incarnation==2);
        ps3dk_prof__wait_end(old,0,0);call();
        ps3dk_prof_thread_end();CHECK(ps3dk_prof_drain()==0);
        CHECK(ps3dk_prof_thread_begin("capacity-third")==0);CHECK(g_tls.incarnation==3);
        CHECK(ps3dk_prof__wait_begin(1,9,0,0,0).admitted==0);
    }else if(!strcmp(mode,"sequence-ceiling")){
        /* Seed a lost-record prehistory; do not pretend to execute 2^64 calls. */
        wait_actor *a=p_wait_actor(g_prof->waits,1);
        ST_RLX(&a->seq,UINT64_MAX-3);ST_RLX(&a->dropped,UINT64_MAX-3);
        ps3dk_prof_wait_cookie c=ps3dk_prof__wait_begin(1,9,0,0,0);
        CHECK(c.admitted==1);CHECK(c.call_id==UINT64_MAX-2);
        ps3dk_prof__wait_end(c,0,0);CHECK(LD_RLX(&a->seq)==UINT64_MAX-1);
        CHECK(ps3dk_prof__wait_begin(1,9,0,0,0).admitted==0);
        CHECK(LD_RLX(&a->seq)==UINT64_MAX-1);CHECK(LD_RLX(&a->head)==2);
        CHECK(LD_RLX(&a->refused)==1);
    }else if(!strcmp(mode,"cookie")){
        ps3dk_prof_wait_cookie c=ps3dk_prof__wait_begin(1,9,0,0,0),bad;
        wait_actor *a=p_wait_actor(g_prof->waits,1);CHECK(c.admitted==1);
        bad=c;bad.session++;ps3dk_prof__wait_end(bad,0,0);CHECK(LD_RLX(&a->seq)==1);
        bad=c;bad.generation++;ps3dk_prof__wait_end(bad,0,0);CHECK(LD_RLX(&a->seq)==1);
        bad=c;bad.call_id++;ps3dk_prof__wait_end(bad,0,0);CHECK(LD_RLX(&a->seq)==1);
        bad=c;bad.admitted=0;ps3dk_prof__wait_end(bad,0,0);CHECK(LD_RLX(&a->seq)==1);
        CHECK(a->active==1);CHECK(LD_RLX(&a->head)==1);
        ps3dk_prof__wait_end(c,0,0);CHECK(LD_RLX(&a->seq)==2);CHECK(a->active==0);
    }else if(!strcmp(mode,"invalid")){
        CHECK(ps3dk_prof__wait_begin(0,0,0,0,0).admitted==0);
        CHECK(ps3dk_prof__wait_begin(1,0,1,0,0).admitted==0);
        CHECK(ps3dk_prof__wait_begin(1,0,0,1,0).admitted==0);
        call();
    }else if(!strcmp(mode,"lost-return")){
        /* First endpoint is drained, leaving seven free slots after three pairs;
         * the eighth slot is a BEGIN and its RETURN is independently dropped. */
        ps3dk_prof_wait_cookie c=ps3dk_prof__wait_begin(1,9,0,0,0);CHECK(c.admitted==1);
        CHECK(ps3dk_prof_drain()==0);ps3dk_prof__wait_end(c,0,0);
        for(unsigned i=0;i<3;++i)call();
        c=ps3dk_prof__wait_begin(1,9,0,0,0);CHECK(c.admitted==1);ps3dk_prof__wait_end(c,0,0);
    }else call();
    if(!strcmp(mode,"subset-saturated")){
        /* Reachable summary shape when the bounded total-counter CAS refuses
         * but its following subset-counter increment succeeds. */
        wait_actor *a=p_wait_actor(g_prof->waits,1);
        ST_RLX(&a->refused,0);ST_RLX(&a->nested,1);ST_RLX(&a->saturated,1);
        ST_RLX(&g_prof->waits->refused,0);ST_RLX(&g_prof->waits->unregistered,1);ST_RLX(&g_prof->waits->saturated,1);
    }
    int rc=ps3dk_prof_shutdown();CHECK(rc==(!strncmp(mode,"sink-",5)?PS3DK_PROF_ESINKFAIL:0));
    unsigned nw=writes;size_t sz=used;CHECK(ps3dk_prof__wait_begin(1,9,0,0,0).admitted==0);
    CHECK(writes==nw && used==sz);
    FILE *f=fopen(argv[2],"wb");CHECK(f!=NULL);if(f){CHECK(fwrite(bytes,1,used,f)==used);CHECK(fclose(f)==0);}
    printf("checks=%u failures=%u writes=%u bytes=%zu\n",checks,failed,writes,used);return failed?1:0;
}
