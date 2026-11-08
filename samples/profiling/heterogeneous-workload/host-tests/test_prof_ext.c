/* Host tests for SAMPLE_PROF_EXT's production protocol code: prof_ext.c (epoch rule, argument packing, fence,
 * lifecycle), pool_proto.c (the PPU pool protocol) and worker_proto.c (the SPU worker loop), each bound to mocks;
 * an end-to-end case runs the real worker loop in a thread behind a mailbox, driven by the real pool protocol. */
#define _POSIX_C_SOURCE 200809L
#include "../source/pool_proto.h"
#include "../source/prof_ext.h"
#include "../source/worker_proto.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned tests, failures;
static void check(int yes, const char *name)
{
    ++tests;
    if (!yes) {
        ++failures;
        printf("FAIL %s\n", name);
    }
}
#define S(x) ((uint64_t)(x) * 1000000ull)

/* ---- the epoch rule ------------------------------------------------------------------------------------------ */

static void epochs(void)
{
    pe_epochs e;
    uint32_t w;

    /* reset acknowledgement: old reset 0, flagged dispatch at 21 s, the acknowledgement decides */
    pe_epochs_init(&e, 1, 0);
    w = pe_job_word(&e, 0, 1, S(21), 1);
    check((w & PE_FLAG_RESET) && e.w32_eligible, "dispatch at 21 s: inside the bound");
    pe_job_done(&e, 0, PE_EPOCH_BOUND_US - 1, 0);
    check(e.w32_eligible, "ack one microsecond under 22 s: the old epoch stays inside its bound");
    pe_epochs_init(&e, 1, 0);
    pe_job_word(&e, 0, 1, S(21), 1);
    pe_job_done(&e, 0, PE_EPOCH_BOUND_US, 0);
    check(!e.w32_eligible, "ack at exactly 22 s: the old epoch's flush may be late");
    pe_epochs_init(&e, 1, 0);
    pe_job_word(&e, 0, 1, S(21), 1);
    pe_job_done(&e, 0, S(23), 0);
    check(!e.w32_eligible, "review control: dispatch 21 s, ack 23 s clears eligibility");

    /* the deadline is tracked independently of the epoch bound */
    pe_epochs_init(&e, 1, 0);
    pe_job_word(&e, 0, 1, S(5), 1);
    pe_job_done(&e, 0, S(5) + PE_DEADLINE_US, 0);
    check(e.w32_eligible, "a job exactly at its deadline");
    pe_job_word(&e, 0, 2, S(8), 1);
    pe_job_done(&e, 0, S(8) + PE_DEADLINE_US + 1, 0);
    check(!e.w32_eligible, "a job one microsecond over its deadline clears it");

    /* 60 s idle: the reset's flush services the old epoch at 60 s */
    pe_epochs_init(&e, 1, 0);
    pe_job_word(&e, 0, 1, S(60), 1);
    check(!e.w32_eligible, "a 60 s pause clears it at dispatch");
    pe_job_done(&e, 0, S(60) + 1000, 0);
    pe_job_word(&e, 0, 2, S(61), 1);
    pe_job_done(&e, 0, S(61) + 1000, 0);
    check(!e.w32_eligible, "a later reset never restores it");

    /* reset-only keep-alive in a normal schedule: SPU phases 1/2, idle phases 0/3 */
    pe_epochs_init(&e, 2, 0);
    uint32_t seq = 1;
    int resets = 0, reset_only = 0;
    for (uint64_t t = S(1); t < S(130); t += 16000) {
        unsigned phase = (unsigned)((t / S(8)) % 4);
        for (unsigned i = 0; i < 2; ++i) {
            w = pe_job_word(&e, i, seq, t, phase == 1 || phase == 2);
            if (w) {
                resets += (w & PE_FLAG_RESET) != 0;
                reset_only += (w & PE_FLAG_RESET_ONLY) != 0;
                pe_job_done(&e, i, t + 1000, 0);
            }
        }
        ++seq;
    }
    for (unsigned i = 0; i < 2; ++i)
        pe_final(&e, i, S(130), 0);
    check(e.w32_eligible && resets > 0 && reset_only > 0, "normal schedule with keep-alive stays eligible");

    /* nondefault phases, a skipped cycle, statuses, protocol misuse, a clock going backwards */
    for (int k = 0; k < 2; ++k) {
        uint64_t ph = k ? S(30) : S(1);
        pe_epochs_init(&e, 1, 0);
        seq = 1;
        for (uint64_t t = 16000; t < S(150); t += 16000) {
            int work = ((t / ph) % 4 == 1) || ((t / ph) % 4 == 2);
            if (pe_job_word(&e, 0, seq++, t, work))
                pe_job_done(&e, 0, t + 1000, 0);
        }
        check(e.w32_eligible, k ? "30 s phases stay eligible" : "1 s phases stay eligible");
    }
    pe_epochs_init(&e, 1, 0);
    check(pe_job_word(&e, 0, 2, S(40), 0) == (2u | PE_FLAG_RESET_ONLY) && !e.w32_eligible, "a skipped cycle");
    const uint32_t bits[] = {PE_SPU_ATTACH_FAILED, PE_SPU_RESET_FAILED, PE_SPU_FLUSH_FAILED, PE_SPU_FINI_FAILED};
    for (unsigned b = 0; b < 4; ++b) {
        pe_epochs_init(&e, 1, 0);
        pe_final(&e, 0, S(1), bits[b]);
        check(!e.w32_eligible, "every SPU failure bit in a FINAL status clears it");
    }
    pe_epochs_init(&e, 1, 0);
    pe_job_done(&e, 0, S(1), 0);
    check(!e.w32_eligible, "a completion with no word in flight clears it");
    pe_epochs_init(&e, 1, 0);
    pe_job_word(&e, 0, 1, S(1), 1);
    pe_job_word(&e, 0, 2, S(2), 1);
    check(!e.w32_eligible, "a second word before the first completed clears it");
    pe_epochs_init(&e, 1, 0);
    pe_job_word(&e, 0, 1, S(1), 1);
    pe_final(&e, 0, S(2), 0);
    check(!e.w32_eligible, "a final marker with a word in flight clears it");
    pe_epochs_init(&e, 1, S(5));
    pe_job_word(&e, 0, 1, S(4), 1);
    check(!e.w32_eligible, "a clock going backwards clears it");
    pe_epochs_init(&e, 1, 0);
    pe_final(&e, 0, PE_EPOCH_BOUND_US, 0);
    check(!e.w32_eligible, "a late fini clears it");
    pe_epochs_init(&e, 1, 0);
    w = pe_job_word(&e, 0, 0xffffffffu, S(25), 1);
    check((w & PE_SEQ_MASK) == PE_SEQ_MASK && (w & PE_FLAG_RESET) && !(w & PE_FLAG_RESET_ONLY), "masking");
}

