/* Host controls for hello-ppu-prof-auto-verdicts' bounds and stop policy: a fake clock drives the shared logic. */
#include <stdint.h>
#include <stdio.h>
#include "../../samples/gcm/hello-ppu-prof-auto-verdicts/source/verdicts_logic.h"
#define CHECK(c, n) do { if (!(c)) { puts("ASSERTION: " n); return 1; } } while (0)

typedef struct {
    uint64_t t, step;      /* fake microseconds, advanced by each idle */
    uint64_t ready_at;     /* UINT64_MAX = never ready */
    int quit, idles, backwards, reads;
} fake;
static uint64_t f_now(void *u) {
    fake *f = u;
    f->reads++;
    return f->backwards ? f->t - (uint64_t)f->reads : f->t; /* each read earlier than the last */
}
static int f_ready(void *u, uint32_t arg) { fake *f = u; (void)arg; return f->t >= f->ready_at; }
static void f_idle(void *u) { fake *f = u; f->t += f->step; f->idles++; }
static int f_quit(void *u) { fake *f = u; return f->quit; }
static int run(fake *f, int stoppable) {
    av_clock c = { f_now, f_ready, f_idle, f_quit, f };
    return av_await(&c, 0, 0, AV_SESSION_US, stoppable);
}

typedef struct { int calls, fail_at; av_state *s; } phase_ctx;
static int frame(void *u, int i) {
    phase_ctx *p = u;
    p->calls++;
    if (i == p->fail_at) { av_fail(p->s, "submit-flip"); return 0; }
    return 1;
}

int main(void) {
    /* a ready condition completes the wait */
    fake f = { 1000, 1000, 5000, 0, 0, 0, 0 };
    CHECK(run(&f, 1) == AV_DONE && f.idles == 4, "ready_completes");
    /* permanently not ready: the per-wait bound stops it after AV_WAIT_US, not the session budget */
    fake g = { 1000, 1000, UINT64_MAX, 0, 0, 0, 0 };
    CHECK(run(&g, 1) == AV_EXPIRED, "never_ready_expires");
    CHECK(g.idles == (int)(AV_WAIT_US / 1000), "wait_bound");
    /* ready exactly at the deadline is not accepted: the deadline is checked first */
    fake h = { 1000, 1000, 1000 + AV_WAIT_US, 0, 0, 0, 0 };
    CHECK(run(&h, 1) == AV_EXPIRED, "deadline_first");
    /* a quit request stops a workload wait, but not the exit drain */
    fake q = { 1000, 1000, UINT64_MAX, 1, 0, 0, 0 };
    CHECK(run(&q, 1) == AV_QUIT && q.idles == 0, "quit_stops_workload_wait");
    fake d = { 1000, 1000, UINT64_MAX, 1, 0, 0, 0 };
    CHECK(run(&d, 0) == AV_EXPIRED, "drain_ignores_quit");
    /* the session budget bounds every wait even when the wait itself just began */
    CHECK(av_poll(0, AV_SESSION_US, AV_SESSION_US, 1, 0) == AV_EXPIRED, "session_budget");
    /* a clock that goes backwards expires at once */
    fake b = { 5000, 1000, UINT64_MAX, 0, 0, 1, 0 };
    CHECK(run(&b, 1) == AV_EXPIRED && b.idles <= 1, "clock_backwards_expires");

    /* a failed frame stops its phase at once and every later phase runs nothing: no retry, no further submission */
    av_state s = { 0, 0 };
    phase_ctx p = { 0, 2, &s };
    CHECK(av_phase(&s, 60, frame, &p) == 3 && p.calls == 3, "failure_stops");
    phase_ctx p2 = { 0, -1, &s };
    CHECK(av_phase(&s, 60, frame, &p2) == 0 && p2.calls == 0, "later_phase_runs_nothing");
    CHECK(s.failed != 0, "failure_kept");
    av_state qs = { 0, 1 };
    phase_ctx p3 = { 0, -1, &qs };
    CHECK(av_phase(&qs, 60, frame, &p3) == 0, "quit_runs_nothing");

    /* exit: seal only when both the last flip and the fresh fence completed */
    CHECK(av_exit(1, 1) == AV_EXIT_SEAL, "idle_seals");
    CHECK(av_exit(1, 0) == AV_EXIT_RECOVERY, "fence_timeout_recovers");
    CHECK(av_exit(0, 0) == AV_EXIT_RECOVERY, "flip_timeout_recovers");
    puts("verdict sample waits, stop policy and exit policy PASS");
    return 0;
}
