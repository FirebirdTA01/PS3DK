/* spurs-suite event flag (row spurs-event-flag), PPU side: the SPU side of the SPURS event flag against
 * the firmware SPURS kernel and taskset policy module.
 *
 *   PPU2SPU  an SPU task blocks (OR 0x00f0); once its wait slot shows in the
 *            flag the PPU sets 0x0010, which must wake it with 0x0010.
 *   SPU2SPU  a task blocks (AND 0x0300, MANUAL clear); a second task sets
 *            0x0100 then 0x0200.  The waiter must wake with 0x0300, then see
 *            TryWait(0x0400) BUSY, clear 0x0100, and read SPU2SPU/MANUAL.
 *   SPU2PPU  the PPU blocks (AND 0x0003, AUTO clear); the task waits until
 *            the PPU wait mask shows, then sets 0x0003 through the flag's
 *            SPU port.  The PPU must return 0x0003 and leave the bits clear.
 *
 * Every wait is forced onto its blocking path. */
#include "harness.h"
#include <cell/spurs/event_flag.h>
#include "../event_flag.h"
#include SUITE_SPU_HEADER

alignas(128) static CellSpursEventFlag g_ef[3];   /* 0 SPU2PPU, 1 PPU2SPU, 2 SPU2SPU */
alignas(128) static volatile result_slot g_result[8];

static uint16_t ef_u16(int i, unsigned off)
{
    const volatile uint8_t *p = reinterpret_cast<const volatile uint8_t *>(&g_ef[i]);
    return static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
}