/* ---- arguments and config -------------------------------------------------------------------------------- */

static void args(void)
{
    pe_args a = pe_pack_off(0x10002000u, 3, 6);
    check(a.arg0 == 0x10002000u && a.arg1 == 3 && a.arg2 == 6 && a.arg3 == 0, "OFF ABI exactly as today");
    pe_args x;
    check(pe_pack_ext(0x10002000u, 3, 6, 0x30000080u, 1024, 0x30010000u, &x) == 0, "EXT pack");
    pe_unpacked u = pe_unpack_ext(x);
    check(u.slot_ea == 0x10002000u && u.lane == 3 && u.lanes == 6 && u.ring_ea == 0x30000080u && u.ring_slots == 1024 &&
              u.ctl_ea == 0x30010000u,
          "EXT round trip");
    check(pe_pack_ext(0, 0, 1, 0x100000000ull, 1024, 0x80, &x) != 0, "a 64-bit ring EA is refused");
    check(pe_pack_ext(0, 0, 1, 0x40, 1024, 0x80, &x) != 0, "an unaligned ring EA is refused");
    check(pe_pack_ext(0, 0, 1, 0x80, 0, 0x80, &x) != 0, "zero ring slots are refused");
    check(pe_pack_ext(0, 0, 1, 0, 1, 0, &x) == 0 && pe_unpack_ext(x).ring_ea == 0, "no lane: ring EA 0 (unprofiled)");
    /* P-EA32: the slot EA is a full 64-bit parameter, checked like the others (it was a uint32_t: narrowed) */
    check(pe_pack_ext(0x100000000ull, 0, 1, 0x80, 1024, 0x80, &x) != 0, "a slot EA of 4 GiB is refused (no narrowing)");
    check(pe_pack_ext(0x100002000ull, 0, 1, 0, 1, 0, &x) != 0, "a 64-bit slot EA is refused for the unprofiled words too");
    check(pe_pack_ext(0x10002040u, 0, 1, 0x80, 1024, 0x80, &x) != 0, "a misaligned slot EA is refused");
    check(pe_pack_ext(0xffffff80u, 0, 1, 0xffffff80u, 1024, 0xffffff80u, &x) == 0 && pe_unpack_ext(x).slot_ea == 0xffffff80u &&
              pe_unpack_ext(x).ctl_ea == 0xffffff80u && pe_unpack_ext(x).ring_ea == 0xffffff80u,
          "the highest aligned 32-bit EAs pack and round-trip exactly");
    check(pe_pack_ext(0, 0, 1, 0x80, 1024, 0x100000000ull, &x) != 0, "a 64-bit control-block EA is refused");
    check(6u * PE_LANE_BYTES == 50688u, "six lanes' rings and control blocks = 50688 B");
    check(PE_RING_SLOTS >= 64 && (PE_RING_SLOTS & (PE_RING_SLOTS - 1)) == 0, "ring slots: a power of two >= 64");
    check(PE_IMAGE_BYTES >= 4096u && PE_IMAGE_BYTES <= 16777216u, "image bytes inside 4 KiB..16 MiB");
    check(PE_GPU_REPORT_COUNT >= 2 && PE_GPU_REPORT_FIRST > 1, "reports 32..63, outside the timestamps 0/1");
    check(PE_GPU_LABEL != 240u && PE_FENCE_LABEL != 240u && PE_FENCE_LABEL != PE_GPU_LABEL, "labels distinct");
}

/* ---- the fence (production pe_fence) ------------------------------------------------------------------------ */

typedef struct {
    volatile uint32_t labels[256];
    int written[256];
    int complete; /* the mock GPU completes the write */
    uint64_t now;
} fence_mock;
static volatile uint32_t *fm_addr(void *c, uint32_t l) { return &((fence_mock *)c)->labels[l]; }
static void fm_write(void *c, uint32_t l, uint32_t v)
{
    fence_mock *m = c;
    ++m->written[l];
    if (m->complete)
        m->labels[l] = v;
}
static void fm_flush(void *c) { (void)c; }
static void fm_barrier(void *c) { (void)c; }
static uint64_t fm_now(void *c) { return ((fence_mock *)c)->now; }
static void fm_sleep(void *c, unsigned us) { ((fence_mock *)c)->now += us; }

static void fence(void)
{
    static fence_mock m;
    memset((void *)&m, 0, sizeof m);
    m.complete = 1;
    m.labels[PE_GPU_LABEL] = 0xabcdu; /* the profiler's ticket value */
    pe_fence_io io = {&m, fm_addr, fm_write, fm_flush, fm_barrier, fm_now, fm_sleep};
    pe_fence_state f = {0};
    check(pe_fence(&f, &io) == 0 && pe_fence(&f, &io) == 0, "two fences complete");
    int others = 0;
    for (unsigned l = 0; l < 256; ++l)
        if (l != PE_FENCE_LABEL)
            others += m.written[l];
    check(m.written[PE_FENCE_LABEL] == 2 && others == 0, "the production fence writes only label 242");
    check(m.labels[PE_GPU_LABEL] == 0xabcdu, "label 241 (the profiler's ticket) is untouched");
    m.complete = 0;
    check(pe_fence(&f, &io) == -1 && m.now >= PE_WAIT_US, "no completion: a bounded timeout");
}

