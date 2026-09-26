/* Host test of the SPU libsync sources over a simulated MFC
 * (tests/sdk/fixtures/spu-mfc-mock).  Built and run by
 * tests/sdk/libsync-spu-host-test.sh.
 *
 *  - Queue and Rwm Initialize leave their whole descriptor in memory (the
 *    old code staged it in the LS line and then re-read the line over it).
 *  - The mutex is the PPU's 32-bit ticket word.  Scripted rows run the
 *    SPU mutex.c code over the simulated MFC against a PPU-side model
 *    (big-endian 16-bit current/next counters at bytes 0 and 2, updated as
 *    foreign stores).  The random interleaving steps the ticket helpers
 *    (mutex_ticket.h) against that PPU model; it is a model of the
 *    protocol, not a run of real MFC locks on hardware.
 *  - Barrier Initialize refuses 0 and counts above 32767.
 *  - Reservation loss.  A putllc is lost either because another processor
 *    changed the line between getllar and putllc (scripted) or with no
 *    change.  Entry points exercised under loss:
 *      with a concurrent change: QueueInitialize, QueuePush (wlock and
 *        commit), QueuePop, RwmInitialize, RwmReadBegin, RwmReadEnd,
 *        MutexLock, MutexUnlock, MutexTryLock, BarrierNotify;
 *      with the first (or the commit / release) store lost: QueueTryPush,
 *        QueuePeek, QueueTryPeek, QueueTryPop, QueueClear, RwmWrite,
 *        RwmTryWrite, RwmTryReadBegin, BarrierTryNotify;
 *      alternate-loss rerun (every other putllc lost) of the descriptor
 *        and barrier sections: QueueInitialize/Push/Pop,
 *        RwmInitialize/Write/ReadBegin/ReadEnd, BarrierInitialize/Notify.
 *    QueueSize, BarrierWait and BarrierTryWait only read (no putllc).
 *    Looping forms must retry and converge without a stale write; the
 *    single-attempt Try forms must report AGAIN and change nothing.
 *
 * Limits of the simulation: DMA completes synchronously (no command runs
 * concurrently with SPU code, so ordering between a put and the next
 * reservation is not tested), and with 32-bit swapping the order of the
 * barrier's two 16-bit halves inside its word follows the host, so the
 * barrier rows check values, not the SPU/PPU half order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/sync.h>
#include <cell/sync/queue_types.h>
#include <cell/sync/barrier_types.h>
#include "mfc_mock.h"
#include "mutex_ticket.h"

static int failures, checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        ++checks;                                                           \
        if (!(cond)) {                                                      \
            ++failures;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

static uint64_t native64(uint64_t ea)
{
    uint64_t v;

    memcpy(&v, mfc_mock_memory + ea, 8);
    return v;
}

/* ---- queue and rwm descriptors -------------------------------------- */

