/* Host B tests for the ps3dk-prof runtime (step 3).  Each test runs in its own process (one capture per process) and
 * gets a fresh output directory.  A test returns 0 on pass.  For every capture it wants round-tripped through the
 * Rust reader it prints "CAPTURE <path> EXPECT <exit>" (0 COMPLETE, 1 INCOMPLETE, 2 MALFORMED); build-and-run.sh runs
 * `ps3dk-prof check` on it and compares the exit status. */
#ifndef RT_H
#define RT_H

#include <ps3dk/prof.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "prof_test_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RT_CEIL (1ull << 63)
#define RT_UNKNOWN 0xFFFFFFFFFFFFFFFFull

enum { RT_BEGIN = 1, RT_BEGIN_PARENT = 2, RT_END = 3, RT_FRAME = 4, RT_GAP = 5, RT_REGERR = 6 };
enum { RT_RETIRED = 1, RT_ACKED = 2, RT_NOT_QUIESCED = 3 };
enum { RT_SLOT_FREE = 0, RT_SLOT_LIVE = 1, RT_SLOT_RETIRING = 2, RT_SLOT_RETIRED_PENDING = 3 };
enum { RT_G_UNINIT = 0, RT_G_CONFIGURING = 1, RT_G_INITIALIZING = 2, RT_G_READY = 3, RT_G_TERMINAL = 4 };
#define RT_H_ (ps3dk_prof_test_hooks)
#define RT_READ(f) ps3dk_prof_test_read(PS3DK_PROF_TEST_##f)
#define RT_PRESET(f, v) ps3dk_prof_test_preset(PS3DK_PROF_TEST_##f, (v))
#define RT_STAT(f) ps3dk_prof_test_stat(PS3DK_PROF_TEST_STAT_##f)

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                 \
            return 1;                                                                                \
        }                                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        unsigned long long rt_a_ = (unsigned long long)(a), rt_b_ = (unsigned long long)(b);         \
        if (rt_a_ != rt_b_) {                                                                        \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s = %llu (%lld), %s = %llu (%lld)\n", __FILE__, \
                    __LINE__, #a, rt_a_, (long long)rt_a_, #b, rt_b_, (long long)rt_b_);             \
            return 1;                                                                                \
        }                                                                                            \
    } while (0)

/* Run a sub-check that returns int (0 pass); propagate a failure. */
#define CHECK_OK(expr)                                                                               \
    do {                                                                                             \
        if ((expr) != 0) {                                                                           \
            fprintf(stderr, "%s:%d: sub-check failed: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                                \
        }                                                                                            \
    } while (0)

/* Checks inside worker threads: record the failure and leave the thread. */
int rt_worker_failed(void);
void rt_worker_fail_set(void);
#define WCHECK(cond)                                                                                 \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            fprintf(stderr, "%s:%d: worker CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            rt_worker_fail_set();                                                                    \
            return NULL;                                                                             \
        }                                                                                            \
    } while (0)

#define AST(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)
#define ALD(x) __atomic_load_n(&(x), __ATOMIC_SEQ_CST)

/* The STOP-batch step observer: records (step, value, the caller's next_seq at that step) while on. */
typedef struct rt_step {
    int step;
    uint64_t value, next_seq;
} rt_step;
extern rt_step g_steps[64];
extern int g_nsteps;
void rt_steps_install(void);
void rt_steps_on(void);
void rt_steps_off(void);

/* ---- setup -------------------------------------------------------------------------------------------------------- */

/* Defaults for tests: app "rt", ring 512, max_threads 8, quiesce 2 s, file sink in out. */
void rt_config(ps3dk_prof_config *c);
int rt_set_dir(const char *out);
/* config + sink dir + init (APP mode unless c->mode says otherwise) + optional thread_begin("main"). */
int rt_start(const char *out, ps3dk_prof_config *c, int register_main);
/* The single *.prof file in dir (0) or -1. */
int rt_capture_path(const char *dir, char *buf, size_t n);
/* Print the CAPTURE line for the single capture in dir. */
int rt_expect(const char *dir, int code);
void rt_expect_path(const char *path, int code);
uint64_t rt_file_hash(const char *path);   /* FNV-1a-64 of the bytes; 0 if unreadable */
long rt_file_size(const char *path);
void rt_sleep_ms(unsigned ms);
uint64_t rt_now_ms(void);
/* Wait until the stop latch is set (5 s limit). */
int rt_wait_latch(void);

/* ---- simple events and threads ------------------------------------------------------------------------------------ */

typedef struct rt_event {
    void *impl;   /* pthread mutex + cond + flag */
} rt_event;
void rt_event_init(rt_event *e);
void rt_event_set(rt_event *e);
int rt_event_wait(rt_event *e, unsigned timeout_ms);   /* 0 set, -1 timeout */
void rt_event_reset(rt_event *e);

typedef struct rt_thread {
    void *impl;
} rt_thread;
void rt_spawn(rt_thread *t, void *(*fn)(void *), void *arg);
void *rt_join(rt_thread *t);

/* ---- a memory callback sink ----------------------------------------------------------------------------------------- */

typedef struct rt_mem {
    uint8_t *buf;
    size_t len, cap;
    unsigned writes, flushes, closes;
    char order[64];          /* 'w' (first write only), 'f', 'c' in call order */
    int over_n;              /* write returns n + 1 */
    int neg_after;           /* nonzero: the neg_after-th write returns -5 */
    int flush_ret, close_ret;
    unsigned block_ms;       /* every write after the latch sleeps this long */
    void (*in_write)(void);  /* called inside every write (reentrancy tests) */
} rt_mem;
extern rt_mem g_mem;
int rt_use_mem_sink(int with_flush);
int rt_mem_dump(const char *path);

/* ---- a mini reader for assertions (the Rust reader is the verdict) -------------------------------------------------- */

typedef struct rt_rec {
    uint32_t inc;
    uint8_t kind, depth;
    uint16_t flags;
    uint32_t a;
    uint64_t seq, b, c;
    uint32_t chunk;          /* index of its RECORDS chunk */
} rt_rec;

typedef struct rt_final {
    uint32_t inc;
    uint8_t state, limit;
    uint64_t produced, written, dropped, refused, regerr, open_spans;
    int in_retired;          /* from a RETIRED chunk (1) or the TRAILER (0) */
} rt_final;

typedef struct rt_cap {
    int has_trailer;
    uint32_t end_flags;
    uint64_t sink_error_count, unreg, desc_count;
    int32_t first_error;
    uint32_t nthreads, threads_inc[2048], threads_slot[2048];
    uint32_t ndesc;
    uint32_t nfinal;
    rt_final finals[2048];
    rt_rec *recs;
    size_t nrecs;
    uint32_t nrecchunks;
    uint32_t recchunk_n[4096];
    uint32_t nretired_chunks;
} rt_cap;

int rt_cap_load(const char *path, rt_cap *c);   /* 0 parsed (a torn or trailer-less file is fine), -1 not a capture */
void rt_cap_free(rt_cap *c);
const rt_final *rt_final_of(const rt_cap *c, uint32_t inc);
size_t rt_count(const rt_cap *c, uint32_t inc, int kind, int flags);   /* inc 0 = any; flags -1 = any */

/* ---- descriptors made at runtime (distinct sites without macros) -------------------------------------------------- */
void rt_desc(ps3dk_prof_desc *d, const char *name, uint32_t line);

#ifdef __cplusplus
}
#endif

#endif /* RT_H */