/* ---- the worker loop (production worker_proto) --------------------------------------------------------------- */

typedef struct {
    uint32_t words[16];
    unsigned n, at;
    int attach_rc, fini_rc, flush_rc_from; /* flush fails from the n-th call (0: never) */
    int dec_rcs[8];
    unsigned dec_at, flushes, zones_open, zones, works, no_works;
    uint32_t pub_status[32], pub_seq[32];
    unsigned pubs;
    char log[512];
} wmock;
static uint32_t wm_read(void *c)
{
    wmock *m = c;
    return m->at < m->n ? m->words[m->at++] : 0;
}
static int wm_attach(void *c)
{
    strcat(((wmock *)c)->log, "attach ");
    return ((wmock *)c)->attach_rc;
}
static int wm_flush(void *c)
{
    wmock *m = c;
    ++m->flushes;
    strcat(m->log, "flush ");
    return m->flush_rc_from && m->flushes >= (unsigned)m->flush_rc_from ? -1 : 0;
}
static int wm_dec(void *c)
{
    wmock *m = c;
    strcat(m->log, "dec ");
    return m->dec_at < 8 ? m->dec_rcs[m->dec_at++] : 0;
}
static int wm_fini(void *c)
{
    strcat(((wmock *)c)->log, "fini ");
    return ((wmock *)c)->fini_rc;
}
static uint32_t wm_zb(void *c)
{
    wmock *m = c;
    ++m->zones_open;
    strcat(m->log, "zone ");
    return 7;
}
static void wm_ze(void *c, uint32_t z)
{
    wmock *m = c;
    if (z == 7 && m->zones_open) {
        --m->zones_open;
        ++m->zones;
    }
    strcat(m->log, "end ");
}
static void wm_work(void *c)
{
    ++((wmock *)c)->works;
    strcat(((wmock *)c)->log, "work ");
}
static void wm_nowork(void *c)
{
    ++((wmock *)c)->no_works;
    strcat(((wmock *)c)->log, "nowork ");
}
static void wm_pub(void *c, uint32_t st, uint32_t seq)
{
    wmock *m = c;
    m->pub_status[m->pubs] = st;
    m->pub_seq[m->pubs++] = seq;
    strcat(m->log, "pub ");
}

static uint32_t wrun(wmock *m, int profiled)
{
    worker_io io = {m, wm_read, wm_attach, wm_flush, wm_dec, wm_fini, wm_zb, wm_ze, wm_work, wm_nowork, wm_pub};
    return worker_run(&io, profiled);
}

static void worker(void)
{
    wmock m;
    memset(&m, 0, sizeof m);
    m.words[0] = 1;
    m.words[1] = 2u | PE_FLAG_RESET;
    m.words[2] = 3u | PE_FLAG_RESET_ONLY;
    m.n = 3;
    uint32_t st = wrun(&m, 1);
    check(!strcmp(m.log, "attach flush dec zone work end flush pub flush dec zone work end flush pub flush dec nowork flush pub fini pub "),
          "the profiled worker's call order");
    check(st == 0 && m.pubs == 4 && m.pub_seq[3] == PE_SEQ_MASK && m.zones == 2 && m.zones_open == 0, "zones per job, final marker");

    memset(&m, 0, sizeof m);
    m.attach_rc = -5;
    st = wrun(&m, 1);
    check(st == PE_SPU_ATTACH_FAILED && m.pubs == 1 && m.pub_status[0] == PE_SPU_ATTACH_FAILED && !strstr(m.log, "fini"),
          "attach failure with no jobs: reported in the FINAL status, no fini");

    memset(&m, 0, sizeof m);
    m.fini_rc = -15;
    st = wrun(&m, 1);
    check(st == PE_SPU_FINI_FAILED && m.pub_status[m.pubs - 1] == PE_SPU_FINI_FAILED, "a failed fini in the final status");

    memset(&m, 0, sizeof m);
    m.dec_rcs[0] = 0;
    m.dec_rcs[1] = WP_EAGAIN;
    m.dec_rcs[2] = WP_EAGAIN;
    m.dec_rcs[3] = 0;
    m.words[0] = 1u | PE_FLAG_RESET;
    m.n = 1;
    st = wrun(&m, 1);
    check(st == 0 && m.dec_at == 4, "EAGAIN: flush and retry (bounded)");

    memset(&m, 0, sizeof m);
    for (int k = 1; k < 8; ++k)
        m.dec_rcs[k] = WP_EAGAIN;
    m.words[0] = 1u | PE_FLAG_RESET;
    m.words[1] = 2;
    m.n = 2;
    st = wrun(&m, 1);
    check((st & PE_SPU_RESET_FAILED) && m.pub_status[0] == PE_SPU_RESET_FAILED && m.zones == 0,
          "EAGAIN past the bound: reset failed, profiling off, no zones after");

    memset(&m, 0, sizeof m);
    m.flush_rc_from = 2; /* flush 1 (the attach-time reset) ok, job 1 flush fails */
    m.words[0] = 1;
    m.words[1] = 2;
    m.n = 2;
    st = wrun(&m, 1);
    check((st & PE_SPU_FLUSH_FAILED) && m.zones == 1 && m.works == 2, "a flush failure turns profiling off; work continues");

    memset(&m, 0, sizeof m);
    m.words[0] = 1;
    m.words[1] = 2u | PE_FLAG_RESET;
    m.n = 2;
    st = wrun(&m, 0);
    check(st == 0 && !strstr(m.log, "attach") && !strstr(m.log, "dec") && !strstr(m.log, "fini") && m.zones == 0 &&
              m.works == 2,
          "unprofiled: no writer calls at all");
}

/* ---- the pool protocol (production pool_proto) against mock slots ------------------------------------------ */