static void test_descriptors(void)
{
    static uint8_t in[128] __attribute__((aligned(128)));
    static uint8_t out[128] __attribute__((aligned(128)));
    const uint64_t q = 0x1000, rwm = 0x2000;
    unsigned i;

    mfc_mock_swap32 = 0;
    memset(mfc_mock_memory + q, 0xab, 0x800);
    CHECK(cellSyncQueueInitialize(q, q + 128, 16, 4, 5) == CELL_OK, "queue init");
    CHECK(native64(q) == 0, "queue head zero");
    CHECK(native64(q + 8) == ((uint64_t)16 << 32 | 4), "queue size/depth survive: %016llx",
          (unsigned long long)native64(q + 8));
    CHECK(native64(q + 16) == q + 128, "queue buffer ea survives: %llx",
          (unsigned long long)native64(q + 16));
    CHECK(native64(q + 24) == 5, "queue tag survives");
    for (i = 0; i < 3; ++i) {
        memset(in, 0x10 + i, 16);
        CHECK(cellSyncQueuePush(q, in, 7) == CELL_OK, "push %u", i);
    }
    CHECK(cellSyncQueueSize(q) == 3, "size 3");
    for (i = 0; i < 3; ++i) {
        memset(out, 0, 16);
        CHECK(cellSyncQueuePop(q, out, 7) == CELL_OK && out[0] == 0x10 + i && out[15] == 0x10 + i,
              "pop %u in order (got %02x)", i, out[0]);
    }
    CHECK(cellSyncQueueTryPop(q, out, 7) == CELL_SYNC_ERROR_AGAIN, "empty");
    CHECK(cellSyncQueuePush(q, in, 32) == CELL_SYNC_ERROR_INVAL, "tag 32 refused");

    memset(mfc_mock_memory + rwm, 0xcd, 0x200);
    CHECK(cellSyncRwmInitialize(rwm, rwm + 128, 64, 6) == CELL_OK, "rwm init");
    CHECK(native64(rwm) == 64, "rwm lock word zero, size 64: %016llx", (unsigned long long)native64(rwm));
    CHECK(native64(rwm + 8) == rwm + 128, "rwm buffer ea survives: %llx",
          (unsigned long long)native64(rwm + 8));
    memset(in, 0x5a, 64);
    CHECK(cellSyncRwmWrite(rwm, in, 6) == CELL_OK, "rwm write");
    CHECK(mfc_mock_memory[rwm + 128] == 0x5a && mfc_mock_memory[rwm + 128 + 63] == 0x5a,
          "rwm write reached the buffer");
    memset(out, 0, 64);
    CHECK(cellSyncRwmReadBegin(rwm, out, 6) == CELL_OK && out[0] == 0x5a && out[63] == 0x5a,
          "rwm read back");
    CHECK(cellSyncRwmReadEnd(rwm, 6) == CELL_OK, "rwm read end");
}

/* ---- mutex: PPU-side model ------------------------------------------ */

/* The PPU view: big-endian u16 current_ticket at +0, next_ticket at +2,
 * written as foreign stores (they break SPU reservations). */
static unsigned ppu_current(uint64_t ea)
{
    return (unsigned)mfc_mock_memory[ea] << 8 | mfc_mock_memory[ea + 1];
}

static unsigned ppu_next(uint64_t ea)
{
    return (unsigned)mfc_mock_memory[ea + 2] << 8 | mfc_mock_memory[ea + 3];
}

static void ppu_store(uint64_t ea, unsigned current, unsigned next)
{
    uint8_t b[4] = { (uint8_t)(current >> 8), (uint8_t)current,
                     (uint8_t)(next >> 8), (uint8_t)next };

    mfc_mock_foreign_store(ea, b, 4);
}

static unsigned ppu_take_ticket(uint64_t ea)
{
    unsigned ticket = ppu_next(ea);

    ppu_store(ea, ppu_current(ea), (ticket + 1) & 0xffff);
    return ticket;
}

static int ppu_try_lock(uint64_t ea)
{
    if (ppu_current(ea) != ppu_next(ea))
        return CELL_SYNC_ERROR_BUSY;
    (void)ppu_take_ticket(ea);
    return CELL_OK;
}

static void ppu_unlock(uint64_t ea)
{
    ppu_store(ea, (ppu_current(ea) + 1) & 0xffff, ppu_next(ea));
}

/* While the SPU spins, act as the PPU releasing after a few reads. */
static uint64_t hook_ea;
static int hook_countdown, hook_fired;

static void release_after_spins(uint64_t line)
{
    if (line != (hook_ea & ~(uint64_t)127) || hook_fired)
        return;
    if (--hook_countdown == 0) {
        ppu_unlock(hook_ea);
        hook_fired = 1;
    }
}

