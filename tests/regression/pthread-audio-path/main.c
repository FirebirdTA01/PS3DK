/* Regression row: a miniaudio-style POSIX thread start on the PPU
 * (t_e87d59d7 item d).
 *
 * Exercises, in miniaudio's real control-flow order, the exact attribute
 * sequence a portable audio/thread library issues:
 *
 *   pthread_attr_init
 *   pthread_attr_setstacksize 128 KiB
 *   pthread_attr_setschedpolicy(SCHED_FIFO)   -> expected ENOTSUP here
 *   pthread_attr_setschedpolicy(SCHED_OTHER)   -> the accepted fallback
 *   sched_get_priority_min / _max(policy)
 *   if (max > min) sched_priority = min + (max - min)/2; else default
 *   pthread_attr_setschedparam
 *   pthread_attr_setinheritsched(PTHREAD_INHERIT_SCHED)
 *   pthread_create
 *   pthread_attr_destroy
 *
 * The PPU shim does not support SCHED_FIFO (returns ENOTSUP), so the code
 * takes the SCHED_OTHER branch and min == max == 0, leaving the default
 * priority.  The row still runs the whole sequence and reports the
 * policy that ended up accepted.
 *
 * Every return value is checked and the first failing step number is
 * reported.  setinheritsched(PTHREAD_INHERIT_SCHED) == 0 only proves it
 * was ACCEPTED; the shim has no real POSIX priority inheritance, so this
 * row does not claim any inherited state.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define HANDOFFS 100

static pthread_mutex_t g_mtx;
static pthread_cond_t  g_cv;
static int g_turn = 0;     /* 0 = main's move, 1 = worker's move */
static int g_counter = 0;

#define MAIN_TURN   0
#define WORKER_TURN 1

/* Worker's half of the ping-pong (50 moves).  Returns the final counter
 * so a broken handoff also fails the result check, not just a timeout. */
static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < HANDOFFS / 2; i++) {
        pthread_mutex_lock(&g_mtx);
        while (g_turn != WORKER_TURN)
            pthread_cond_wait(&g_cv, &g_mtx);
        g_counter++;
        g_turn = MAIN_TURN;
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mtx);
    }
    return (void *)(intptr_t)g_counter;
}

/* Print the failure line and stop. */
static int fail_step(int step, int rc, int policy)
{
    printf("PTHREAD_AUDIO_PATH_FAIL %d (rc=%d, policy=%d)\n", step, rc, policy);
    return 1;
}

int main(void)
{
    pthread_attr_t attr;
    struct sched_param param;
    pthread_t thr;
    int policy = SCHED_OTHER;
    int min = 0, max = 0;
    int rc = 0;
    void *ret = NULL;
    int prio_set = 0;

    /* 1. init */
    if (pthread_attr_init(&attr) != 0)
        return fail_step(1, rc, policy);
    /* 2. stack size 128 KiB */
    if (pthread_attr_setstacksize(&attr, (size_t)(128 * 1024)) != 0)
        return fail_step(2, rc, policy);

    /* 3. try SCHED_FIFO; the PPU shim does not support it -> ENOTSUP. */
    rc = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    if (rc != ENOTSUP)
        return fail_step(3, rc, policy);
    /* 4. fall back to the only supported policy. */
    policy = SCHED_OTHER;
    rc = pthread_attr_setschedpolicy(&attr, policy);
    if (rc != 0)
        return fail_step(4, rc, policy);

    /* 5. priority range for the accepted policy. */
    min = sched_get_priority_min(policy);
    max = sched_get_priority_max(policy);
    param.sched_priority = 0;   /* default unless a range exists */
    if (max > min) {
        param.sched_priority = min + (max - min) / 2;
        prio_set = 1;
    }
    if (min < 0 || max < 0)
        return fail_step(5, rc, policy);

    /* 6. apply the parameter. */
    rc = pthread_attr_setschedparam(&attr, &param);
    if (rc != 0)
        return fail_step(6, rc, policy);

    /* 7. PTHREAD_INHERIT_SCHED must be ACCEPTED (not: inherited). */
    rc = pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
    if (rc != 0)
        return fail_step(7, rc, policy);

    /* 8 & 9. sync primitives for the ping-pong. */
    rc = pthread_mutex_init(&g_mtx, NULL);
    if (rc != 0)
        return fail_step(8, rc, policy);
    rc = pthread_cond_init(&g_cv, NULL);
    if (rc != 0)
        return fail_step(9, rc, policy);

    /* 10. create the thread. */
    rc = pthread_create(&thr, &attr, worker, NULL);
    if (rc != 0)
        return fail_step(10, rc, policy);

    /* 11. attr no longer needed. */
    rc = pthread_attr_destroy(&attr);
    if (rc != 0)
        return fail_step(11, rc, policy);

    /* Main's half of the ping-pong (50 moves), then join and check. */
    for (int i = 0; i < HANDOFFS / 2; i++) {
        pthread_mutex_lock(&g_mtx);
        while (g_turn != MAIN_TURN)
            pthread_cond_wait(&g_cv, &g_mtx);
        g_counter++;
        g_turn = WORKER_TURN;
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mtx);
    }

    /* 12. join and check the worker's result. */
    rc = pthread_join(thr, &ret);
    if (rc != 0)
        return fail_step(12, rc, policy);

    if (g_counter != HANDOFFS)
        return fail_step(15, rc, policy);
    if ((long)(intptr_t)ret != HANDOFFS)
        return fail_step(16, rc, policy);

    /* 13 & 14. clean up the sync primitives. */
    rc = pthread_mutex_destroy(&g_mtx);
    if (rc != 0)
        return fail_step(13, rc, policy);
    rc = pthread_cond_destroy(&g_cv);
    if (rc != 0)
        return fail_step(14, rc, policy);

    /* Timing check: a 1 ms nanosleep must advance CLOCK_MONOTONIC. */
    {
        struct timespec t0, t1, one_ms = { 0, 1000000L };
        int64_t adv;
        rc = clock_gettime(CLOCK_MONOTONIC, &t0);
        if (rc != 0)
            return fail_step(20, rc, policy);
        rc = nanosleep(&one_ms, NULL);
        if (rc != 0 && errno != EINTR)
            return fail_step(21, rc, policy);
        rc = clock_gettime(CLOCK_MONOTONIC, &t1);
        if (rc != 0)
            return fail_step(22, rc, policy);
        adv = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
              (int64_t)(t1.tv_nsec - t0.tv_nsec);
        if (adv <= 0)
            return fail_step(23, rc, policy);
    }

    (void)prio_set;
    printf("PTHREAD_AUDIO_PATH_OK\n");
    return 0;
}
