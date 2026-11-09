/* Controlled LV2/resolver boundary for verbatim pthread wrapper bodies.
 * No kernel blocking, real thread scheduling or target ABI claim. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <setjmp.h>
#include <ps3dk/prof_wait_internal.h>
typedef int32_t s32;
typedef uint64_t u64;
typedef uint32_t sys_mutex_t;
typedef uint32_t sys_cond_t;
typedef uint32_t wait_mutex;
typedef uint32_t wait_cond;
static unsigned checks, failures, begins, ends, syscalls, tries, deadlines;
static int resolve_error, cond_error, deadline_error, try_result, raw_result;
static int syscall_errno, translated_errno, admitted, recurse, in_callback;
static uint32_t seen_op;
static uint64_t seen_target, seen_secondary, seen_timeout, seen_site;
static int32_t end_raw, end_api;
static char order[128];
static unsigned order_n;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"FAIL %u %s\n",__LINE__,#x); } } while(0)
static void note(char c) { if(order_n<sizeof(order)-1)order[order_n++]=c; order[order_n]=0; }
static int pt_mutex_resolve(wait_mutex *p, sys_mutex_t *out) { if(resolve_error)return resolve_error; *out=*p; return 0; }
static int pt_cond_resolve(wait_cond *p,sys_mutex_t m,sys_cond_t *out) { CHECK(m==17); if(cond_error)return cond_error; *out=*p; return 0; }
static int pt_clock_known(clockid_t c) { return c==CLOCK_REALTIME || c==CLOCK_MONOTONIC; }
static int pt_deadline_usec(clockid_t c,const struct timespec *t,u64 *out) { (void)c;(void)t;++deadlines;*out=123456;return deadline_error; }
static int lv2error(s32 r) { if(translated_errno)errno=translated_errno; return r+1000; }
static s32 sysMutexLock(sys_mutex_t id,u64 timeout) { CHECK(id==17);CHECK(timeout==0||timeout==123456);++syscalls;note('K');if(syscall_errno)errno=syscall_errno;return raw_result; }
static s32 sysCondWait(sys_cond_t id,u64 timeout) { CHECK(id==29);CHECK(timeout==0||timeout==123456);++syscalls;note('K');if(syscall_errno)errno=syscall_errno;return raw_result; }
static s32 sysMutexTryLock(sys_mutex_t id) { CHECK(id==17);++tries;return try_result; }

/* Lifecycle dependency boundary: structures supply only the fields used by
 * the real cleanup/trampoline bodies, not a host/target layout equivalence. */
struct pt_thread { int cleanup,finished,detached,used;void *retval;void *(*start)(void *);void *arg; };
struct pt_raw_start { void (*entry)(uint64_t);uint64_t arg; };
static __thread struct pt_thread *pt_self_cookie;
static __thread int pt_in_cleanup;
static jmp_buf exit_jump;
static int claim_ok=1,tsd_ok=1,abandon,table_held,life_recurse,destructor_waits;
static unsigned starts,wait_exits,old_exits;
static int tested_pthread_mutex_lock(wait_mutex *);
static void pt_thread_cleanup(struct pt_thread *,void *);
static int pt_table_lock(void) { ++table_held;return 0; }
static void pt_table_unlock(void) { --table_held; }
static int pt_cleanup_claim(struct pt_thread *self) { if(!claim_ok||self->cleanup)return 0;self->cleanup=1;return 1; }
static void __lv2_tls_dtors_run(void) { note('D');if(destructor_waits){wait_mutex m=17;CHECK(tested_pthread_mutex_lock(&m)==0);}if(abandon==1)longjmp(exit_jump,1); }
static int pt_run_tsd_destructors(struct pt_thread *self) { (void)self;note('T');if(destructor_waits){wait_mutex m=17;CHECK(tested_pthread_mutex_lock(&m)==0);}if(abandon==2)longjmp(exit_jump,1);return tsd_ok; }
static void sysThreadExit(uint64_t code) { CHECK(code==0);note('X');longjmp(exit_jump,1); }
static void probe_free(void *p) { CHECK(p!=NULL);note('F'); }
void __ps3dk_prof_thread_exit(void) __attribute__((weak));

/* @PRODUCTION_WAIT_BODIES@ */

void __ps3dk_prof_thread_exit(void) { CHECK(!table_held);++old_exits;note('O'); }
#if !defined(OMIT_LIFECYCLE)
void __ps3dk_prof_wait_thread_start(void) { CHECK(!table_held);++starts;note('S');if(life_recurse){wait_mutex m=17;CHECK(tested_pthread_mutex_lock(&m)==0);}errno=777; }
void __ps3dk_prof_wait_thread_exit(void) { CHECK(!table_held);++wait_exits;note('W');if(life_recurse){wait_mutex m=17;CHECK(tested_pthread_mutex_lock(&m)==0);}errno=888; }
#endif