typedef struct {
    uint32_t seq[PE_MAX_WORKERS], status[PE_MAX_WORKERS];
    uint32_t final_status[PE_MAX_WORKERS];
    int marker[PE_MAX_WORKERS];   /* a worker publishes its final marker on the exit word */
    int fail_mb_at;               /* 1-based mailbox write that fails (0: none) */
    unsigned mbs;
    int join_rc, close_rc, joined, closed;
    uint64_t now;
    char log[1024];
    int barrier_before_status; /* every status read had a barrier before it since the last completion read */
    int seen_barrier;
} pmock;
static int pm_mb(void *c, unsigned i, uint32_t w)
{
    pmock *m = c;
    ++m->mbs;
    if (m->fail_mb_at && (int)m->mbs == m->fail_mb_at)
        return -1;
    if (w == 0 && m->marker[i]) {
        m->status[i] = m->final_status[i];
        m->seq[i] = PE_SEQ_MASK;
    }
    return 0;
}
static uint32_t pm_seq(void *c, unsigned i)
{
    pmock *m = c;
    m->seen_barrier = 0;
    return m->seq[i];
}
static uint32_t pm_status(void *c, unsigned i)
{
    pmock *m = c;
    if (!m->seen_barrier)
        m->barrier_before_status = 0;
    strcat(m->log, "status ");
    return m->status[i];
}
static void pm_barrier(void *c)
{
    ((pmock *)c)->seen_barrier = 1;
    strcat(((pmock *)c)->log, "barrier ");
}
static uint64_t pm_now(void *c) { return ((pmock *)c)->now; }
static void pm_sleep(void *c, unsigned us) { ((pmock *)c)->now += us; }
static int pm_join(void *c)
{
    ((pmock *)c)->joined = 1;
    strcat(((pmock *)c)->log, "join ");
    return ((pmock *)c)->join_rc;
}
static int pm_close(void *c)
{
    ((pmock *)c)->closed = 1;
    strcat(((pmock *)c)->log, "close ");
    return ((pmock *)c)->close_rc;
}
static void pm_log(void *c, const char *what, int v)
{
    (void)v;
    strcat(((pmock *)c)->log, "!");
    strcat(((pmock *)c)->log, what);
    strcat(((pmock *)c)->log, " ");
}

static pool_io pio(pmock *m)
{
    pool_io io = {m, pm_mb, pm_seq, pm_status, pm_barrier, pm_now, pm_sleep, pm_join, pm_close, pm_log};
    return io;
}

static void pool(void)
{
    pmock m;
    pe_epochs e;
    int join_ok;

    /* success: final statuses read after a barrier, join (no terminate), then close */
    memset(&m, 0, sizeof m);
    m.barrier_before_status = 1;
    m.marker[0] = m.marker[1] = 1;
    pool_io io = pio(&m);
    pe_epochs_init(&e, 2, 0);
    m.now = S(1);
    check(pp_close(&e, &io, 2, 1, &join_ok) == 0 && join_ok && e.w32_eligible, "close: markers, join, eligible");
    check(m.barrier_before_status && strstr(m.log, "barrier status status join close"), "barrier before final statuses");

    /* a failed fini (or attach) reported only in the FINAL status */
    for (int k = 0; k < 2; ++k) {
        memset(&m, 0, sizeof m);
        m.marker[0] = m.marker[1] = 1;
        m.final_status[1] = k ? PE_SPU_ATTACH_FAILED : PE_SPU_FINI_FAILED;
        io = pio(&m);
        pe_epochs_init(&e, 2, 0);
        pp_close(&e, &io, 2, 1, &join_ok);
        check(join_ok && !e.w32_eligible, k ? "final status: attach failure clears it" : "final status: fini failure clears it");
    }

    /* a missing marker: no join, terminate-close, lanes must be retained */
    memset(&m, 0, sizeof m);
    m.marker[0] = 1;
    io = pio(&m);
    pe_epochs_init(&e, 2, 0);
    pp_close(&e, &io, 2, 1, &join_ok);
    check(!join_ok && !m.joined && m.closed && !e.w32_eligible && strstr(m.log, "!final-markers-missing"),
          "a missing final marker: retained, cleared, terminate-close");
    check(m.now >= PE_DEADLINE_US, "the marker wait is bounded");

    /* a failed join after every marker */
    memset(&m, 0, sizeof m);
    m.marker[0] = m.marker[1] = 1;
    m.join_rc = -1;
    io = pio(&m);
    pe_epochs_init(&e, 2, 0);
    pp_close(&e, &io, 2, 1, &join_ok);
    check(!join_ok && m.joined && m.closed && !e.w32_eligible, "a failed join: retained, cleared");

    /* a failed close after a good join */
    memset(&m, 0, sizeof m);
    m.marker[0] = m.marker[1] = 1;
    m.close_rc = -1;
    io = pio(&m);
    pe_epochs_init(&e, 2, 0);
    check(pp_close(&e, &io, 2, 1, &join_ok) != 0 && !join_ok && !e.w32_eligible, "a failed close: not joined, cleared");

    /* never started: nothing sent, close only */
    memset(&m, 0, sizeof m);
    io = pio(&m);
    pe_epochs_init(&e, 2, 0);
    pp_close(&e, &io, 2, 0, &join_ok);
    check(!join_ok && m.mbs == 0 && m.closed, "not running: close only");

    /* a partial mailbox send */
    memset(&m, 0, sizeof m);
    m.fail_mb_at = 2;
    io = pio(&m);
    pe_epochs_init(&e, 2, 0);
    check(pp_submit(&e, &io, 2, 5) == -1 && !e.w32_eligible, "a partial send clears it");

    /* completion statuses after a barrier; an SPU reset failure reported by a job */
    memset(&m, 0, sizeof m);
    m.barrier_before_status = 1;
    io = pio(&m);
    pe_epochs_init(&e, 2, 0);
    m.now = S(1);
    pp_submit(&e, &io, 2, 5);
    m.seq[0] = m.seq[1] = 5;
    m.status[1] = PE_SPU_RESET_FAILED;
    m.now += 1000;
    pp_done(&e, &io, 2);
    check(m.barrier_before_status && !e.w32_eligible, "pp_done: barrier first; a job's reset failure clears it");

    /* keep-alive: only due workers, bounded wait */
    memset(&m, 0, sizeof m);
    io = pio(&m);
    pe_epochs_init(&e, 2, 0);
    m.now = S(5);
    check(pp_keepalive(&e, &io, 2, 0x20000001u) == 0 && m.mbs == 0, "keep-alive: nothing due, nothing sent");
    m.now = S(20);
    m.seq[0] = m.seq[1] = 0x20000002u; /* the mock completes immediately */
    check(pp_keepalive(&e, &io, 2, 0x20000002u) == 0 && m.mbs == 2 && e.w32_eligible, "keep-alive: due workers reset");
    m.now = S(40) + 1;
    check(pp_keepalive(&e, &io, 2, 0x20000003u) == -1 && !e.w32_eligible, "keep-alive never acknowledged: deadline");

    pe_epochs_init(&e, 1, 0);
    pp_failed(&e, "the SPE job deadline");
    check(!e.w32_eligible, "the caller's deadline clears it");
}

