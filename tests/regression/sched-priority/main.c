/* Regression row: PPU sched priority-range queries (t_a2644ed1).
 *
 * Two distinct error conventions are exercised, and each is checked the
 * way its API defines:
 *   - sched_get_priority_min/max()   -> return -1 and set errno;
 *   - pthread_attr_setschedpolicy()  -> RETURN the error number (errno is
 *     NOT the channel for pthread_attr_* here).
 *
 * For SCHED_OTHER both priority queries return 0 (the portable "default"
 * value; 0 means the Lv-2 default priority, NOT the highest slot).  Any
 * other policy, including an out-of-range one, returns -1 / errno EINVAL.
 *
 * The worker thread reads its own REAL Lv-2 priority back to prove the
 * pass-through: a thread that merely runs and joins is not enough.
 *
 * Include order deliberately differs from cxx_linkage.cpp: here
 * <sched.h> precedes <pthread.h> (the C TUs already in the tree, and the
 * C++ TU, use the other order) -- this proves the declarations resolve
 * under both orderings. */
#include <errno.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ppu_thread.h>

/* C++/C-linkage check (see cxx_linkage.cpp): proves the declarations are
 * visible from C++ with C linkage under the reverse include order. */
extern int sched_cxx_check(void);

static const char *fail(const char *step)
{
    if (step)
        printf("SCHED_PRIORITY_FAIL %s\n", step);
    return step;
}

/* Return the worker's real Lv-2 priority as the thread result, so the
 * caller can verify what the shim actually assigned.  -1 on the readback
 * failing, so main distinguishes it from a legitimate 0/1000. */
static void *worker(void *arg)
{
    (void)arg;
    sys_ppu_thread_t id;
    int prio;
    long v;

    if (sys_ppu_thread_get_id(&id) != 0)
        v = -1;                       /* get_id failed */
    else if (sys_ppu_thread_get_priority(id, &prio) != 0)
        v = -1;                       /* get_priority failed */
    else
        v = prio;
    return (void *)(intptr_t)v;
}

/* Create a worker with the given portable sched_priority, join it, and
 * hand back its observed Lv-2 priority via out_prio (-1 if the readback
 * failed).  Returns 0 on success, 1 if create/join itself failed. */
static int spawn_read_prio(int sched_priority, int *out_prio)
{
    pthread_attr_t attr;
    struct sched_param param;
    pthread_t th;
    void *ret;
    long v;

    if (pthread_attr_init(&attr) != 0)
        return 1;
    if (pthread_attr_setschedpolicy(&attr, SCHED_OTHER) != 0) {
        pthread_attr_destroy(&attr);
        return 1;
    }
    param.sched_priority = sched_priority;
    if (pthread_attr_setschedparam(&attr, &param) != 0) {
        pthread_attr_destroy(&attr);
        return 1;
    }
    if (pthread_create(&th, &attr, worker, NULL) != 0) {
        pthread_attr_destroy(&attr);
        return 1;
    }
    if (pthread_join(th, &ret) != 0) {
        pthread_attr_destroy(&attr);
        return 1;
    }
    pthread_attr_destroy(&attr);

    v = (long)(intptr_t)ret;
    *out_prio = (v < 0 || v > 65535) ? -1 : (int)v;
    return 0;
}

int main(void)
{
    int prio;
    const char *step = NULL;

    /* ---- sched_get_priority_* : -1/errno EINVAL for non-SCHED_OTHER;
     *      errno is the error channel; SCHED_OTHER must be 0. ---- */
    errno = 0;
    if (sched_get_priority_min(SCHED_OTHER) != 0)
        step = fail("sched_get_priority_min(SCHED_OTHER) != 0");
    errno = 0;
    if (sched_get_priority_max(SCHED_OTHER) != 0)
        step = fail("sched_get_priority_max(SCHED_OTHER) != 0");

    errno = 0;
    if (sched_get_priority_min(SCHED_FIFO) != -1 || errno != EINVAL)
        step = fail("sched_get_priority_min(SCHED_FIFO)");
    errno = 0;
    if (sched_get_priority_max(SCHED_FIFO) != -1 || errno != EINVAL)
        step = fail("sched_get_priority_max(SCHED_FIFO)");
    errno = 0;
    if (sched_get_priority_min(SCHED_RR) != -1 || errno != EINVAL)
        step = fail("sched_get_priority_min(SCHED_RR)");
    errno = 0;
    if (sched_get_priority_max(SCHED_RR) != -1 || errno != EINVAL)
        step = fail("sched_get_priority_max(SCHED_RR)");
    errno = 0;
    if (sched_get_priority_min(12345) != -1 || errno != EINVAL)
        step = fail("sched_get_priority_min(12345)");
    errno = 0;
    if (sched_get_priority_max(12345) != -1 || errno != EINVAL)
        step = fail("sched_get_priority_max(12345)");

    /* ---- pthread_attr_setschedpolicy : RETURN the error number. ---- */
    if (!step) {
        pthread_attr_t a;
        if (pthread_attr_init(&a) != 0)
            step = fail("pthread_attr_init");
        else if (pthread_attr_setschedpolicy(&a, SCHED_OTHER) != 0)
            step = fail("pthread_attr_setschedpolicy(SCHED_OTHER)");
        else if (pthread_attr_setschedpolicy(&a, SCHED_FIFO) != ENOTSUP)
            step = fail("pthread_attr_setschedpolicy(SCHED_FIFO)");
        pthread_attr_destroy(&a);
    }

    /* ---- C++/C-linkage agreement across include orders. ---- */
    if (!step && sched_cxx_check() != 0)
        step = fail("cxx_linkage/sched prototypes");

    /* ---- Case A: priority 0 (== min(SCHED_OTHER)) is the portable
     *      default; the worker must read back Lv-2 1000, not 0. ---- */
    if (!step) {
        if (spawn_read_prio(sched_get_priority_min(SCHED_OTHER), &prio) != 0 ||
            prio != 1000)
            step = fail("case A: min(SCHED_OTHER)=0 must read back lv2 1000");
    }

    /* ---- Case B (non-POSIX extension): priority 1500 -> 1500. ---- */
    if (!step) {
        if (spawn_read_prio(1500, &prio) != 0 || prio != 1500)
            step = fail("case B: extension 1500 must read back lv2 1500");
    }

    if (step)
        return 1;
    printf("SCHED_PRIORITY_OK\n");
    return 0;
}
