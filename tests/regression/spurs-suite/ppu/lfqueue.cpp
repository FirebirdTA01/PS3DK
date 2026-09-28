/* spurs-suite lock-free queue (row spurs-lfqueue): the SPU side of the
 * SPURS lock-free queue (and the libsync queue under it) against the
 * firmware PPU side.
 *   api      one task initializes a queue itself and walks its calls and
 *            error paths: getters, try-pop on empty, try-push on full,
 *            order across an index wrap
 *   spu2spu  producer and consumer tasks through a depth-2 queue, both
 *            blocking
 *   spu2ppu  a task pushes while the PPU pops through the attached event
 *            queue; the task blocks on a full ring
 *   ppu2spu  the PPU pushes while a task pops; the task blocks on an empty
 *            ring and the PPU on a full one */
#include "harness.h"
#include <cell/spurs/lfqueue.h>
#include "../lfqueue.h"
#include SUITE_SPU_HEADER

struct queue_block {
    CellSpursLFQueue queue[LQ_COUNT];
    uint8_t ring[LQ_COUNT][LQ_RING_BYTES];
};
alignas(128) static queue_block s_block;
alignas(128) static volatile result_slot g_result[LQ_SLOTS];

static bool done(unsigned kind) { return suite::slot_done(g_result[kind], kind); }

/* watchdog dump: every queue's control block and every result slot */
static void dump()
{
    for (unsigned q = 0; q < LQ_COUNT; ++q) {
        const volatile uint32_t *w = reinterpret_cast<const volatile uint32_t *>(&s_block.queue[q]);
        for (unsigned row = 0; row < 32; row += 8)
            std::printf("lq%u+%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", q, row * 4,
                        w[row], w[row + 1], w[row + 2], w[row + 3],
                        w[row + 4], w[row + 5], w[row + 6], w[row + 7]);
    }
    for (unsigned k = 0; k < LQ_SLOTS; ++k)
        std::printf("slot%u: %08x %08x %08x %08x\n", k, g_result[k].magic, g_result[k].status,
                    g_result[k].value, g_result[k].extra);
}

/* Probe: while the main thread sleeps in a pop on the empty SPU2PPU queue,
 * print what the PPU side published in the control block, then start the
 * producer. */
static cell::Spurs::Taskset *s_ts;
static int s_probeRc;
static void probe_then_launch(uint64_t)
{
    sys_timer_usleep(100000);
    const volatile uint32_t *w = reinterpret_cast<const volatile uint32_t *>(&s_block.queue[LQ_SPU2PPU]);
    std::printf("probe ppu-sleeping: %08x %08x %08x %08x pack30=%08x pack50=%08x bs=%08x\n",
                w[0], w[1], w[2], w[3], w[12], w[20], w[8]);
    std::fflush(stdout);
    s_probeRc = suite::launch(s_ts, SUITE_SPU_BIN, reinterpret_cast<uintptr_t>(g_result),
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_block)),
                              LQ_SPU2PPU_PRODUCER);
    sys_ppu_thread_exit(0);
}

static void phase(const char *name)
{
    suite::activity("phase %s", name);
    std::printf("lfqueue phase %s\n", name);
    std::fflush(stdout);
}

static int check_task(unsigned kind, const char *what)
{
    suite::activity("waiting for %s", what);
    if (!suite::wait_for([&] { return done(kind); }))
        return suite::fail(what, g_result[kind].magic, RESULT_MAGIC | kind);
    if (g_result[kind].status) {
        std::printf("%s: step %u rc=%#x detail=%#x\n", what, g_result[kind].value,
                    g_result[kind].status, g_result[kind].extra);
        return suite::fail(what, g_result[kind].status, 0);
    }
    return 0;
}

