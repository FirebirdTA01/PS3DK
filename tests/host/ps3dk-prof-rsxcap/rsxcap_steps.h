/* Capture RSX frame S2 host tests: the deterministic step-point harness shared by rsxcap_core.c and rsxcap_rt.c.
 *
 * A case arms up to TWO independent pauses (slot 0 and 1), each (point, nth hit, optional thread).  The thread that
 * reaches an armed point stores PAUSED (release) and spins on GO (acquire); the controller spins on PAUSED, performs
 * the racing action, then stores GO.  Every wait is a spin of at most g_spin_max (SPIN_MAX) sched_yield iterations;
 * past it the case prints HARNESS-STALL and exits 3 (INVALID, never PASS).  An inline action (run on the reaching
 * thread itself, no pause) is the alternative for single-thread injections.  Every point's hits are counted. */
#ifndef RSXCAP_STEPS_H
#define RSXCAP_STEPS_H

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define SPIN_MAX 100000000L
static long g_spin_max = SPIN_MAX; /* a harness control lowers it to force a stall */
#define STEP_POINTS 64

static const char *g_case_name = "?";
typedef struct step_slot {
    atomic_int point, nth, state, hits, thread_set; /* state: 0 idle, 1 paused, 2 go */
    pthread_t thread;
} step_slot;
static step_slot st_slot[2];
static atomic_int st_hits[STEP_POINTS];
static void (*volatile st_inline)(int point); /* slot 0 only */

static void harness_stall(const char *what)
{
    printf("HARNESS-STALL %s: %s\n", g_case_name, what);
    fflush(stdout);
    _Exit(3);
}

/* every spin is bounded by iteration count */
#define SPIN_UNTIL(cond, what)                                                                                         \
    do {                                                                                                               \
        long spin_i_;                                                                                                  \
        for (spin_i_ = 0; !(cond); spin_i_++) {                                                                        \
            if (spin_i_ >= g_spin_max)                                                                                 \
                harness_stall(what);                                                                                   \
            sched_yield();                                                                                             \
        }                                                                                                              \
    } while (0)

__attribute__((unused)) static void step_arm2(int s, int point, int nth)
{
    atomic_store(&st_slot[s].hits, 0);
    atomic_store(&st_slot[s].state, 0);
    atomic_store(&st_slot[s].nth, nth);
    atomic_store(&st_slot[s].point, point);
}
__attribute__((unused)) static void step_arm(int point, int nth) { step_arm2(0, point, nth); }
__attribute__((unused)) static void step_arm_thread(int point, int nth, pthread_t t)
{
    st_slot[0].thread = t;
    atomic_store(&st_slot[0].thread_set, 1);
    step_arm2(0, point, nth);
}
__attribute__((unused)) static void step_disarm(void)
{
    atomic_store(&st_slot[0].point, 0);
    atomic_store(&st_slot[0].thread_set, 0);
}
__attribute__((unused)) static void step_wait_paused2(int s) { SPIN_UNTIL(atomic_load(&st_slot[s].state) == 1, "waiting for the paused point"); }
__attribute__((unused)) static void step_go2(int s) { atomic_store(&st_slot[s].state, 2); }
__attribute__((unused)) static void step_wait_paused(void) { step_wait_paused2(0); }
__attribute__((unused)) static void step_go(void) { step_go2(0); }
__attribute__((unused)) static int step_hits(int point) { return atomic_load(&st_hits[point]); }

__attribute__((unused)) static void step_hit(int point)
{
    if (point > 0 && point < STEP_POINTS)
        atomic_fetch_add(&st_hits[point], 1);
    for (int s = 0; s < 2; s++) {
        step_slot *x = &st_slot[s];
        if (point != atomic_load(&x->point))
            continue;
        if (atomic_load(&x->thread_set) && !pthread_equal(pthread_self(), x->thread))
            continue;
        if (atomic_fetch_add(&x->hits, 1) + 1 != atomic_load(&x->nth))
            continue;
        atomic_store(&x->point, 0); /* one shot */
        if (s == 0 && st_inline) {
            st_inline(point);
            return;
        }
        atomic_store(&x->state, 1);
        SPIN_UNTIL(atomic_load(&x->state) == 2, "paused, waiting for GO");
        atomic_store(&x->state, 0);
        return;
    }
}

#endif