static void test_mutex_script(void)
{
    const uint64_t m = 0x3000 + 8;      /* 4-byte aligned, not line aligned */
    static const uint8_t old_binary_held[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };

    mfc_mock_swap32 = 1;
    memset(mfc_mock_memory + 0x3000, 0x77, 128);

    /* The layout the old SPU code used, a 64-bit 1 for "held", reads as
     * a free ticket mutex on the PPU: the defect this protocol fixes. */
    memcpy(mfc_mock_memory + m, old_binary_held, 8);
    CHECK(ppu_current(m) == ppu_next(m), "old binary layout looks free to the PPU (negative control)");

    CHECK(cellSyncMutexInitialize(m, 0) == CELL_OK && ppu_current(m) == 0 && ppu_next(m) == 0, "init");
    CHECK(memcmp(mfc_mock_memory + m + 4, old_binary_held + 4, 4) == 0
          && mfc_mock_memory[m - 1] == 0x77 && mfc_mock_memory[m + 8] == 0x77, "only the word written");
    CHECK(cellSyncMutexInitialize(m + 2, 0) == (int)CELL_SYNC_ERROR_ALIGN, "2-byte alignment refused");

    /* PPU holds; SPU TryLock refuses; SPU Lock queues and waits. */
    CHECK(ppu_take_ticket(m) == 0, "PPU ticket 0");
    CHECK(cellSyncMutexTryLock(m) == CELL_SYNC_ERROR_BUSY && ppu_next(m) == 1, "SPU TryLock BUSY, word unchanged");
    hook_ea = m;
    hook_countdown = 6;
    hook_fired = 0;
    mfc_mock_on_reserve = release_after_spins;
    CHECK(cellSyncMutexLock(m) == CELL_OK, "SPU Lock");
    mfc_mock_on_reserve = NULL;
    CHECK(hook_fired, "SPU Lock returned only after the PPU released");
    CHECK(ppu_current(m) == 1 && ppu_next(m) == 2, "SPU holds ticket 1 (%u,%u)", ppu_current(m), ppu_next(m));

    /* SPU holds; PPU TryLock refuses; PPU queues ticket 2; SPU Unlock admits it. */
    CHECK(ppu_try_lock(m) == CELL_SYNC_ERROR_BUSY, "PPU TryLock BUSY while SPU holds");
    CHECK(ppu_take_ticket(m) == 2, "PPU waits with ticket 2");
    CHECK(cellSyncMutexUnlock(m) == CELL_OK && ppu_current(m) == 2, "SPU Unlock admits ticket 2");
    CHECK(cellSyncMutexTryLock(m) == CELL_SYNC_ERROR_BUSY, "SPU TryLock BUSY while PPU holds");
    ppu_unlock(m);
    CHECK(cellSyncMutexTryLock(m) == CELL_OK && ppu_current(m) == 3 && ppu_next(m) == 4, "SPU TryLock on free");
    CHECK(cellSyncMutexUnlock(m) == CELL_OK && ppu_current(m) == 4 && ppu_next(m) == 4, "free again");

    /* 16-bit wrap. */
    ppu_store(m, 0xffff, 0xffff);
    CHECK(cellSyncMutexTryLock(m) == CELL_OK && ppu_current(m) == 0xffff && ppu_next(m) == 0, "next wraps");
    CHECK(ppu_try_lock(m) == CELL_SYNC_ERROR_BUSY, "held across the wrap");
    CHECK(cellSyncMutexUnlock(m) == CELL_OK && ppu_current(m) == 0 && ppu_next(m) == 0, "current wraps");
}

/* Random interleaving of PPU-model agents and SPU agents stepping the
 * shared transitions (the ones mutex.c applies under a reservation). */
enum { IDLE, WAITING, INSIDE };

