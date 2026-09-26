/* Host test of the SPU libsync sources over a simulated MFC
 * (tests/sdk/fixtures/spu-mfc-mock).  Built and run by
 * tests/sdk/libsync-spu-host-test.sh.
 *
 *  - Queue and Rwm Initialize leave their whole descriptor in memory (the
 *    old code staged it in the LS line and then re-read the line over it).
 *  - The mutex is the PPU's 32-bit ticket word: SPU lock/unlock and an
 *    independent PPU-side model (big-endian 16-bit current/next counters
 *    at bytes 0 and 2, updated as foreign stores) exclude each other and
 *    admit waiters in ticket order, both in a scripted sequence and in a
 *    random interleaving of several PPU and SPU agents.
 *  - Barrier Initialize refuses 0 and counts above 32767.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/sync.h>
#include <cell/sync/queue_types.h>
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
    printf("  mutex interleaving: %d PPU/SPU agents, %u critical sections, %u steps\n", AGENTS, entries, steps);
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

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_descriptors();
    test_mutex_script();
    test_mutex_interleaving();
    test_barrier();
    if (failures) {
        printf("libsync-spu: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("libsync-spu: PASS (%d checks)\n", checks);
    return 0;
}
