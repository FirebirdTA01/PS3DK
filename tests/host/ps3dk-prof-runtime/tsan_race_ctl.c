/* TSan detection controls for build-and-run.sh; not a profiler test and not in rt-registry.txt.
 *
 * Built with the tsan variant's compiler, flags and options, and run through the same setarch wrapper as every
 * other TSan-instrumented executable.  Bounded: a fixed, small iteration count (and the driver's timeout).
 *
 *   default build        POSITIVE control: writer_a and writer_b update g_shared with no synchronization.  TSan
 *                        must report ONE data race whose stacks name writer_a at the line marked RACE-A and
 *                        writer_b at the line marked RACE-B, and exit with the declared TSAN_OPTIONS exitcode.
 *   -DRACE_CTL_LOCKED    NEGATIVE control: the same accesses under one mutex.  No sanitizer report, exit 0.
 *
 * The driver finds the two racing lines by their RACE-A / RACE-B markers, so each loop stays on one line. */
#include <pthread.h>
#include <stdio.h>

#define ITERS 1000

static int g_shared;

#ifdef RACE_CTL_LOCKED
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

static void *writer_a(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < ITERS; i++) { LOCK(); g_shared++; UNLOCK(); } /* RACE-A */
    return NULL;
}

static void *writer_b(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < ITERS; i++) { LOCK(); g_shared++; UNLOCK(); } /* RACE-B */
    return NULL;
}

int main(void)
{
    pthread_t a, b;
    if (pthread_create(&a, NULL, writer_a, NULL) != 0)
        return 2;
    if (pthread_create(&b, NULL, writer_b, NULL) != 0) {
        pthread_join(a, NULL);
        return 2;
    }
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("g_shared=%d\n", g_shared);   /* after both joins: ordered, not part of the race */
    return 0;
}