static void test_mutex_interleaving(void)
{
    enum { AGENTS = 6, ROUNDS = 400 };
    const uint64_t m = 0x4000 + 4;
    struct { int ppu, state, done; unsigned ticket; } a[AGENTS];
    uint64_t rng = 0x243f6a8885a308d3ull;
    unsigned expected_ticket = 0, inside = 0, entries = 0, steps = 0;
    int i, finished = 0, broken = 0;

    mfc_mock_swap32 = 1;
    CHECK(cellSyncMutexInitialize(m, 0) == CELL_OK, "init");
    for (i = 0; i < AGENTS; ++i) {
        a[i].ppu = i & 1;
        a[i].state = IDLE;
        a[i].done = 0;
    }
    while (finished < AGENTS && steps < 10000000u && !broken) {
        int k;
        uint32_t word;

        ++steps;
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        k = (int)(rng % AGENTS);
        if (a[k].done == ROUNDS && a[k].state == IDLE)
            continue;
        /* The SPU agent reads the word as the SPU does: a big-endian u32. */
        word = (uint32_t)ppu_current(m) << 16 | ppu_next(m);
        switch (a[k].state) {
        case IDLE:
            if (a[k].ppu) {
                a[k].ticket = ppu_take_ticket(m);
            } else {
                uint32_t next = __sync_mutex_take_ticket(word);

                a[k].ticket = __sync_mutex_next(word);
                ppu_store(m, __sync_mutex_current(next), __sync_mutex_next(next));
            }
            a[k].state = WAITING;
            break;
        case WAITING:
            if ((a[k].ppu ? ppu_current(m) : __sync_mutex_current(word)) == a[k].ticket) {
                if (inside++ != 0 || a[k].ticket != (expected_ticket & 0xffff))
                    broken = 1;
                ++expected_ticket;
                ++entries;
                a[k].state = INSIDE;
            }
            break;
        case INSIDE:
            --inside;
            if (a[k].ppu) {
                ppu_unlock(m);
            } else {
                uint32_t next = __sync_mutex_release(word);

                ppu_store(m, __sync_mutex_current(next), __sync_mutex_next(next));
            }
            a[k].state = IDLE;
            if (++a[k].done == ROUNDS)
                ++finished;
            break;
        }
    }
    CHECK(!broken, "mutual exclusion and ticket order held");
    CHECK(finished == AGENTS && entries == AGENTS * ROUNDS, "all %d agents finished (%u entries)", AGENTS, entries);
    CHECK(ppu_current(m) == ppu_next(m) && ppu_current(m) == (AGENTS * ROUNDS) % 65536u, "word ends free");
    printf("  mutex ticket-helper model: %d agents (PPU model / SPU helpers), %u critical sections, "
           "%u steps\n", AGENTS, entries, steps);
}

/* ---- barrier -------------------------------------------------------- */

static void test_barrier(void)
{
    const uint64_t b = 0x5000;
    uint8_t before[4];

    mfc_mock_swap32 = 1;
    memset(mfc_mock_memory + b, 0x99, 4);
    memcpy(before, mfc_mock_memory + b, 4);
    CHECK(cellSyncBarrierInitialize(b, 0, 0) == CELL_SYNC_ERROR_INVAL, "count 0");
    CHECK(cellSyncBarrierInitialize(b, 32768, 0) == CELL_SYNC_ERROR_INVAL, "count 32768");
    CHECK(cellSyncBarrierInitialize(b, 65535, 0) == CELL_SYNC_ERROR_INVAL, "count 65535");
    CHECK(memcmp(before, mfc_mock_memory + b, 4) == 0, "refusals write nothing");
    CHECK(cellSyncBarrierInitialize(b + 2, 4, 0) == (int)CELL_SYNC_ERROR_ALIGN, "misaligned");
    CHECK(cellSyncBarrierInitialize(b, 32767, 0) == CELL_OK, "count 32767");
    CHECK(cellSyncBarrierInitialize(b, 1, 0) == CELL_OK && cellSyncBarrierTryWait(b) == CELL_SYNC_ERROR_AGAIN
          && cellSyncBarrierNotify(b) == CELL_OK && cellSyncBarrierTryWait(b) == CELL_OK, "count 1 cycle");
}

/* ---- reservation loss ----------------------------------------------- */

/* Scripted interference at putllc: script[i] runs on the i-th attempt
 * since arm(); NULL entries and attempts past the end lose nothing.  Each
 * action stores to the line as another processor, which loses the SPU's
 * reservation. */
typedef void (*interference)(uint64_t line);
static interference script[8];
static unsigned script_len, script_pos;
static uint64_t target_ea;

