/* ps3dk-prof test hooks.  Compiled ONLY with PS3DK_PROF_TEST_HOOKS (the host B tests); the SDK Makefile never defines
 * it, and the archive check refuses any ps3dk_prof_test_ symbol in an SDK archive.  Every hook defaults to off. */
#ifndef PS3DK_PROF_TEST_HOOKS_H
#define PS3DK_PROF_TEST_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* STOP-batch step observer events. */
enum {
    PS3DK_PROF_TEST_STEP_ASSIGN = 1,    /* value: number of sequences assigned at once */
    PS3DK_PROF_TEST_STEP_PUBLISH = 2,   /* value: number of slots published at once */
    PS3DK_PROF_TEST_STEP_DROP = 3,      /* value: number of slots dropped at once */
    PS3DK_PROF_TEST_STEP_REFUSE = 4     /* value: k */
};

/* Preset and read fields of the calling thread's incarnation. */
enum {
    PS3DK_PROF_TEST_NEXT_SEQ = 1,
    PS3DK_PROF_TEST_REFUSED = 2,
    PS3DK_PROF_TEST_REGERR = 3,
    PS3DK_PROF_TEST_NEXT_REFUSED_ID = 4,
    PS3DK_PROF_TEST_NEXT_SPAN_ID = 5,
    PS3DK_PROF_TEST_DROPPED = 6,
    PS3DK_PROF_TEST_LIMIT = 7,
    PS3DK_PROF_TEST_DEPTH = 8,
    PS3DK_PROF_TEST_REFUSED_TOP = 9,
    PS3DK_PROF_TEST_OPEN_SPANS = 10,
    PS3DK_PROF_TEST_INCARNATION = 11,
    PS3DK_PROF_TEST_GENERATION = 12,
    PS3DK_PROF_TEST_SLOT = 13,
    PS3DK_PROF_TEST_EXHAUSTED = 14
};

/* Global statistics. */
enum {
    PS3DK_PROF_TEST_STAT_POST_FRONTIER_TRANSITIONS = 1,  /* must stay 0 */
    PS3DK_PROF_TEST_STAT_ORDINARY_DRAINS_AFTER_FRONTIER = 2,  /* must stay 0 */
    PS3DK_PROF_TEST_STAT_DRAINS = 3,
    PS3DK_PROF_TEST_STAT_DRAIN_THREAD_ITERATIONS = 4,
    PS3DK_PROF_TEST_STAT_WAKE_SEEN_ITERATION = 5,      /* the drain iteration that observed the wake flag */
    PS3DK_PROF_TEST_STAT_WAKE_SET_ITERATION = 6,       /* the drain iteration count when shutdown set it */
    PS3DK_PROF_TEST_STAT_GATE = 7,
    PS3DK_PROF_TEST_STAT_FROZEN = 8,
    PS3DK_PROF_TEST_STAT_LATCH = 9,                    /* the stop latch (ACQUIRE) */
    PS3DK_PROF_TEST_STAT_SLOT_STATE_BASE = 100         /* + slot index */
};

typedef struct ps3dk_prof_test_hooks_t {
    void (*before_ack_store)(void);
    void (*before_retire_store)(void);
    void (*before_live_to_retiring)(void);
    void (*after_reserve_before_publish)(void);
    void (*between_desc_entry_and_publish)(void);
    void (*drainer_after_snapshot)(void);
    void (*drainer_before_free)(void);
    void (*before_frontier)(void);
    void (*step_observer)(int step, uint64_t value);
    uint64_t (*clock)(void);           /* replaces the timebase */
    int mono_clock_fail;
    int id_fail;
    int alloc_fail;
    int thread_create_fail;
    int join_fail;
    int freq_zero;
    int open_errno;                    /* nonzero: open fails with this errno */
    /* file-sink write fault: return 1 to replace the write with *ret / *err */
    int (*write_fault)(unsigned long n, long *ret, int *err);
    int close_fail;                    /* nonzero: close fails with this errno */
    int corrupt_next_pub_len;          /* corrupt the next publication's pub_len */
} ps3dk_prof_test_hooks_t;

extern ps3dk_prof_test_hooks_t ps3dk_prof_test_hooks;

void ps3dk_prof_test_preset(int field, uint64_t value);
uint64_t ps3dk_prof_test_read(int field);
uint64_t ps3dk_prof_test_stat(int which);

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_PROF_TEST_HOOKS_H */