#if !defined(OMIT_BEGIN)
ps3dk_prof_wait_cookie ps3dk_prof__wait_begin(uint32_t op,uint64_t target,uint64_t secondary,uint64_t timeout,uint64_t site)
{
    ++begins;note('B');seen_op=op;seen_target=target;seen_secondary=secondary;seen_timeout=timeout;seen_site=site;
    if(recurse && !in_callback){wait_mutex m=17;in_callback=1;CHECK(tested_pthread_mutex_lock(&m)==(raw_result?raw_result+1000:0));in_callback=0;}
    errno=777;
    ps3dk_prof_wait_cookie c={11,13,1,1,(uint32_t)admitted,0};return c;
}
#endif
#if !defined(OMIT_END)
void ps3dk_prof__wait_end(ps3dk_prof_wait_cookie c,int32_t raw,int32_t api)
{
    CHECK(c.session==11 && c.call_id==13 && c.admitted==1);++ends;note('E');end_raw=raw;end_api=api;
    if(recurse && !in_callback){wait_mutex m=17;in_callback=1;CHECK(tested_pthread_mutex_lock(&m)==(raw_result?raw_result+1000:0));in_callback=0;}
    errno=888;
}
#endif
static void reset(void)
{
    begins=ends=syscalls=tries=deadlines=order_n=0;order[0]=0;
    resolve_error=cond_error=deadline_error=raw_result=syscall_errno=translated_errno=recurse=in_callback=0;
    try_result=EBUSY-1000;admitted=1;seen_op=0;seen_target=seen_secondary=seen_timeout=seen_site=0;end_raw=end_api=0;errno=71;
}
/* Separate application callers discriminate a helper's return address from the
 * real caller site; neither caller is permitted to tail-call the wrapper here. */
