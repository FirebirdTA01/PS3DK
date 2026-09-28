/* spurs-suite barrier: tasks, slots and timing. */
#ifndef SPURS_SUITE_BARRIER_H
#define SPURS_SUITE_BARRIER_H

#include "common.h"

/* argTask: u64[0] = result slots, u32[2] = barrier EA, u32[3] = task index.
 * Each task reports in slot <index>: status = the wait's rc, value = how
 * many tasks had notified when it passed the barrier (read through a
 * shared counter), extra = its try-wait rc before notifying. */
#define B_TASKS      3
#define B_LATE       0          /* this task notifies last, after a delay */
#define B_ERRORS     B_TASKS    /* slot for the error-path task */
#define B_SLOTS      (B_TASKS + 1)

#endif
