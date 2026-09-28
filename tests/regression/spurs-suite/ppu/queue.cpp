/* spurs-suite queue (row spurs-queue): the SPU side of the SPURS queue
 * against the firmware PPU side.
 *   api      one task initializes a queue itself and walks every call and
 *            error path: size/depth/entry size/direction/taskset getters,
 *            try-pop on empty, try-push on full, peek, pop order across an
 *            index wrap, clear, and argument errors
 *   perm     an SPU may not push a PPU2SPU queue nor pop an SPU2PPU one
 *   spu2spu  producer and consumer tasks through a depth-2 queue, both
 *            blocking, the consumer peeking before every third pop
 *   spu2ppu  a task pushes while the PPU pops through the attached event
 *            queue; the task blocks on a full ring
 *   ppu2spu  the PPU pushes while a task pops; the task blocks on an empty
 *            ring and the PPU on a full one */
#include "harness.h"
#include <cell/spurs/queue.h>
#include "../queue.h"
#include SUITE_SPU_HEADER

struct queue_block {
    CellSpursQueue queue[Q_COUNT];
    uint8_t ring[Q_COUNT][Q_RING_BYTES];
};
alignas(128) static queue_block s_block;
alignas(128) static volatile result_slot g_result[Q_SLOTS];

static bool done(unsigned kind) { return suite::slot_done(g_result[kind], kind); }

/* watchdog dump: every queue's control line and every result slot */
static void dump()
{
    for (unsigned q = 0; q < Q_COUNT; ++q) {
        const volatile uint32_t *w = reinterpret_cast<const volatile uint32_t *>(&s_block.queue[q]);
        for (unsigned row = 0; row < 32; row += 8)
            std::printf("q%u+%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", q, row * 4,
                        w[row], w[row + 1], w[row + 2], w[row + 3],
                        w[row + 4], w[row + 5], w[row + 6], w[row + 7]);
    }
    for (unsigned k = 0; k < Q_SLOTS; ++k)
        std::printf("slot%u: %08x %08x %08x %08x\n", k, g_result[k].magic, g_result[k].status,
                    g_result[k].value, g_result[k].extra);
}

static void phase(const char *name)
{
    suite::activity("phase %s", name);
    std::printf("queue phase %s\n", name);
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
    suite::watch_slots(g_result, Q_SLOTS);
    suite::watchdog(30, dump);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteQ");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);
    auto *cts = reinterpret_cast<CellSpursTaskset *>(ts);

    std::memset(&s_block, 0, sizeof s_block);
    std::memset((void *)g_result, 0, sizeof g_result);
    const struct { unsigned idx, depth; CellSpursQueueDirection dir; } queues[] = {
        { Q_SPU2PPU, 4, CELL_SPURS_QUEUE_SPU2PPU },
        { Q_PPU2SPU, 4, CELL_SPURS_QUEUE_PPU2SPU },
        { Q_SPU2SPU, 2, CELL_SPURS_QUEUE_SPU2SPU },
    };
    for (const auto &q : queues) {
        rc = cellSpursQueueInitialize(cts, &s_block.queue[q.idx], s_block.ring[q.idx], Q_ENTRY, q.depth, q.dir);
        if (rc) return suite::invalid("queue initialize", rc);
    }
    if ((rc = cellSpursQueueAttachLv2EventQueue(&s_block.queue[Q_SPU2PPU])))
        return suite::invalid("attach spu2ppu", rc);
    if ((rc = cellSpursQueueAttachLv2EventQueue(&s_block.queue[Q_PPU2SPU])))
        return suite::invalid("attach ppu2spu", rc);

    const uint64_t slots = reinterpret_cast<uintptr_t>(g_result);
    const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_block));
    auto launch = [&](unsigned kind) { return suite::launch(ts, SUITE_SPU_BIN, slots, base, kind); };
    int result = 0;

    phase("api");
    if ((rc = launch(Q_API))) return suite::invalid("launch api", rc);
    result = check_task(Q_API, "api");

    if (!result) {
        phase("perm");
        if ((rc = launch(Q_PERM))) return suite::invalid("launch perm", rc);
        result = check_task(Q_PERM, "perm");
    }

    if (!result) {
        phase("spu2spu");
        if ((rc = launch(Q_SPU2SPU_CONSUMER))) return suite::invalid("launch spu2spu consumer", rc);
        sys_timer_usleep(20000);          /* consumer blocks on the empty ring */
        if ((rc = launch(Q_SPU2SPU_PRODUCER))) return suite::invalid("launch spu2spu producer", rc);
        result = check_task(Q_SPU2SPU_PRODUCER, "spu2spu producer");
        if (!result) result = check_task(Q_SPU2SPU_CONSUMER, "spu2spu consumer");
    }

    if (!result) {
        phase("spu2ppu");
        if ((rc = launch(Q_SPU2PPU_PRODUCER))) return suite::invalid("launch spu2ppu producer", rc);
        sys_timer_usleep(50000);          /* producer fills the ring and blocks */
        for (unsigned i = 0; i < Q_ROUNDS && !result; ++i) {
            alignas(16) q_entry e;
            std::memset(&e, 0, sizeof e);
            if ((rc = cellSpursQueuePop(&s_block.queue[Q_SPU2PPU], &e)))
                result = suite::fail("spu2ppu pop rc", rc, 0);
            else if (e.seq != i || e.magic != (Q_SEQ_MAGIC | i))
                result = suite::fail("spu2ppu order", e.seq, i);
        }
        if (!result) result = check_task(Q_SPU2PPU_PRODUCER, "spu2ppu producer");
    }

    if (!result) {
        phase("ppu2spu");
        if ((rc = launch(Q_PPU2SPU_CONSUMER))) return suite::invalid("launch ppu2spu consumer", rc);
        sys_timer_usleep(20000);          /* consumer blocks on the empty ring */
        for (unsigned i = 0; i < Q_ROUNDS && !result; ++i) {
            alignas(16) q_entry e = { i, Q_SEQ_MAGIC | i, 0xffffffffu, 0 };
            if ((rc = cellSpursQueuePush(&s_block.queue[Q_PPU2SPU], &e)))
                result = suite::fail("ppu2spu push rc", rc, 0);
        }
        if (!result) result = check_task(Q_PPU2SPU_CONSUMER, "ppu2spu consumer");
    }

    /* a failed step can leave a task blocked on a queue, and the taskset
       would never join: report without tearing down */
    if (result)
        return result;
    unsigned size = 99;
    if ((rc = cellSpursQueueSize(&s_block.queue[Q_SPU2SPU], &size)) || size)
        return suite::fail("spu2spu drained", rc ? rc : size, 0);

    cellSpursQueueDetachLv2EventQueue(&s_block.queue[Q_SPU2PPU]);
    cellSpursQueueDetachLv2EventQueue(&s_block.queue[Q_PPU2SPU]);
    suite::taskset_down(ts);
    spurs->finalize();
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