/* ---- end to end: the real worker loop behind a mailbox, the real pool protocol in front ---------------------- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t q[64];
    unsigned head, tail;
    _Atomic uint32_t seq, status;
    int dec_fail_after; /* the n-th dec_write fails (0: never) */
    unsigned decs;
} e2e;
static e2e g;
static uint32_t e_read(void *c)
{
    (void)c;
    pthread_mutex_lock(&g.mu);
    while (g.head == g.tail)
        pthread_cond_wait(&g.cv, &g.mu);
    uint32_t w = g.q[g.head++ % 64];
    pthread_mutex_unlock(&g.mu);
    return w;
}
static int e_ok(void *c) { (void)c; return 0; }
static int e_dec(void *c)
{
    (void)c;
    ++g.decs;
    return g.dec_fail_after && g.decs >= (unsigned)g.dec_fail_after ? -5 : 0;
}
static uint32_t e_zb(void *c) { (void)c; return 1; }
static void e_ze(void *c, uint32_t z) { (void)c; (void)z; }
static void e_nop(void *c) { (void)c; }
static void e_pub(void *c, uint32_t st, uint32_t seq)
{
    (void)c;
    atomic_store(&g.status, st);
    atomic_store(&g.seq, seq);
}
static void *worker_thread(void *arg)
{
    (void)arg;
    static const worker_io io = {0, e_read, e_ok, e_ok, e_dec, e_ok, e_zb, e_ze, e_nop, e_nop, e_pub};
    worker_run(&io, 1);
    return NULL;
}
static int r_mb(void *c, unsigned i, uint32_t w)
{
    (void)c;
    (void)i;
    pthread_mutex_lock(&g.mu);
    g.q[g.tail++ % 64] = w;
    pthread_cond_signal(&g.cv);
    pthread_mutex_unlock(&g.mu);
    return 0;
}
static uint32_t r_seq(void *c, unsigned i) { (void)c; (void)i; return atomic_load(&g.seq); }
static uint32_t r_status(void *c, unsigned i) { (void)c; (void)i; return atomic_load(&g.status); }
static void r_barrier(void *c) { (void)c; atomic_thread_fence(memory_order_seq_cst); }
static uint64_t r_now(void *c)
{
    (void)c;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
}
static void r_sleep(void *c, unsigned us)
{
    (void)c;
    struct timespec t = {0, (long)us * 1000L};
    nanosleep(&t, NULL);
}
static pthread_t g_thread;
static int r_join(void *c)
{
    (void)c;
    return pthread_join(g_thread, NULL);
}
static int r_close(void *c) { (void)c; return 0; }
static void r_log(void *c, const char *w, int v) { (void)c; printf("  e2e log %s %d\n", w, v); }

static int e2e_run(int dec_fail_after)
{
    memset(&g, 0, sizeof g);
    pthread_mutex_init(&g.mu, NULL);
    pthread_cond_init(&g.cv, NULL);
    g.dec_fail_after = dec_fail_after;
    pool_io io = {0, r_mb, r_seq, r_status, r_barrier, r_now, r_sleep, r_join, r_close, r_log};
    pe_epochs e;
    pe_epochs_init(&e, 1, r_now(0));
    pthread_create(&g_thread, NULL, worker_thread, NULL);
    for (uint32_t s = 1; s <= 3; ++s) {
        if (pp_submit(&e, &io, 1, s))
            return -1;
        uint64_t t = r_now(0);
        while (r_seq(0, 0) != s)
            if (r_now(0) - t > PE_DEADLINE_US)
                return -1;
        pp_done(&e, &io, 1);
    }
    int join_ok = 0;
    pp_close(&e, &io, 1, 1, &join_ok);
    return join_ok ? (e.w32_eligible ? 1 : 0) : -1;
}

static void end_to_end(void)
{
    check(e2e_run(0) == 1, "e2e: three jobs, final marker, join: eligible");
    check(e2e_run(1) == 0, "e2e: the attach-time reset fails on the SPU: the first completion clears it");
}


/* ---- the run receipts (production pe_receipt_* / pe_end_line) -------------------------------------------------
 * A mock filesystem: files by name with their bytes; the acceptance file must exist only after a published record. */