static int run_script(uint64_t line)
{
    unsigned i = script_pos++;

    if (i < script_len && script[i])
        script[i](line);
    return 0;
}

static void arm(unsigned n, interference a, interference b, interference c)
{
    script[0] = a;
    script[1] = b;
    script[2] = c;
    script_len = n;
    script_pos = 0;
    mfc_mock_putllc_attempts = mfc_mock_putllc_lost = 0;
    mfc_mock_on_putllc = run_script;
}

static void disarm(void)
{
    mfc_mock_on_putllc = NULL;
    mfc_mock_on_reserve = NULL;
}

/* Native-order (queue/rwm) foreign actions. */
static void foreign_scribble(uint64_t line)
{
    static const uint64_t marker = 0x0123456789abcdefull;

    mfc_mock_foreign_store(line + 64, &marker, 8);
}

static void foreign_queue_push(uint64_t line)
{
    uint64_t head = native64(target_ea);

    (void)line;
    head = _cellSyncQueueMakeHead(_cellSyncQueueGetRlock(head), _cellSyncQueueGetIndex(head) + 1,
                                  _cellSyncQueueGetWlock(head), _cellSyncQueueGetSize(head) + 1);
    mfc_mock_foreign_store(target_ea, &head, 8);
}

static void foreign_queue_pop(uint64_t line)
{
    uint64_t head = native64(target_ea);

    (void)line;
    head = _cellSyncQueueMakeHead(_cellSyncQueueGetRlock(head), _cellSyncQueueGetIndex(head),
                                  _cellSyncQueueGetWlock(head), _cellSyncQueueGetSize(head) - 1);
    mfc_mock_foreign_store(target_ea, &head, 8);
}

static void foreign_rwm_reader(uint64_t line)
{
    uint64_t word = native64(target_ea) + ((uint64_t)1 << 48);   /* rlock + 1 */

    (void)line;
    mfc_mock_foreign_store(target_ea, &word, 8);
}

/* Big-endian (mutex/barrier) foreign actions. */
static void foreign_take_ticket(uint64_t line)
{
    (void)line;
    (void)ppu_take_ticket(target_ea);
}

/* The barrier word as the SPU code sees it: the big-endian 32-bit value
 * viewed through the SPU's CellSyncBarrier union (on this host the union's
 * halves follow host order, so only the value round trip is meaningful). */
static CellSyncBarrier barrier_word(uint64_t ea)
{
    CellSyncBarrier w;
    const uint8_t *p = mfc_mock_memory + ea;

    w.uint_val = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return w;
}

static void foreign_barrier_notify(uint64_t line)
{
    CellSyncBarrier w = barrier_word(target_ea);
    uint8_t bytes[4];

    (void)line;
    w.count--;
    bytes[0] = (uint8_t)(w.uint_val >> 24);
    bytes[1] = (uint8_t)(w.uint_val >> 16);
    bytes[2] = (uint8_t)(w.uint_val >> 8);
    bytes[3] = (uint8_t)w.uint_val;
    mfc_mock_foreign_store(target_ea, bytes, 4);
}

