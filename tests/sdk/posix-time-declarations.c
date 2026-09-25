/* Missing any of the four public declarations must fail under -Werror. */
#include <pthread.h>
#include <time.h>
#include <sched.h>
int main(void)
{
    pthread_attr_t attr;
    struct timespec ts = {0, 10000000};
    int rc = pthread_attr_init(&attr);
    rc |= pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    rc |= pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
    rc |= clock_gettime(CLOCK_REALTIME, &ts);
    rc |= nanosleep(&ts, 0);
    rc |= pthread_attr_destroy(&attr);
    return rc;
}