typedef struct {
    char name[4][64];
    char data[4][512];
    unsigned len[4];
    int used[4];
    int fd_file; /* the open fd's file index (one at a time) */
    int open_rc_fail, fsync_rc, close_rc, rename_rc;
    long chunk, fail_at, fail_value;
    int writes, closes, unlinks;
} fsmock;
static int fs_find(fsmock *m, const char *n)
{
    for (int i = 0; i < 4; ++i)
        if (m->used[i] && !strcmp(m->name[i], n))
            return i;
    return -1;
}
static int fs_make(fsmock *m, const char *n, int trunc)
{
    int i = fs_find(m, n);
    if (i < 0) {
        for (i = 0; i < 4 && m->used[i]; ++i)
            ;
        if (i == 4)
            return -1;
        m->used[i] = 1;
        strcpy(m->name[i], n);
        m->len[i] = 0;
    } else if (trunc) {
        m->len[i] = 0;
    }
    return i;
}
static int fs_open_append(void *c, const char *n)
{
    fsmock *m = c;
    if (m->open_rc_fail)
        return -1;
    m->fd_file = fs_make(m, n, 0);
    return 3;
}
static int fs_open_trunc(void *c, const char *n)
{
    fsmock *m = c;
    if (m->open_rc_fail)
        return -1;
    m->fd_file = fs_make(m, n, 1);
    return 3;
}
static long fs_write(void *c, int fd, const void *b, unsigned long n)
{
    fsmock *m = c;
    (void)fd;
    ++m->writes;
    if (m->fail_at && m->writes == m->fail_at)
        return m->fail_value;
    unsigned long k = m->chunk && (unsigned long)m->chunk < n ? (unsigned long)m->chunk : n;
    memcpy(m->data[m->fd_file] + m->len[m->fd_file], b, k);
    m->len[m->fd_file] += (unsigned)k;
    return (long)k;
}
static int fs_fsync(void *c, int fd) { (void)fd; return ((fsmock *)c)->fsync_rc; }
static int fs_close(void *c, int fd)
{
    (void)fd;
    ++((fsmock *)c)->closes;
    return ((fsmock *)c)->close_rc;
}
static int fs_rename(void *c, const char *from, const char *to)
{
    fsmock *m = c;
    if (m->rename_rc)
        return m->rename_rc;
    int i = fs_find(m, from);
    if (i < 0)
        return -1;
    int j = fs_find(m, to);
    if (j >= 0)
        m->used[j] = 0;
    strcpy(m->name[i], to);
    return 0;
}
static int fs_unlink(void *c, const char *n)
{
    fsmock *m = c;
    int i = fs_find(m, n);
    ++m->unlinks;
    if (i < 0)
        return -1;
    m->used[i] = 0;
    return 0;
}

#define FINAL "/dev_hdd0/tmp/h-1.ext-end.txt"
#define TMP "/dev_hdd0/tmp/h-1.ext-end.tmp"
#define LINE "WORKLOAD_PROF_EXT_END join=1 w32_eligible=1 rc=0 frames=1 checked=1 phase_mask=15 why=- receipt=complete" "\n"

static void receipt(void)
{
    char path[64];
    check(pe_receipt_path("/dev_hdd0/tmp/heterogeneous-00ab.csv", ".ext-end.txt", path, sizeof path) == 0 &&
              !strcmp(path, "/dev_hdd0/tmp/heterogeneous-00ab.ext-end.txt"),
          "receipt path: .csv -> .ext-end.txt");
    check(pe_receipt_path("/dev_hdd0/tmp/x.prof", ".ext.txt", path, sizeof path) == -1, "receipt path: not a .csv");
    check(pe_receipt_path("/dev_hdd0/tmp/heterogeneous-00ab.csv", ".ext.txt", path, 20) == -1, "receipt path: too long");

    /* late CSV finalization: the line built after the finalization carries the final rc */
    char line[224];
    int rc = 1;
    int n = pe_end_line(line, sizeof line, 1, 1, NULL, rc, 7200, 7200, 15);
    check(n > 0 && strstr(line, " rc=1 ") && strstr(line, " phase_mask=15 ") && strstr(line, " join=1 ") &&
              strstr(line, " w32_eligible=1 ") && strstr(line, " frames=7200 ") && strstr(line, " checked=7200 "),
          "the END line carries the final rc and every acceptance field");
    check(n > 0 && !strcmp(line + n - (int)strlen("receipt=complete" "\n"), "receipt=complete" "\n"), "completion token last");
    check(pe_end_line(line, 40, 1, 1, NULL, 0, 1, 1, 15) == -1, "a truncated END line is refused, never written");

    fsmock m;
    pe_file_io io = {&m, fs_open_append, fs_open_trunc, fs_write, fs_fsync, fs_close, fs_rename, fs_unlink};

    /* published: the final file holds exactly the line; no temporary remains */
    memset(&m, 0, sizeof m);
    m.chunk = 5;
    check(pe_receipt_publish(&io, TMP, FINAL, LINE) == 0, "publish: success");
    int f = fs_find(&m, FINAL);
    check(f >= 0 && m.len[f] == strlen(LINE) && !memcmp(m.data[f], LINE, m.len[f]) && fs_find(&m, TMP) < 0,
          "publish: the acceptance file holds the complete record (short writes continued), no temporary left");

    /* every failure after the full bytes were written leaves NO acceptance file */
    const char *what[] = {"fsync", "close", "rename"};
    for (int k = 0; k < 3; ++k) {
        memset(&m, 0, sizeof m);
        if (k == 0) m.fsync_rc = -1;
        if (k == 1) m.close_rc = -1;
        if (k == 2) m.rename_rc = -1;
        int r = pe_receipt_publish(&io, TMP, FINAL, LINE);
        int t = fs_find(&m, TMP);
        (void)t;
        char name[96];
        snprintf(name, sizeof name, "publish: %s failure after the full bytes: no acceptance file, temporary unlinked", what[k]);
        check(r == -1 && fs_find(&m, FINAL) < 0 && fs_find(&m, TMP) < 0 && m.unlinks == 1 && m.writes >= 1, name);
    }
    /* a partial write, a zero-byte write, an open failure: no acceptance file either */
    memset(&m, 0, sizeof m);
    m.chunk = 4;
    m.fail_at = 2;
    m.fail_value = -1;
    check(pe_receipt_publish(&io, TMP, FINAL, LINE) == -1 && fs_find(&m, FINAL) < 0 && m.closes == 1,
          "publish: a partial write then failure: no acceptance file, still closed");
    memset(&m, 0, sizeof m);
    m.fail_at = 1;
    m.fail_value = 0;
    check(pe_receipt_publish(&io, TMP, FINAL, LINE) == -1 && fs_find(&m, FINAL) < 0, "publish: zero-byte write");
    memset(&m, 0, sizeof m);
    m.open_rc_fail = 1;
    check(pe_receipt_publish(&io, TMP, FINAL, LINE) == -1 && fs_find(&m, FINAL) < 0, "publish: open failure");
    /* a stale temporary from an earlier attempt is truncated, never published as is */
    memset(&m, 0, sizeof m);
    int st = fs_make(&m, TMP, 0);
    memcpy(m.data[st], "stale garbage", 13);
    m.len[st] = 13;
    check(pe_receipt_publish(&io, TMP, FINAL, LINE) == 0 && m.len[fs_find(&m, FINAL)] == strlen(LINE),
          "publish: a stale temporary is truncated first");

    /* start notes (append, informational): checked the same way */
    memset(&m, 0, sizeof m);
    m.chunk = 3;
    check(pe_receipt_write(&io, "n", "WORKLOAD_PROF_EXT spu=1" "\n") == 0 && m.closes == 1, "note: appended completely");
    memset(&m, 0, sizeof m);
    m.fsync_rc = -1;
    check(pe_receipt_write(&io, "n", "x" "\n") == -1 && m.closes == 1, "note: fsync failure refused, closed");
}

