#ifndef HELLO_PPU_PROF_AUTO_VERDICTS_LOGIC_H
#define HELLO_PPU_PROF_AUTO_VERDICTS_LOGIC_H
/* Bounds and stop policy for hello-ppu-prof-auto-verdicts, kept free of GCM so a host test can drive them with a
 * fake clock (tests/host/prof-auto-verdicts-test.py). AV_MUTANT selects a deliberately broken policy for the
 * test's sensitivity controls; it is 0 in the sample. */
#include <stdint.h>
#ifndef AV_MUTANT
#define AV_MUTANT 0
#endif

#define AV_SESSION_US UINT64_C(120000000) /* the whole workload: 300 frames run in about 55 s on the console */
#define AV_DRAIN_US   UINT64_C(130000000) /* the exit drain may run a little past the workload budget */
#define AV_WAIT_US    UINT64_C(2000000)   /* any one flip, label or fence wait */

enum { AV_PENDING, AV_DONE, AV_EXPIRED, AV_QUIT };

/* One polling step. A deadline wins over a ready that arrives at or after it; a quit request stops a workload
 * wait (the exit drain passes quit = 0). Elapsed values are UINT64_MAX when the clock went backwards. */
static inline int av_poll(uint64_t waited_us, uint64_t session_us, uint64_t session_limit, int ready, int quit) {
    if ((AV_MUTANT != 1 && waited_us >= AV_WAIT_US) || session_us >= session_limit) return AV_EXPIRED;
    if (ready) return AV_DONE;
    if (quit) return AV_QUIT;
    return AV_PENDING;
}

static inline uint64_t av_since(uint64_t now, uint64_t start) { return now >= start ? now - start : UINT64_MAX; }

typedef struct {
    uint64_t (*now)(void *);       /* microseconds */
    int (*ready)(void *, uint32_t);
    void (*idle)(void *);          /* sleep briefly and service system callbacks */
    int (*quit)(void *);
    void *ctx;
} av_clock;

/* Poll `ready(arg)` until done, a deadline, or (when `stoppable`) a quit request. */
static inline int av_await(const av_clock *c, uint32_t arg, uint64_t session_start, uint64_t session_limit,
                           int stoppable) {
    uint64_t start = c->now(c->ctx);
    for (;;) {
        uint64_t now = c->now(c->ctx);
        int d = av_poll(av_since(now, start), av_since(now, session_start), session_limit, c->ready(c->ctx, arg),
                        stoppable && c->quit(c->ctx));
        if (d != AV_PENDING) return d;
        c->idle(c->ctx);
    }
}

/* The run's stop state: the first failure is kept, and once failed or quit no further frame is submitted. */
typedef struct {
    const char *failed;
    int quit;
} av_state;

static inline void av_fail(av_state *s, const char *where) {
    if (!s->failed) s->failed = where;
}
static inline int av_stopping(const av_state *s) {
    return AV_MUTANT == 2 ? 0 : (s->failed != 0 || s->quit);
}

/* Run up to `frames` frames of one phase; `frame` returns 0 after recording a failure. Returns frames attempted. */
static inline int av_phase(av_state *s, int frames, int (*frame)(void *, int), void *ctx) {
    int n = 0;
    for (int i = 0; i < frames && !av_stopping(s); i++) {
        n++;
        if (!frame(ctx, i)) av_fail(s, "frame");
    }
    return n;
}

/* Exit: seal and return only after the final flip and fence prove the RSX idle; otherwise recovery is required. */
enum { AV_EXIT_SEAL, AV_EXIT_RECOVERY };
static inline int av_exit(int flip_done, int fence_done) {
    if (AV_MUTANT == 3) return AV_EXIT_SEAL;
    return flip_done && fence_done ? AV_EXIT_SEAL : AV_EXIT_RECOVERY;
}
#endif
