/* spurs-suite custom policy module (row spurs-module).
 *   add      the PPU adds a policy module as a workload and makes it ready
 *   run      the kernel runs it, repeatedly while it stays ready; it records
 *            its workload id, SPU and argument
 *   unit     each run loads a relocatable -mcustom-module work unit at an
 *            LS address of its choosing and runs it: its _init must
 *            relocate it (vtables, a pointer table) and construct it
 *   count    ready count compare-and-swap (miss and hit) and add, which
 *            clamps at 0; each returns the previous count
 *   remove   ready count 0, shutdown, wait for the shutdown, remove */
#include "harness.h"
#include <cell/spurs/workload.h>
#include <cell/spurs/ready_count.h>
#include "../module.h"
#include SUITE_PM_HEADER
#include SUITE_UNIT_HEADER

alignas(128) static volatile module_box s_box;
static const uint8_t s_prio[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };

static int row_main()
{
    suite::watchdog(30);
    std::memset((void *)&s_box, 0, sizeof s_box);
    s_box.unitEa = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(SUITE_UNIT_BIN));
    s_box.unitSize = SUITE_UNIT_BIN_SIZE;
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteMod");
    if (rc) return suite::invalid("spurs", rc);
    CellSpurs *cs = reinterpret_cast<CellSpurs *>(spurs);

    CellSpursWorkloadId wid = 99;
    suite::activity("adding the policy module (%u bytes)", static_cast<unsigned>(SUITE_PM_BIN_SIZE));
    if ((rc = cellSpursAddWorkload(cs, &wid, SUITE_PM_BIN, SUITE_PM_BIN_SIZE,
                                   reinterpret_cast<uintptr_t>(&s_box), s_prio, 1, 1)))
        return suite::fail("add workload", rc, 0);
    if ((rc = cellSpursReadyCountStore(cs, wid, 1))) return suite::fail("ready count 1", rc, 0);

    suite::activity("waiting for the module to run twice");
    if (!suite::wait_for([] { return s_box.magic == M_MAGIC && s_box.runs >= 2; }))
        return suite::fail("module ran twice", s_box.runs, 2);
    if (s_box.wid != wid) return suite::fail("module's workload id", s_box.wid, wid);
    if (s_box.spu > 3) return suite::fail("module's SPU id", s_box.spu, 3);
    if (s_box.arg != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_box)))
        return suite::fail("module's workload argument", s_box.arg, 0);
    if (s_box.unitResult != M_UNIT_OK)
        return suite::fail("work unit relocated and constructed", s_box.unitResult, M_UNIT_OK);

    suite::activity("ready count compare-and-swap and add");
    unsigned old = 0;
    if ((rc = cellSpursReadyCountCompareAndSwap(cs, wid, &old, 3, 7))) return suite::fail("ready count CAS (miss) rc", rc, 0);
    if (old != 1) return suite::fail("CAS miss: previous ready count", old, 1);
    /* the count stays 0..1: the module is not written to run on two SPUs */
    if ((rc = cellSpursReadyCountCompareAndSwap(cs, wid, &old, 1, 0))) return suite::fail("ready count CAS rc", rc, 0);
    if (old != 1) return suite::fail("CAS: previous ready count", old, 1);
    if ((rc = cellSpursReadyCountAdd(cs, wid, &old, 1))) return suite::fail("ready count add rc", rc, 0);
    if (old != 0) return suite::fail("add: previous ready count", old, 0);
    if ((rc = cellSpursReadyCountAdd(cs, wid, &old, -5))) return suite::fail("ready count subtract rc", rc, 0);
    if (old != 1) return suite::fail("subtract: previous ready count", old, 1);
    if ((rc = cellSpursReadyCountAdd(cs, wid, &old, 1))) return suite::fail("ready count add after clamp rc", rc, 0);
    if (old != 0) return suite::fail("subtract clamps at 0", old, 0);

    suite::activity("removing the workload");
    if ((rc = cellSpursReadyCountSwap(cs, wid, &old, 0))) return suite::fail("ready count swap", rc, 0);
    if (old != 1) return suite::fail("previous ready count", old, 1);
    if ((rc = cellSpursShutdownWorkload(cs, wid))) return suite::fail("shutdown workload", rc, 0);
    if ((rc = cellSpursWaitForWorkloadShutdown(cs, wid))) return suite::fail("wait for shutdown", rc, 0);
    if ((rc = cellSpursRemoveWorkload(cs, wid))) return suite::fail("remove workload", rc, 0);
    spurs->finalize();
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