/* ---- lifecycle (the port order) ---------------------------------------------------------------------------- */

typedef struct {
    char log[2048];
    const char *fail;
    int fail_from;
    int counts[16];
    uint32_t next_handle;
} mock;
static int mk(mock *m, const char *name, int idx)
{
    strcat(m->log, name);
    strcat(m->log, " ");
    int n = ++m->counts[idx];
    return m->fail && !strcmp(m->fail, name) && (m->fail_from == 0 || m->fail_from == n) ? -5 : 0;
}
static int m_init(void *c, unsigned l) { (void)l; return mk(c, "init", 0); }
static int m_tb(void *c) { return mk(c, "tbegin", 1); }
static int m_reg(void *c, uint32_t *img) { *img = 7; return mk(c, "reg", 2); }
static int m_lc(void *c, uint32_t img, pe_lane *out)
{
    mock *m = c;
    (void)img;
    int rc = mk(c, "lane", 3);
    if (!rc)
        out->handle = ++m->next_handle;
    return rc;
}
static int m_le(void *c, uint32_t h) { (void)h; return mk(c, "lend", 4); }
static int m_ga(void *c, uint32_t *gid) { *gid = 1; return mk(c, "attach", 5); }
static int m_gc(void *c, uint32_t gid) { (void)gid; return mk(c, "cal", 6); }
static int m_dr(void *c) { return mk(c, "drain", 7); }
static int m_fe(void *c) { return mk(c, "fence", 8); }
static int m_gd(void *c, uint32_t gid) { (void)gid; return mk(c, "detach", 9); }
static int m_gr(void *c, uint32_t gid) { (void)gid; return mk(c, "release", 10); }
static int m_te(void *c) { return mk(c, "tend", 11); }
static int m_sd(void *c) { return mk(c, "shutdown", 12); }
static void m_log(void *c, const char *what, int rc)
{
    (void)rc;
    strcat(((mock *)c)->log, "!");
    strcat(((mock *)c)->log, what);
    strcat(((mock *)c)->log, " ");
}
static const char *run(mock *m, const char *fail, int nth, unsigned workers, int join_ok, int *stop_rc)
{
    memset(m, 0, sizeof *m);
    m->fail = fail;
    m->fail_from = nth;
    pe_port p = {m, m_init, m_tb, m_reg, m_lc, m_le, m_ga, m_gc, m_dr, m_fe, m_gd, m_gr, m_te, m_sd, m_log};
    pe_state s;
    if (pe_start(&s, &p, workers) != 0) {
        *stop_rc = -1;
        return m->log;
    }
    pe_gpu_start(&s, &p);
    pe_gpu_calibrate(&s, &p);
    *stop_rc = pe_stop(&s, &p, join_ok);
    return m->log;
}
static int has(const char *log, const char *seq) { return strstr(log, seq) != NULL; }