static int row_main()
{
    suite::watch_slots(g_result, 8);
    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    cell::Spurs::SpursAttribute attr;
    static const uint8_t prio[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    CellSpursTaskLsPattern ls = { { CELL_SPURS_TASK_TOP_MASK, 0xffffffffU, 0xffffffffU, 0xffffffffU } };
    int rc = cell::Spurs::SpursAttribute::initialize(&attr, 4, 100, 2, false);
    if (!rc) rc = attr.setNamePrefix("EvFlag", 6);
    if (!rc) rc = attr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    if (!rc) rc = cell::Spurs::Spurs2::initialize(spurs, &attr);
    if (rc) return suite::invalid("spurs", rc);
    auto *ts = static_cast<cell::Spurs::Taskset *>(::aligned_alloc(CELL_SPURS_TASKSET_ALIGN, CELL_SPURS_TASKSET_SIZE));
    rc = cell::Spurs::Taskset::create(spurs, ts, 0, prio, 4);
    if (rc) return suite::invalid("taskset", rc);

    rc = cellSpursEventFlagInitialize(ts, &g_ef[0], CELL_SPURS_EVENT_FLAG_CLEAR_AUTO, CELL_SPURS_EVENT_FLAG_SPU2PPU);
    if (!rc) rc = cellSpursEventFlagAttachLv2EventQueue(&g_ef[0]);
    if (!rc) rc = cellSpursEventFlagInitialize(ts, &g_ef[1], CELL_SPURS_EVENT_FLAG_CLEAR_AUTO, CELL_SPURS_EVENT_FLAG_PPU2SPU);
    if (!rc) rc = cellSpursEventFlagInitialize(ts, &g_ef[2], CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL, CELL_SPURS_EVENT_FLAG_SPU2SPU);
    if (rc) return suite::invalid("event flags", rc);

    auto launch = [&](unsigned kind, int ef) -> int {
        void *ctx = ::aligned_alloc(CELL_SPURS_TASK_CONTEXT_ALIGN, CELL_SPURS_TASK_CONTEXT_SIZE_ALL);
        CellSpursTaskArgument a;
        CellSpursTaskId id;
        std::memset(&a, 0, sizeof a);
        a.u64[0] = reinterpret_cast<uint64_t>(&g_ef[ef]);
        a.u32[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_result[kind]));
        a.u32[3] = kind;
        return ctx ? ts->createTask(&id, SUITE_SPU_BIN, ctx, CELL_SPURS_TASK_CONTEXT_SIZE_ALL, &ls, &a) : -1;
    };
    auto done = [&](unsigned kind) { return g_result[kind].magic == (RESULT_MAGIC | kind); };
    int result = 0;
    uint16_t bits;

    /* syscall diagnostics: each stage prints, so the TTY shows how far it got */
    auto stage = [&](unsigned kind) { return g_result[kind].magic; };
    if ((rc = launch(KIND_DIAG_YIELD, 0))) return suite::invalid("launch diag yield", rc);
    if (!suite::wait_for([&] { return done(KIND_DIAG_YIELD); })) return suite::fail("diag yield never returned", stage(KIND_DIAG_YIELD), RESULT_MAGIC | KIND_DIAG_YIELD);
    std::printf("diag yield rc=%#x task sp=%#x\n", g_result[KIND_DIAG_YIELD].status, g_result[KIND_DIAG_YIELD].value);
    /* a task that blocks must run on a stack its LS pattern covers (at or above 0x3000) */
    if (g_result[KIND_DIAG_YIELD].value < 0x3000) return suite::fail("task stack below 0x3000", g_result[KIND_DIAG_YIELD].value, 0x3000);
    if ((rc = launch(KIND_DIAG_SIGNAL_SELF, 0))) return suite::invalid("launch diag signal-self", rc);
    if (!suite::wait_for([&] { return done(KIND_DIAG_SIGNAL_SELF); })) return suite::fail("diag signal-self never returned", g_result[KIND_DIAG_SIGNAL_SELF].extra, 0x23);
    std::printf("diag signal-self wait rc=%#x\n", g_result[KIND_DIAG_SIGNAL_SELF].status);
    if ((rc = launch(KIND_DIAG_WAIT_SIGNAL, 0))) return suite::invalid("launch diag wait-signal", rc);
    if (!suite::wait_for([&] { return g_result[KIND_DIAG_WAIT_SIGNAL].extra == 0x31; })) return suite::fail("diag wait-signal never started", g_result[KIND_DIAG_WAIT_SIGNAL].extra, 0x31);
    sys_timer_usleep(20000);
    rc = cellSpursSendSignal(ts, g_result[KIND_DIAG_WAIT_SIGNAL].value);
    std::printf("diag ppu signal to task %u rc=%#x\n", g_result[KIND_DIAG_WAIT_SIGNAL].value, rc);
    if (!suite::wait_for([&] { return done(KIND_DIAG_WAIT_SIGNAL); })) return suite::fail("diag wait-signal never woke", g_result[KIND_DIAG_WAIT_SIGNAL].extra, 0x32);
    std::printf("diag wait-signal rc=%#x\n", g_result[KIND_DIAG_WAIT_SIGNAL].status);

    /* PPU2SPU */
    if ((rc = launch(KIND_WAIT_PPU2SPU, 1))) return suite::invalid("launch ppu2spu waiter", rc);
    if (!suite::wait_for([&] { return ef_u16(1, 0x08) != 0; })) return suite::fail("ppu2spu waiter never blocked", ef_u16(1, 0x08), 1);
    std::printf("ppu2spu waiter blocked\n");
    if ((rc = cellSpursEventFlagSet(&g_ef[1], 0x0010))) return suite::invalid("ppu set", rc);
    if (!suite::wait_for([&] { return done(KIND_WAIT_PPU2SPU); })) return suite::fail("ppu2spu waiter never woke", 0, 1);
    if (g_result[KIND_WAIT_PPU2SPU].status) result = suite::fail("ppu2spu wait rc", g_result[KIND_WAIT_PPU2SPU].status, 0);
    else if (g_result[KIND_WAIT_PPU2SPU].value != 0x0010) result = suite::fail("ppu2spu bits", g_result[KIND_WAIT_PPU2SPU].value, 0x0010);

    /* SPU2SPU */
    if ((rc = launch(KIND_WAIT_SPU2SPU, 2))) return suite::invalid("launch spu2spu waiter", rc);
    if (!suite::wait_for([&] { return ef_u16(2, 0x08) != 0; })) return suite::fail("spu2spu waiter never blocked", 0, 1);
    if ((rc = launch(KIND_SET_SPU2SPU, 2))) return suite::invalid("launch spu2spu setter", rc);
    if (!suite::wait_for([&] { return done(KIND_WAIT_SPU2SPU) && done(KIND_SET_SPU2SPU); })) return suite::fail("spu2spu never completed", g_result[KIND_SET_SPU2SPU].status, 0);
    if (!result && g_result[KIND_SET_SPU2SPU].status) result = suite::fail("spu2spu set rc", g_result[KIND_SET_SPU2SPU].status, 0);
    if (!result && g_result[KIND_WAIT_SPU2SPU].status) result = suite::fail("spu2spu wait/clear/get rc", g_result[KIND_WAIT_SPU2SPU].status, 0);
    if (!result && g_result[KIND_WAIT_SPU2SPU].value != 0x0300) result = suite::fail("spu2spu bits", g_result[KIND_WAIT_SPU2SPU].value, 0x0300);
    if (!result && g_result[KIND_WAIT_SPU2SPU].extra != ((0x090au << 16) | (CELL_SPURS_EVENT_FLAG_SPU2SPU << 8) | CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL))
        result = suite::fail("spu2spu trywait/direction/clearmode", g_result[KIND_WAIT_SPU2SPU].extra, 0x090a0001u);
    if (!result && ef_u16(2, 0x00) != 0x0200) result = suite::fail("spu2spu events after clear", ef_u16(2, 0x00), 0x0200);

    /* SPU2PPU */
    if ((rc = launch(KIND_SET_SPU2PPU, 0))) return suite::invalid("launch spu2ppu setter", rc);
    bits = 0x0003;
    rc = cellSpursEventFlagWait(&g_ef[0], &bits, CELL_SPURS_EVENT_FLAG_AND);
    if (!suite::wait_for([&] { return done(KIND_SET_SPU2PPU); })) return suite::fail("spu2ppu setter never reported", 0, 1);
    if (!result && rc) result = suite::fail("spu2ppu ppu wait rc", static_cast<unsigned>(rc), 0);
    if (!result && bits != 0x0003) result = suite::fail("spu2ppu bits", bits, 0x0003);
    if (!result && g_result[KIND_SET_SPU2PPU].status) result = suite::fail("spu2ppu spu set rc", g_result[KIND_SET_SPU2PPU].status, 0);
    if (!result && ef_u16(0, 0x00) != 0) result = suite::fail("spu2ppu auto clear", ef_u16(0, 0x00), 0);
    std::printf("spu2ppu setter spun %u polls before the PPU blocked\n", g_result[KIND_SET_SPU2PPU].extra);

    ts->shutdown();
    ts->join();
    cellSpursEventFlagDetachLv2EventQueue(&g_ef[0]);
    spurs->finalize();
    return result ? result : suite::ok();
}

SUITE_ENTRY_POINT(row_main)