static int row_main()
{
    suite::watch_slots(g_result, LQ_SLOTS);
    suite::watchdog(30, dump);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteLq");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);
    auto *cts = reinterpret_cast<CellSpursTaskset *>(ts);

    std::memset(&s_block, 0, sizeof s_block);
    std::memset((void *)g_result, 0, sizeof g_result);
    const struct { unsigned idx, depth; CellSpursLFQueueDirection dir; } queues[] = {
        { LQ_SPU2PPU, 4, CELL_SPURS_LFQUEUE_SPU2PPU },
        { LQ_PPU2SPU, 4, CELL_SPURS_LFQUEUE_PPU2SPU },
        { LQ_SPU2SPU, 2, CELL_SPURS_LFQUEUE_SPU2SPU },
    };
    for (const auto &q : queues) {
        rc = cellSpursLFQueueInitialize(cts, &s_block.queue[q.idx], s_block.ring[q.idx], LQ_ENTRY, q.depth, q.dir);
        if (rc) return suite::invalid("lfqueue initialize", rc);
    }
    if ((rc = cellSpursLFQueueAttachLv2EventQueue(&s_block.queue[LQ_SPU2PPU])))
        return suite::invalid("attach spu2ppu", rc);
    if ((rc = cellSpursLFQueueAttachLv2EventQueue(&s_block.queue[LQ_PPU2SPU])))
        return suite::invalid("attach ppu2spu", rc);

    const uint64_t slots = reinterpret_cast<uintptr_t>(g_result);
    const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_block));
    auto launch = [&](unsigned kind) { return suite::launch(ts, SUITE_SPU_BIN, slots, base, kind); };
    int result = 0;

    phase("api");
    if ((rc = launch(LQ_API))) return suite::invalid("launch api", rc);
    result = check_task(LQ_API, "api");

    if (!result) {
        phase("spu2spu");
        if ((rc = launch(LQ_SPU2SPU_CONSUMER))) return suite::invalid("launch spu2spu consumer", rc);
        sys_timer_usleep(20000);          /* consumer blocks on the empty ring */
        if ((rc = launch(LQ_SPU2SPU_PRODUCER))) return suite::invalid("launch spu2spu producer", rc);
        result = check_task(LQ_SPU2SPU_PRODUCER, "spu2spu producer");
        if (!result) result = check_task(LQ_SPU2SPU_CONSUMER, "spu2spu consumer");
    }

    if (!result) {
        phase("spu2ppu");
        /* the pop below sleeps on the empty ring; a helper thread records
           what that published, then starts the producer */
        s_ts = ts;
        sys_ppu_thread_t probe;
        if ((rc = sys_ppu_thread_create(&probe, probe_then_launch, 0, 1000, 0x4000, 0, "lfq probe")))
            return suite::invalid("probe thread", rc);
        for (unsigned i = 0; i < LQ_ROUNDS && !result; ++i) {
            alignas(16) lq_entry e;
            std::memset(&e, 0, sizeof e);
            if ((rc = cellSpursLFQueuePop(&s_block.queue[LQ_SPU2PPU], &e)))
                result = suite::fail("spu2ppu pop rc", rc, 0);
            else if (e.seq != i || e.magic != (LQ_SEQ_MAGIC | i))
                result = suite::fail("spu2ppu order", e.seq, i);
        }
        if (!result) result = check_task(LQ_SPU2PPU_PRODUCER, "spu2ppu producer");
    }

    if (!result) {
        phase("ppu2spu");
        if ((rc = launch(LQ_PPU2SPU_CONSUMER))) return suite::invalid("launch ppu2spu consumer", rc);
        sys_timer_usleep(20000);          /* consumer blocks on the empty ring */
        for (unsigned i = 0; i < LQ_ROUNDS && !result; ++i) {
            alignas(16) lq_entry e = { i, LQ_SEQ_MAGIC | i, 0xffffffffu, 0 };
            if ((rc = cellSpursLFQueuePush(&s_block.queue[LQ_PPU2SPU], &e)))
                result = suite::fail("ppu2spu push rc", rc, 0);
        }
        if (!result) result = check_task(LQ_PPU2SPU_CONSUMER, "ppu2spu consumer");
    }

    /* a failed step can leave a task blocked on a queue, and the taskset
       would never join: report without tearing down */
    if (result)
        return result;
    for (unsigned q = 0; q < 3; ++q) {
        unsigned size = 99;
        if ((rc = cellSpursLFQueueSize(&s_block.queue[q], &size)) || size)
            return suite::fail("queue drained", rc ? rc : size, q);
    }

    cellSpursLFQueueDetachLv2EventQueue(&s_block.queue[LQ_SPU2PPU]);
    cellSpursLFQueueDetachLv2EventQueue(&s_block.queue[LQ_PPU2SPU]);
    suite::taskset_down(ts);
    spurs->finalize();
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
