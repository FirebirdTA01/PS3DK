/* Portable drain cost/byte-parity harness. Host memory-collector timings are
 * diagnostic only; they do not establish LV2 file cost or console overhead. */
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux__) && defined(__GLIBC__)
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#define DC_FAULT_TRACE 1
#endif

/* The test driver instruments an otherwise unchanged copy of flush_loss. */
uint64_t prof_test_empty_loss_flushes;

static uint64_t ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t hash_bytes(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
    return h;
}

static int one(unsigned lanes, unsigned events, int cost_gate)
{
    run r;
    uint64_t total = 0, lo = UINT64_MAX, hi = 0, bytes = 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned frames = 64;
    prof_test_empty_loss_flushes = 0;
    g_order_next = 1;
    run_init(&r, 8192, lanes, NULL);
    for (unsigned j = 0; j < lanes; ++j)
        if (!lane_new(&r, 4096, 0, 1)) abort();
    psp_drain(&r.p);
    for (unsigned f = 0; f < frames; ++f) {
        for (unsigned j = 0; j < lanes; ++j) {
            lane_h *h = r.lanes[j];
            for (unsigned e = 0; e < events / 2; ++e) { zb(h, 0); ze(h, 0); }
            uint16_t cal = h->s.cal_answered;
            if (psp_spu_flush(&h->s) != PSP_OK) abort();
            cal_note(h, cal);
        }
        uint64_t begin = ns();
        psp_drain(&r.p);
        uint64_t dt = ns() - begin;
        total += dt;
        if (dt < lo) lo = dt;
        if (dt > hi) hi = dt;
    }
    for (unsigned j = 0; j < lanes; ++j) {
        lane_h *h = r.lanes[j];
        uint16_t cal = h->s.cal_answered;
        if (psp_spu_fini(&h->s) != PSP_OK) abort();
        cal_note(h, cal);
        if (psp_lane_end(&r.p, h->idx) != PSP_P_OK) abort();
    }
    for (size_t i = 0; i < r.nch; ++i) {
        const chunk_rec *c = &r.ch[i];
        uint8_t header[8];
        for (unsigned k = 0; k < 4; ++k) {
            header[k] = (uint8_t)(c->type >> (8 * k));
            header[k+4] = (uint8_t)(c->len >> (8 * k));
        }
        hash = hash_bytes(hash_bytes(hash, header, sizeof header), c->data, c->len);
        bytes += c->len + 20u;
    }
    if (check_streams(&r, "drain-cost") || check_order(&r, "drain-cost", CK_RUN))
        FAIL("drain changed the independent producer/order truth");
    /* Empty boundary work may scale with lanes/drains, never with normal
     * event population. Leave room for two boundaries and setup/teardown. */
    if (cost_gate && prof_test_empty_loss_flushes > 2u * lanes * (frames + 4u))
        FAIL("empty loss work scales with events: %" PRIu64, prof_test_empty_loss_flushes);
    printf("{\"lanes\":%u,\"events\":%u,\"frames\":%u,\"mean_ns\":%" PRIu64
           ",\"min_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64 ",\"chunks\":%zu,\"bytes\":%" PRIu64
           ",\"hash\":\"%016" PRIx64 "\",\"empty_loss_calls\":%" PRIu64 "}\n",
           lanes, events, frames, total / frames, lo, hi, r.nch, bytes, hash, prof_test_empty_loss_flushes);
    run_free(&r);
    return g_failures ? 1 : 0;
}

#ifdef DC_FAULT_TRACE
/* A fault seen on one host only: print the faulting address and the frames (the test resolves the binary offsets
 * with addr2line), then die by the same signal. Diagnostics only; the gate's verdicts are unchanged. */
static void fault_trace(int sig, siginfo_t *si, void *uc)
{
    static const char head[] = "DRAIN_COST_FAULT signal ";
    char line[64];
    void *frames[48];
    int n, len = 0;
    unsigned long a = (unsigned long)si->si_addr;
    (void)uc;
    if (write(2, head, sizeof head - 1) < 0) {}
    line[len++] = (char)('0' + sig / 10);
    line[len++] = (char)('0' + sig % 10);
    memcpy(line + len, " addr 0x", 8);
    len += 8;
    for (int k = 60; k >= 0; k -= 4)
        line[len++] = "0123456789abcdef"[(a >> k) & 15u];
    line[len++] = '\n';
    if (write(2, line, (size_t)len) < 0) {}
    n = backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void fault_trace_install(void)
{
    struct sigaction sa;
    void *warm[1];
    (void)backtrace(warm, 1); /* load the unwinder now, not inside the handler */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_trace;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
#endif

int main(int argc, char **argv)
{
    const unsigned sizes[] = {0, 256, 2048, 4096};
    int cost = argc == 2 && !strcmp(argv[1], "--cost-gate");
#ifdef DC_FAULT_TRACE
    fault_trace_install();
#endif
    for (unsigned lanes = 1; lanes <= 6; lanes += 5)
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i)
            (void)one(lanes, sizes[i], cost);
    return g_failures ? 1 : 0;
}