static __attribute__((noinline)) int call_a(unsigned op)
{
    wait_mutex m=17;wait_cond c=29;struct timespec t={1,2};int r;
    switch(op){
    case 1:r=tested_pthread_mutex_lock(&m);break;
    case 2:r=tested_pthread_mutex_timedlock(&m,&t);break;
    case 3:r=tested_pthread_mutex_clocklock(&m,CLOCK_MONOTONIC,&t);break;
    case 4:r=tested_pthread_cond_wait(&c,&m);break;
    case 5:r=tested_pthread_cond_timedwait(&c,&m,&t);break;
    default:r=tested_pthread_cond_clockwait(&c,&m,CLOCK_MONOTONIC,&t);break;
    }
    __asm__ volatile("" : "+r"(r));return r;
}
static __attribute__((noinline)) int call_b(unsigned op)
{
    wait_mutex m=17;wait_cond c=29;struct timespec t={1,2};int r;
    switch(op){
    case 1:r=tested_pthread_mutex_lock(&m);break;
    case 2:r=tested_pthread_mutex_timedlock(&m,&t);break;
    case 3:r=tested_pthread_mutex_clocklock(&m,CLOCK_MONOTONIC,&t);break;
    case 4:r=tested_pthread_cond_wait(&c,&m);break;
    case 5:r=tested_pthread_cond_timedwait(&c,&m,&t);break;
    default:r=tested_pthread_cond_clockwait(&c,&m,CLOCK_MONOTONIC,&t);break;
    }
    __asm__ volatile("" : "+r"(r));return r+1;
}
static void *user_entry(void *arg) { CHECK(arg==(void *)(uintptr_t)19);CHECK(errno==71);note('U');return (void *)(uintptr_t)23; }
static void raw_entry(uint64_t arg) { CHECK(arg==UINT64_C(0x123456789abcdef0));CHECK(errno==71);note('U'); }
static void life_reset(void) { reset();starts=wait_exits=old_exits=0;claim_ok=tsd_ok=1;abandon=table_held=pt_in_cleanup=life_recurse=destructor_waits=0;pt_self_cookie=NULL; }
static void life_tests(void)
{
#if defined(PS3DK_PROF_WAIT_OBSERVE) && !defined(OMIT_LIFECYCLE)
    const unsigned observed=1;
#else
    const unsigned observed=0;
#endif
    static struct pt_thread t;
    for(volatile unsigned mode=0;mode<5;++mode){
        life_reset();memset(&t,0,sizeof t);t.start=user_entry;t.arg=(void *)(uintptr_t)19;t.used=1;
        if(mode==1||mode==2)abandon=(int)mode;
        if(mode==3)claim_ok=0;
        if(mode==4)tsd_ok=0;
        if(!setjmp(exit_jump))pt_trampoline(&t);
        CHECK(starts==observed);CHECK(errno==71);CHECK(!table_held);
        CHECK(wait_exits==(observed&&mode==0));CHECK(old_exits==(!observed&&mode==0));
        if(!mode){CHECK(!strcmp(order,observed?"SUDTWX":"UDTOX"));CHECK(t.finished==1&&t.retval==(void *)(uintptr_t)23);pt_thread_cleanup(&t,NULL);CHECK(wait_exits==observed&&old_exits==!observed);}
        else CHECK(!wait_exits&&!old_exits);
    }
    life_reset();struct pt_raw_start r={raw_entry,UINT64_C(0x123456789abcdef0)};
    if(!setjmp(exit_jump))pt_raw_trampoline(&r);
    CHECK(!strcmp(order,observed?"FSUDWX":"FUDOX"));CHECK(starts==observed);CHECK(wait_exits==observed);CHECK(old_exits==!observed);CHECK(errno==71);
    life_reset();memset(&t,0,sizeof t);t.start=user_entry;t.arg=(void *)(uintptr_t)19;life_recurse=1;
    if(!setjmp(exit_jump))pt_trampoline(&t);
    CHECK(!begins&&!ends);CHECK(syscalls==2*observed);CHECK(errno==71);
    CHECK(!strcmp(order,observed?"SKUDTWKX":"UDTOX"));
    life_reset();memset(&t,0,sizeof t);t.start=user_entry;t.arg=(void *)(uintptr_t)19;destructor_waits=1;
    if(!setjmp(exit_jump))pt_trampoline(&t);
    CHECK(syscalls==2);CHECK(errno==71);
#if defined(PS3DK_PROF_WAIT_OBSERVE) && !defined(OMIT_BEGIN) && !defined(OMIT_END)
    CHECK(begins==2&&ends==2);CHECK(strstr(order,"DBKETBKE")!=NULL);
#else
    CHECK(!begins&&!ends);CHECK(strstr(order,"DKTK")!=NULL);
#endif
    CHECK(wait_exits==observed&&old_exits==!observed);
}
int main(void)
{
#if defined(PS3DK_PROF_WAIT_OBSERVE) && !defined(OMIT_BEGIN) && !defined(OMIT_END)
    const unsigned observed=1;
#else
    const unsigned observed=0;
#endif
    for(unsigned op=1;op<=6;++op){
        for(unsigned mode=0;mode<5;++mode){
            reset();raw_result=mode==1?-73:0;syscall_errno=mode==2?91:0;translated_errno=mode==1?92:0;admitted=mode!=3;recurse=mode==4;
            int rc=call_a(op);CHECK(rc==(mode==1?927:0));CHECK(errno==(mode==1?92:mode==2?91:71));
            CHECK(syscalls==1+(observed&&recurse?2:0));CHECK(begins==observed);CHECK(ends==(observed&&admitted));
            CHECK(tries==(op==2||op==3));CHECK(deadlines==(op==2||op==3||op==5||op==6));
            if(observed){CHECK(seen_op==(op<4?(op==1?1:2):3));CHECK(seen_target==(op<4?17:29));CHECK(seen_secondary==(op<4?0:17));CHECK(seen_timeout==((op==1||op==4)?0:123456));CHECK(seen_site!=0);if(admitted){CHECK(end_raw==raw_result);CHECK(end_api==rc);}}
            if(!mode){uint64_t site=seen_site;reset();CHECK(call_b(op)==1);if(observed)CHECK(seen_site!=site);}
        }
        reset();resolve_error=31;CHECK(call_a(op)==31);CHECK(!syscalls&&!begins&&!ends);
        if(op>=4){reset();cond_error=32;CHECK(call_a(op)==32);CHECK(!syscalls&&!begins&&!ends);}
        if(op==2||op==3||op>=5){reset();deadline_error=33;CHECK(call_a(op)==33);CHECK(!syscalls&&!begins&&!ends);}
        if(op==2||op==3){reset();try_result=0;CHECK(call_a(op)==0);CHECK(!syscalls&&!begins&&!ends&&!deadlines);reset();try_result=-57;CHECK(call_a(op)==943);CHECK(!syscalls&&!begins&&!ends&&!deadlines);}
    }
    life_tests();
    printf("checks=%u failures=%u observed=%u\n",checks,failures,observed);return failures?1:0;
}
