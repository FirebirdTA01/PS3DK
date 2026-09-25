/* Real production code, controlled Lv-2 boundary. Rejects rounding down,
 * wall-clock monotonic time, unbounded remainders and pretend sched support. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <sys/systime.h>
int ps3test_clock_gettime(clockid_t, struct timespec *);
int ps3test_nanosleep(const struct timespec *, struct timespec *);
static uint64_t mono, real_sec, real_nsec, elapsed;
static int time_error, sleep_error, calls, interrupt_call;
static uint32_t sleeps[8];
s64 sysGetSystemTime(void) { return (s64)mono; }
s32 sysGetCurrentTime(u64 *s, u64 *n)
{ *s=real_sec; *n=real_nsec; return time_error; }
s32 sysUsleep(u32 us)
{
    assert(calls < 8); sleeps[calls++]=us;
    if (calls == interrupt_call) { mono+=elapsed; return sleep_error; }
    mono+=us; return 0;
}
int main(void)
{
    struct timespec t={99,88}, r={77,66}, q;
    pthread_attr_t attr={0};
    real_sec=1700000000; real_nsec=987654321; mono=1234567;
    assert(ps3test_clock_gettime(CLOCK_REALTIME,&t)==0);
    assert(t.tv_sec==1700000000 && t.tv_nsec==987654321);
    assert(ps3test_clock_gettime(CLOCK_MONOTONIC,&t)==0);
    assert(t.tv_sec==1 && t.tv_nsec==234567000);
    real_sec=1; mono+=1;
    assert(ps3test_clock_gettime(CLOCK_MONOTONIC,&t)==0 && t.tv_nsec==234568000);
    errno=0; assert(ps3test_clock_gettime(123,&t)==-1 && errno==EINVAL);
    assert(t.tv_sec==1 && t.tv_nsec==234568000);
    assert(ps3test_clock_gettime(CLOCK_REALTIME,0)==-1 && errno==EFAULT);
    time_error=(s32)0x8001000d;
    assert(ps3test_clock_gettime(CLOCK_REALTIME,&t)==-1 && errno==EFAULT);
    assert(t.tv_sec==1 && t.tv_nsec==234568000); time_error=0;
    real_sec=UINT64_MAX;
    assert(ps3test_clock_gettime(CLOCK_REALTIME,&t)==-1 && errno==EOVERFLOW);
    q=(struct timespec){-1,0}; assert(ps3test_nanosleep(&q,&r)==-1 && errno==EINVAL);
    q=(struct timespec){0,-1}; assert(ps3test_nanosleep(&q,&r)==-1 && errno==EINVAL);
    q=(struct timespec){0,1000000000}; assert(ps3test_nanosleep(&q,&r)==-1 && errno==EINVAL);
    assert(ps3test_nanosleep(0,&r)==-1 && errno==EFAULT);
    assert(calls==0 && r.tv_sec==77 && r.tv_nsec==66);
    q=(struct timespec){0,0}; assert(ps3test_nanosleep(&q,&r)==0 && calls==0);
    q.tv_nsec=1; assert(ps3test_nanosleep(&q,&r)==0 && sleeps[0]==1);
    assert(r.tv_sec==77 && r.tv_nsec==66);
    calls=0; q=(struct timespec){4295,999999999};
    assert(ps3test_nanosleep(&q,0)==0 && calls==2);
    assert(sleeps[0]==UINT32_MAX && sleeps[1]==1032705);
    calls=0; interrupt_call=1; sleep_error=(s32)0x8001001f; elapsed=3000;
    q=(struct timespec){1,10000123};
    assert(ps3test_nanosleep(&q,&q)==-1 && errno==EINTR);
    assert(q.tv_sec==1 && q.tv_nsec==7000123);
    calls=0; q=(struct timespec){1,100}; elapsed=1;
    assert(ps3test_nanosleep(&q,&r)==-1 && errno==EINTR);
    assert(r.tv_sec==0 && r.tv_nsec==999999100);
    calls=0; q=(struct timespec){0,1000}; elapsed=2000;
    assert(ps3test_nanosleep(&q,&r)==-1 && errno==EINTR);
    assert(r.tv_sec==0 && r.tv_nsec==0);
    calls=0; interrupt_call=2; elapsed=12345; q=(struct timespec){4295,999999999};
    assert(ps3test_nanosleep(&q,&r)==-1 && errno==EINTR);
    assert(r.tv_sec==1 && r.tv_nsec==20359999);
    calls=0; interrupt_call=1; sleep_error=(s32)0x8001000d; r=(struct timespec){7,8};
    assert(ps3test_nanosleep(&q,&r)==-1 && errno==EFAULT);
    assert(r.tv_sec==7 && r.tv_nsec==8);
    calls=0; sleep_error=(s32)0x8001001f;
    assert(ps3test_nanosleep(&q,0)==-1 && errno==EINTR);
    errno=73;
    assert(pthread_attr_setschedpolicy(0,SCHED_OTHER)==EINVAL);
    assert(pthread_attr_setinheritsched(0,PTHREAD_INHERIT_SCHED)==EINVAL);
    assert(pthread_attr_setschedpolicy(&attr,SCHED_OTHER)==EINVAL);
    attr.is_initialized=1; attr.schedpolicy=42; attr.inheritsched=43;
    assert(pthread_attr_setschedpolicy(&attr,SCHED_FIFO)==ENOTSUP && attr.schedpolicy==42);
    assert(pthread_attr_setschedpolicy(&attr,-99)==ENOTSUP && attr.schedpolicy==42);
    assert(pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED)==ENOTSUP && attr.inheritsched==43);
    assert(pthread_attr_setinheritsched(&attr,-99)==ENOTSUP && attr.inheritsched==43);
    assert(pthread_attr_setschedpolicy(&attr,SCHED_OTHER)==0 && attr.schedpolicy==SCHED_OTHER);
    assert(pthread_attr_setinheritsched(&attr,PTHREAD_INHERIT_SCHED)==0 && attr.inheritsched==PTHREAD_INHERIT_SCHED);
    assert(errno==73);
    puts("posix time boundary: PASS");
}