static void test_reservation_loss(void)
{
    static uint8_t in[128] __attribute__((aligned(128)));
    static uint8_t out[128] __attribute__((aligned(128)));
    const uint64_t q = 0x6000, rwm = 0x7000, m = 0x8000 + 12, b = 0x9000;
    uint64_t head, marker;

    /* Queue Initialize: the first store is lost because another processor
     * wrote elsewhere in the line.  The retry re-reads the line, so the
     * descriptor is complete and the other bytes are the other
     * processor's, not a stale LS copy. */
    mfc_mock_swap32 = 0;
    memset(mfc_mock_memory + q, 0xab, 0x800);
    arm(1, foreign_scribble, NULL, NULL);
    CHECK(cellSyncQueueInitialize(q, q + 128, 16, 4, 5) == CELL_OK, "queue init under loss");
    memcpy(&marker, mfc_mock_memory + q + 64, 8);
    CHECK(mfc_mock_putllc_lost == 1 && mfc_mock_putllc_attempts == 2, "one lost, one stored (%lu/%lu)",
          mfc_mock_putllc_lost, mfc_mock_putllc_attempts);
    CHECK(native64(q) == 0 && native64(q + 8) == ((uint64_t)16 << 32 | 4) && native64(q + 16) == q + 128,
          "queue descriptor complete after a lost reservation");
    CHECK(marker == 0x0123456789abcdefull, "other processor's bytes in the line kept");

    /* Push: a foreign producer completes a push while ours takes wlock
     * (attempt 0), and a foreign consumer pops while ours commits
     * (attempt 2).  Our element lands in slot 1 and the head counts all
     * three operations. */
    target_ea = q;
    memset(in, 0x61, 16);
    arm(3, foreign_queue_push, NULL, foreign_queue_pop);
    CHECK(cellSyncQueuePush(q, in, 7) == CELL_OK, "push under loss");
    head = native64(q);
    CHECK(_cellSyncQueueGetIndex(head) == 2 && _cellSyncQueueGetSize(head) == 1
          && _cellSyncQueueGetWlock(head) == 0 && _cellSyncQueueGetRlock(head) == 0,
          "head re-read each time: index %u size %u wlock %u", _cellSyncQueueGetIndex(head),
          _cellSyncQueueGetSize(head), _cellSyncQueueGetWlock(head));
    CHECK(mfc_mock_memory[q + 128 + 16] == 0x61 && mfc_mock_memory[q + 128] == 0xab,
          "element in slot 1, slot 0 untouched");
    CHECK(mfc_mock_putllc_lost == 2, "two reservations lost (%lu)", mfc_mock_putllc_lost);

    /* Pop: a foreign producer pushes while ours takes rlock. */
    arm(1, foreign_queue_push, NULL, NULL);
    CHECK(cellSyncQueuePop(q, out, 7) == CELL_OK, "pop under loss");
    head = native64(q);
    CHECK(_cellSyncQueueGetSize(head) == 1 && _cellSyncQueueGetIndex(head) == 3
          && _cellSyncQueueGetRlock(head) == 0, "pop re-read: size %u index %u",
          _cellSyncQueueGetSize(head), _cellSyncQueueGetIndex(head));
    disarm();

    /* Rwm Initialize, then a reader arriving while ours takes rlock, and
     * another while ours releases it. */
    memset(mfc_mock_memory + rwm, 0xcd, 0x200);
    arm(1, foreign_scribble, NULL, NULL);
    CHECK(cellSyncRwmInitialize(rwm, rwm + 128, 64, 6) == CELL_OK, "rwm init under loss");
    memcpy(&marker, mfc_mock_memory + rwm + 64, 8);
    CHECK(native64(rwm) == 64 && native64(rwm + 8) == rwm + 128 && marker == 0x0123456789abcdefull,
          "rwm descriptor complete, other bytes kept");
    target_ea = rwm;
    arm(1, foreign_rwm_reader, NULL, NULL);
    CHECK(cellSyncRwmReadBegin(rwm, out, 6) == CELL_OK, "read begin under loss");
    CHECK(native64(rwm) >> 48 == 2, "two readers counted (%llu)", (unsigned long long)(native64(rwm) >> 48));
    arm(1, foreign_rwm_reader, NULL, NULL);
    CHECK(cellSyncRwmReadEnd(rwm, 6) == CELL_OK && native64(rwm) >> 48 == 2,
          "read end re-read: the new reader stays counted");
    disarm();

    /* Mutex: the PPU takes a ticket while the SPU's Lock stores its own.
     * The SPU must re-read and take ticket 1, then wait for the PPU. */
    mfc_mock_swap32 = 1;
    CHECK(cellSyncMutexInitialize(m, 0) == CELL_OK, "mutex init");
    target_ea = m;
    arm(1, foreign_take_ticket, NULL, NULL);
    hook_ea = m;
    hook_countdown = 4;
    hook_fired = 0;
    mfc_mock_on_reserve = release_after_spins;
    CHECK(cellSyncMutexLock(m) == CELL_OK && hook_fired, "SPU Lock waited for the PPU's ticket 0");
    CHECK(ppu_current(m) == 1 && ppu_next(m) == 2, "no stale ticket: (%u,%u) want (1,2)",
          ppu_current(m), ppu_next(m));
    /* Unlock while the PPU queues ticket 2: current advances, the PPU's
     * ticket is kept. */
    mfc_mock_on_reserve = NULL;
    arm(1, foreign_take_ticket, NULL, NULL);
    CHECK(cellSyncMutexUnlock(m) == CELL_OK && ppu_current(m) == 2 && ppu_next(m) == 3,
          "unlock re-read: (%u,%u) want (2,3)", ppu_current(m), ppu_next(m));
    ppu_unlock(m);
    /* TryLock on a free mutex while the PPU takes it: the re-read says BUSY. */
    arm(1, foreign_take_ticket, NULL, NULL);
    CHECK(cellSyncMutexTryLock(m) == (int)CELL_SYNC_ERROR_BUSY && ppu_current(m) == 3 && ppu_next(m) == 4,
          "trylock re-read: BUSY, only the PPU's ticket taken (%u,%u)", ppu_current(m), ppu_next(m));
    ppu_unlock(m);

    /* Barrier: another participant notifies while ours stores. */
    CHECK(cellSyncBarrierInitialize(b, 3, 0) == CELL_OK, "barrier init");
    target_ea = b;
    arm(1, foreign_barrier_notify, NULL, NULL);
    CHECK(cellSyncBarrierNotify(b) == CELL_OK, "notify under loss");
    CHECK(barrier_word(b).count == 1 && barrier_word(b).total_count == 3, "count 3 - 2 = 1 (%u)",
          barrier_word(b).count);
    disarm();

    /* Every retry loop again with every other reservation lost. */
    mfc_mock_putllc_attempts = mfc_mock_putllc_lost = 0;
    mfc_mock_on_putllc = mfc_mock_lose_alternate;
    test_descriptors();
    test_barrier();
    disarm();
    CHECK(mfc_mock_putllc_lost > 10, "alternate-loss rerun lost %lu reservations", mfc_mock_putllc_lost);
    printf("  reservation loss: scripted interference on queue, rwm, mutex and barrier loops;\n"
           "       descriptor and barrier sections rerun losing %lu of %lu putllc\n",
           mfc_mock_putllc_lost, mfc_mock_putllc_attempts);
}

