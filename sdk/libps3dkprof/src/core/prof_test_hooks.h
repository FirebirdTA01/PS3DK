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
    int id_fail;                       /* the APP owner identity read fails (pal_owner_get / pal_owner_is_self) */
    int os_id_fail;                    /* the OS thread id read fails (pal_os_thread_id): registration refused */
    int alloc_fail;
    /* SPU lane extension (slice I2) */
    int slot_generation_seed_set;      /* the next registration into slot_generation_seed_slot starts from it */
    uint32_t slot_generation_seed, slot_generation_seed_slot;
    uint32_t spu_lock_fail_at, spu_unlock_fail_at, spu_ops; /* nth spu_mutex lock / unlock fails (1-based) */
    uint32_t spu_unlock_fail_site; /* the first unlock at this site fails: 1 step 3b, 2 the seal, 3 lane_end's final */
    void (*ext_before_failing_unlock)(void); /* R2: runs right BEFORE the real unlock that spu_unlock_fail_site fails */
    void (*gpu_before_ack)(void);       /* R2: between the drainer's real closing unlock and a detach acknowledgement */
    void (*spu_admission_pause)(void);  /* after the admission check, before the mutation (race tests) */
    void (*spu_before_seal)(void);      /* holding spu_mutex, right before psp_seal (seal-time corruption tests) */
    uint32_t spu_src_fail_at;           /* register_elf: the nth descriptor-source get (both passes, 1-based) fails */
    void (*spu_elf_after_parse)(void);  /* register_elf: after the lock-free parse, before spu_mutex (race tests) */
    /* P3.3 R2 GPU domains (host only) */
    const void *gpu_port;               /* a prg_port: the emulated GPU device (its lock and emittable fields unused) */
    const void *gpu_gpp_dev;            /* R4a: a gpp_dev: the REAL GCM port over fake memory (exclusive with gpu_port) */
    void (*gpu_after_ack_store)(void);  /* R4a: after a detach acknowledgement's store, before the publisher's unpin */
    uint32_t gpu_mutex_init_fail_at;    /* the nth GPU snap mutex init fails (1-based) */
    void (*gpu_after_pin_inc)(void);    /* after users++ and before the id load (stale-call schedules) */
    void (*gpu_after_prologue)(void);   /* after the shutdown check, before the core call (admission schedules) */
    void (*gpu_in_frame_install)(void); /* inside gpu_frame's thread_mutex section, after validation */
    uint32_t gpu_snap_lock_fail_at, gpu_snap_unlock_fail_at, gpu_snap_ops; /* nth snap lock / unlock fails */
    int gpu_snap_nolock;                /* the TSan NEGATIVE control: the snap mutex does nothing */
    void (*drainer_after_tables)(void); /* the drain, after steps 2a/2b (THREADS, DESCRIPTORS) and before step 3 */
    uint32_t p_lock_fail_at, p_lock_ops, p_unlock_fail_at, p_unlock_ops; /* the nth p_lock / p_unlock fails */
    uint64_t chunk_seq_seed;           /* nonzero: the drainer's chunk_seq starts there (ceiling tests) */
    int thread_create_fail;
    int join_fail;
    int freq_zero;
    int open_errno;                    /* nonzero: open fails with this errno */
    /* file-sink write fault: return 1 to replace the write with *ret / *err */
    int (*write_fault)(unsigned long n, long *ret, int *err);
    int close_fail;                    /* nonzero: close fails with this errno */
    int corrupt_next_pub_len;          /* corrupt the next publication's pub_len */
    /* SYNTHETIC ID_COLLISION branch coverage: returns the candidate id that registration compares against the
     * published ids, given the natural id.  Only that comparison uses it; a new entry and its REGERR keep the natural
     * id, so no descriptor with a forced id is ever written.  NULL (the default): the natural id. */
    uint64_t (*candidate_desc_id)(const char *name, uint32_t line, uint64_t natural_id);
    /* Called by the successful init after the capture record is filled and before it is published. */
    void (*before_capture_publish)(void);
    /* Called by shutdown right after it wins its claim, before it locks anything or sets the stop latch. */
    void (*after_shutdown_claim)(void);
    int live_ephemeral_port;           /* ps3dk_prof_live_start binds port 0 (an ephemeral port), for host tests */
    uint16_t live_listen_port;         /* nonzero: ps3dk_prof_live_start binds 127.0.0.1 on this port (wins) */
    unsigned join_calls;               /* host PAL: pal_thread_join calls, counted (controlling thread only) */
    /* Capture RSX frame S2 (host test builds only) */
    void (*rsxcap_step)(int point);    /* the runtime's step points (rsxcap.h RT_* / HK_* / AG_* / DT_*) */
    uint32_t rsxcap_other_cas_fail;    /* the next n other-thread CAS attempts meet a real concurrent increment */
    int rsxcap_neg_plain_other;        /* TSan NEGATIVE control: the other-thread counter is a plain increment */
    int rsxcap_neg_late_fields;        /* TSan NEGATIVE control: the agent writes its fields after the publication */
    uint64_t ext_lock_contended;       /* atomic: the extension lock found its mutex held (then waited) */
    uint32_t rsxcap_pin_cas_fail;      /* the next n request-scan pin CAS attempts meet a real concurrent pin */
} ps3dk_prof_test_hooks_t;

extern ps3dk_prof_test_hooks_t ps3dk_prof_test_hooks;

void ps3dk_prof_test_preset(int field, uint64_t value);
uint64_t ps3dk_prof_test_read(int field);
uint64_t ps3dk_prof_test_stat(int which);

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_PROF_TEST_HOOKS_H */
