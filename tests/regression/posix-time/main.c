#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#define CHECK(c) do { if (!(c)) { printf("POSIX_TIME_FAIL line=%d errno=%d\n", __LINE__, errno); return 1; } } while (0)
int main(void)
{
    struct timespec before, after, wall, request={0,10000000}, remaining={7,8};
    pthread_attr_t attr;
    int64_t elapsed;
    CHECK(pthread_attr_init(&attr)==0);
    CHECK(pthread_attr_setschedpolicy(&attr,SCHED_OTHER)==0);
    CHECK(pthread_attr_setschedpolicy(&attr,SCHED_FIFO)==ENOTSUP);
    CHECK(pthread_attr_setinheritsched(&attr,PTHREAD_INHERIT_SCHED)==0);
    CHECK(pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED)==ENOTSUP);
    CHECK(pthread_attr_destroy(&attr)==0);
    CHECK(clock_gettime(CLOCK_REALTIME,&wall)==0);
    CHECK(wall.tv_sec>0 && wall.tv_nsec>=0 && wall.tv_nsec<1000000000);
    CHECK(clock_gettime(CLOCK_MONOTONIC,&before)==0);
    CHECK(nanosleep(&request,&remaining)==0);
    CHECK(clock_gettime(CLOCK_MONOTONIC,&after)==0);
    elapsed=((int64_t)after.tv_sec-before.tv_sec)*INT64_C(1000000000)+after.tv_nsec-before.tv_nsec;
    printf("POSIX_TIME elapsed_ns=%lld requested_ns=10000000\n",(long long)elapsed);
    /* Quantized to microseconds; allow one tick at the measurement boundary.
     * The upper bound tolerates scheduling overhead but rejects hangs. */
    CHECK(elapsed>=9999000 && elapsed<INT64_C(2000000000));
    CHECK(remaining.tv_sec==7 && remaining.tv_nsec==8);
    request.tv_nsec=1000000000;
    errno=0;
    CHECK(nanosleep(&request,&remaining)==-1 && errno==EINVAL);
    errno=0;
    CHECK(clock_gettime((clockid_t)-1,&after)==-1 && errno==EINVAL);
    puts("POSIX_TIME_OK");
    return 0;
}