/* Lose exactly the n-th putllc from now (1 = the next one), no change. */
static unsigned long lose_at;

static int lose_nth(uint64_t line)
{
    (void)line;
    return mfc_mock_putllc_attempts == lose_at;
}

static void lose_next(unsigned long n)
{
    mfc_mock_putllc_attempts = mfc_mock_putllc_lost = 0;
    lose_at = n;
    mfc_mock_on_putllc = lose_nth;
}

/* The remaining entry points, each with its first store lost.  Looping
 * forms must retry and succeed; the single-attempt Try forms report
 * AGAIN and leave the object unchanged, then succeed when called again. */
static void test_loss_entry_points(void)
{
    static uint8_t in[128] __attribute__((aligned(128)));
    static uint8_t out[128] __attribute__((aligned(128)));
    const uint64_t q = 0xa000, rwm = 0xb000, b = 0xc000;
    uint64_t before;

    mfc_mock_swap32 = 0;
    CHECK(cellSyncQueueInitialize(q, q + 128, 16, 4, 5) == CELL_OK, "queue init");
    memset(in, 0x33, 16);

    lose_next(1);
    before = native64(q);
    CHECK(cellSyncQueueTryPush(q, in, 7) == CELL_SYNC_ERROR_AGAIN && native64(q) == before
          && mfc_mock_putllc_lost == 1, "TryPush: lost wlock store -> AGAIN, head unchanged");
    lose_next(2);                   /* the commit store */
    CHECK(cellSyncQueueTryPush(q, in, 7) == CELL_OK && cellSyncQueueSize(q) == 1
          && mfc_mock_putllc_lost == 1, "TryPush: lost commit store retried");

    lose_next(1);
    CHECK(cellSyncQueuePeek(q, out, 7) == CELL_OK && out[0] == 0x33 && cellSyncQueueSize(q) == 1
          && mfc_mock_putllc_lost == 1, "Peek retried, size kept");
    lose_next(1);
    before = native64(q);
    CHECK(cellSyncQueueTryPeek(q, out, 7) == CELL_SYNC_ERROR_AGAIN && native64(q) == before,
          "TryPeek: lost store -> AGAIN, head unchanged");
    CHECK(cellSyncQueueTryPeek(q, out, 7) == CELL_OK && out[15] == 0x33, "TryPeek again");
    lose_next(1);
    before = native64(q);
    CHECK(cellSyncQueueTryPop(q, out, 7) == CELL_SYNC_ERROR_AGAIN && native64(q) == before,
          "TryPop: lost store -> AGAIN, head unchanged");
    CHECK(cellSyncQueueTryPop(q, out, 7) == CELL_OK && cellSyncQueueSize(q) == 0, "TryPop again");
    CHECK(cellSyncQueuePush(q, in, 7) == CELL_OK && cellSyncQueuePush(q, in, 7) == CELL_OK, "refill");
    lose_next(1);
    CHECK(cellSyncQueueClear(q) == CELL_OK && native64(q) == 0 && mfc_mock_putllc_lost == 1,
          "Clear retried, head zero");
    CHECK(native64(q + 8) == ((uint64_t)16 << 32 | 4) && native64(q + 16) == q + 128,
          "descriptor intact after Clear");

    CHECK(cellSyncRwmInitialize(rwm, rwm + 128, 32, 6) == CELL_OK, "rwm init");
    memset(in, 0x44, 32);
    lose_next(1);
    CHECK(cellSyncRwmWrite(rwm, in, 6) == CELL_OK && mfc_mock_memory[rwm + 128] == 0x44
          && native64(rwm) == 32 && mfc_mock_putllc_lost == 1, "Write retried, lock released");
    lose_next(1);
    before = native64(rwm);
    CHECK(cellSyncRwmTryWrite(rwm, in, 6) == CELL_SYNC_ERROR_AGAIN && native64(rwm) == before,
          "TryWrite: lost store -> AGAIN, lock word unchanged");
    lose_next(2);                   /* the release store */
    CHECK(cellSyncRwmTryWrite(rwm, in, 6) == CELL_OK && native64(rwm) == 32 && mfc_mock_putllc_lost == 1,
          "TryWrite: lost release store retried");
    lose_next(1);
    before = native64(rwm);
    CHECK(cellSyncRwmTryReadBegin(rwm, out, 6) == CELL_SYNC_ERROR_AGAIN && native64(rwm) == before,
          "TryReadBegin: lost store -> AGAIN, no reader counted");
    CHECK(cellSyncRwmTryReadBegin(rwm, out, 6) == CELL_OK && out[31] == 0x44 && native64(rwm) >> 48 == 1,
          "TryReadBegin again");
    CHECK(cellSyncRwmReadEnd(rwm, 6) == CELL_OK && native64(rwm) >> 48 == 0, "ReadEnd");

    mfc_mock_swap32 = 1;
    CHECK(cellSyncBarrierInitialize(b, 2, 0) == CELL_OK, "barrier init");
    lose_next(1);
    CHECK(cellSyncBarrierTryNotify(b) == CELL_OK && barrier_word(b).count == 1 && mfc_mock_putllc_lost == 1,
          "TryNotify retried, one arrival counted");
    disarm();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_descriptors();
    test_mutex_script();
    test_mutex_interleaving();
    test_barrier();
    test_reservation_loss();
    test_loss_entry_points();
    if (failures) {
        printf("libsync-spu: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("libsync-spu: PASS (%d checks)\n", checks);
    return 0;
}