static void worker_args(void)
{
    pmock m;
    pe_epochs e;
    pe_args a[3];
    pool_io io = pio(&m);
    pe_lane good0 = {1, 1024, 0x30000080u, 0x30010000u}, good1 = {2, 1024, 0x30020000u, 0x30030000u};
    pe_lane high = {2, 1024, 0x30020000u, 0x100000000ull}, odd = {2, 1024, 0x30020040u, 0x30030000u};
    uint64_t slot[3] = {0x10002000u, 0x10002100u, 0x10002200u};

    memset(&m, 0, sizeof m);
    pe_epochs_init(&e, 2, 0);
    const pe_lane *both[3] = {&good0, &good1, NULL};
    check(pp_worker_args(&e, &io, slot, both, 2, a) == 0 && e.w32_eligible && !m.log[0], "args: two lanes pack");
    check(pe_unpack_ext(a[1]).ring_ea == 0x30020000u && pe_unpack_ext(a[1]).ctl_ea == 0x30030000u &&
              pe_unpack_ext(a[1]).slot_ea == 0x10002100u && pe_unpack_ext(a[1]).lane == 1 && pe_unpack_ext(a[1]).lanes == 2,
          "args: worker 1's words are its lane's");

    /* a control-block EA above 4 GiB: refused loudly, the worker unprofiled, eligibility cleared, SPU reported off */
    memset(&m, 0, sizeof m);
    pe_epochs_init(&e, 2, 0);
    const pe_lane *hi[3] = {&good0, &high, NULL};
    check(pp_worker_args(&e, &io, slot, hi, 2, a) == 1, "args: a 64-bit control-block EA returns 1 (SPU off)");
    check(strstr(m.log, "!pack-refused") != NULL, "args: the refusal is logged (no silent fallback)");
    check(!e.w32_eligible && e.why && strstr(e.why, "argument packing refused"), "args: a refusal clears W32 eligibility");
    check(pe_unpack_ext(a[1]).ring_ea == 0 && pe_unpack_ext(a[1]).slot_ea == 0x10002100u,
          "args: the refused worker gets the unprofiled words");
    check(pe_unpack_ext(a[0]).ring_ea == 0x30000080u, "args: the other worker keeps its lane");
    memset(&m, 0, sizeof m);
    pe_epochs_init(&e, 2, 0);
    const pe_lane *mis[3] = {&good0, &odd, NULL};
    check(pp_worker_args(&e, &io, slot, mis, 2, a) == 1 && !e.w32_eligible && strstr(m.log, "!pack-refused"),
          "args: a misaligned ring EA is refused the same way");

    /* no lane at all: unprofiled words, nothing refused (the caller reports "SPU lanes are off") */
    memset(&m, 0, sizeof m);
    pe_epochs_init(&e, 2, 0);
    const pe_lane *none[3] = {NULL, NULL, NULL};
    check(pp_worker_args(&e, &io, slot, none, 2, a) == 0 && e.w32_eligible && !m.log[0] &&
              pe_unpack_ext(a[0]).ring_ea == 0 && pe_unpack_ext(a[1]).ring_ea == 0,
          "args: without lanes the workers run unprofiled, nothing refused");

    /* a work slot above 4 GiB: nothing the SPU program can use; the pool must not start */
    memset(&m, 0, sizeof m);
    pe_epochs_init(&e, 2, 0);
    uint64_t slot_hi[3] = {0x10002000u, 0x100002100ull, 0};
    check(pp_worker_args(&e, &io, slot_hi, both, 2, a) == -1 && strstr(m.log, "!slot-pack-refused") && !e.w32_eligible,
          "args: a 64-bit work slot EA refuses the start");

    /* pe_spu_off: reported off and logged; the lanes stay live and are still ended after the join */
    mock lm;
    memset(&lm, 0, sizeof lm);
    pe_port lp = {&lm, m_init, m_tb, m_reg, m_lc, m_le, m_ga, m_gc, m_dr, m_fe, m_gd, m_gr, m_te, m_sd, m_log};
    pe_state s;
    check(pe_start(&s, &lp, 2) == 0 && s.spu == 1, "spu-off: lanes up");
    pe_spu_off(&s, &lp, "spu-off-pack-refused");
    check(s.spu == 0 && strstr(lm.log, "!spu-off-pack-refused"), "spu-off: SPU reported off, logged");
    pe_spu_off(&s, &lp, "again");
    check(!strstr(lm.log, "!again"), "spu-off: logged once");
    check(pe_stop(&s, &lp, 1) == 0 && lm.counts[4] == 2, "spu-off: both lanes still ended after the join");
}

static void lifecycle(void)
{
    mock m;
    int rc;
    const char *l = run(&m, NULL, 0, 2, 1, &rc);
    check(!strcmp(l, "init tbegin reg lane lane fence attach cal drain cal drain fence detach release lend lend tend shutdown ") &&
              rc == 0,
          "the success order");
    l = run(&m, "init", 0, 2, 1, &rc);
    check(!strcmp(l, "init !init-ex ") && rc == -1, "init failure: nothing else");
    l = run(&m, "tbegin", 0, 2, 1, &rc);
    check(has(l, "!thread-begin") && !has(l, "reg") && !has(l, "attach") && !has(l, "tend") && has(l, "shutdown"),
          "thread-begin failure");
    l = run(&m, "reg", 0, 2, 1, &rc);
    check(!has(l, "lane ") && has(l, "attach") && !has(l, "lend"), "registration failure: no lanes, GPU still");
    l = run(&m, "lane", 2, 3, 1, &rc);
    check(has(l, "lane lane !lane-create") && m.counts[4] == 1, "second lane fails: the first ends after the join");
    l = run(&m, "fence", 1, 2, 1, &rc);
    check(has(l, "!fence-quiesce") && !has(l, "attach") && !has(l, "release"), "no quiescence proof: no attach");
    l = run(&m, "attach", 0, 2, 1, &rc);
    check(has(l, "!gpu-attach") && !has(l, "detach") && rc == 0, "attach failure: GPU off");
    l = run(&m, "cal", 0, 2, 1, &rc);
    check(has(l, "!gpu-calibrate") && has(l, "release") && rc == 0, "a calibration failure is never fatal");
    l = run(&m, "fence", 2, 2, 1, &rc);
    check(has(l, "!fence-final") && has(l, "detach") && !has(l, "release") && has(l, "!gpu-retained") &&
              has(l, "!shutdown-skipped") && rc != 0,
          "final fence timeout: never release, no shutdown");
    l = run(&m, "detach", 0, 2, 1, &rc);
    check(!has(l, "release") && has(l, "!gpu-retained") && rc != 0, "detach failure: never release");
    l = run(&m, NULL, 0, 2, 0, &rc);
    check(!has(l, "lend") && has(l, "!lanes-retained") && has(l, "!shutdown-skipped") && rc != 0,
          "join failure: lanes retained, no shutdown");
    l = run(&m, "lend", 1, 2, 1, &rc);
    check(m.counts[4] == 2 && has(l, "!lane-end") && rc != 0, "one lane_end failure; the other still ends");
    l = run(&m, "release", 0, 2, 1, &rc);
    check(has(l, "!gpu-release") && has(l, "shutdown") && rc != 0, "a release failure is reported");
}

int main(void)
{
    epochs();
    args();
    fence();
    worker();
    pool();
    worker_args();
    end_to_end();
    receipt();
    lifecycle();
    printf("%u tests, %u failures\n", tests, failures);
    return failures != 0;
}
