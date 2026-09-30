/* spurs-suite fiber signalling (row spu-fiber-signal): two PPU fibers run by
 * a cell::Fiber::Ppu::Util::Runtime (one worker thread) wait in
 * cellFiberPpuWaitSignal; SPURS tasks wake them.  Fiber A through the
 * worker control's signal (which also wakes the sleeping worker), fiber B
 * with cellFiberPpuSendSignal plus an explicit worker wake-up.  Checks both
 * fibers resume and exit with their codes, and that the SPU sees the
 * fiber's scheduler (the runtime itself). */
#include "harness.h"
#include <cell/sysmodule.h>
#include <cell/fiber/ppuUtilRuntime.h>
#include "../fiber_signal.h"
#include SUITE_SPU_HEADER

typedef cell::Fiber::Ppu::Util::Runtime FiberRuntime;

alignas(128) static FiberRuntime s_runtime;
alignas(CELL_FIBER_PPU_ALIGN) static CellFiberPpu s_fiber[2];
alignas(16) static uint8_t s_stack[2][16 * 1024];
alignas(128) static volatile fs_box s_box[2];
static volatile int s_state[2];

static int fiber_entry(uint64_t arg)
{
    const unsigned i = static_cast<unsigned>(arg);
    s_state[i] = 1;
    int rc = cellFiberPpuWaitSignal();
    s_state[i] = rc ? 0x100 | rc : 2;
    return 7 + static_cast<int>(i);
}

static int signal_fiber(cell::Spurs::Taskset *ts, unsigned i, uint32_t kind)
{
    volatile fs_box &b = s_box[i];
    std::memset(const_cast<fs_box *>(&b), 0, sizeof b);
    b.fiber = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_fiber[i]));
    b.runtime = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_runtime));
    __sync_synchronize();
    return suite::launch(ts, SUITE_SPU_BIN, reinterpret_cast<uintptr_t>(&b), 0, kind);
}

static int row_main()
{
    suite::watchdog(40);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteFib");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);

    suite::activity("starting a fiber runtime with one worker");
    if ((rc = FiberRuntime::initialize(&s_runtime, 1000, 1))) return suite::invalid("fiber runtime", rc);
    for (unsigned i = 0; i < 2; ++i)
        if ((rc = s_runtime.createFiber(&s_fiber[i], fiber_entry, i, s_stack[i], sizeof s_stack[i])))
            return suite::invalid("create fiber", rc);
    s_runtime.wakeup();
    if (!suite::wait_for([] { return s_state[0] == 1 && s_state[1] == 1; }))
        return suite::fail("both fibers waiting", s_state[0] | s_state[1] << 8, 0x101);
    sys_timer_usleep(50000);

    int result = 0;
    suite::activity("SPU task signals fiber A through the worker control");
    if ((rc = signal_fiber(ts, 0, FS_UTIL_SIGNAL))) return suite::invalid("launch A", rc);
    if (!suite::wait_for([] { return s_box[0].state == 2; })) result = suite::fail("task A done", s_box[0].state, 2);
    else if (s_box[0].rc) result = suite::fail("worker control signal rc", s_box[0].rc, 0);
    else if (s_box[0].scheduler != s_box[0].runtime) result = suite::fail("fiber's scheduler", s_box[0].scheduler, s_box[0].runtime);
    else if (!suite::wait_for([] { return s_state[0] != 1; }) || s_state[0] != 2) result = suite::fail("fiber A resumed", s_state[0], 2);
    else if (s_state[1] != 1) result = suite::fail("fiber B still waiting", s_state[1], 1);

    if (!result) {
        suite::activity("SPU task signals fiber B and wakes a worker");
        if ((rc = signal_fiber(ts, 1, FS_PLAIN_SIGNAL))) return suite::invalid("launch B", rc);
        if (!suite::wait_for([] { return s_box[1].state == 2; })) result = suite::fail("task B done", s_box[1].state, 2);
        else if (s_box[1].rc) result = suite::fail("plain signal + wake-up rc", s_box[1].rc, 0);
        else if (s_box[1].numWorker == 0xffffffffu) result = suite::fail("worker count reported", s_box[1].numWorker, 1);
        else if (!suite::wait_for([] { return s_state[1] != 1; }) || s_state[1] != 2) result = suite::fail("fiber B resumed", s_state[1], 2);
    }

    for (unsigned i = 0; i < 2 && !result; ++i) {
        int exitCode = -1;
        if ((rc = s_runtime.joinFiber(&s_fiber[i], &exitCode))) result = suite::fail("join fiber rc", rc, 0);
        else if (exitCode != static_cast<int>(7 + i)) result = suite::fail("fiber exit code", exitCode, 7 + i);
    }
    suite::activity("shutting down the fiber runtime");
    if ((rc = s_runtime.shutdown()) && !result) result = suite::fail("runtime shutdown rc", rc, 0);
    if ((rc = s_runtime.finalize()) && !result) result = suite::fail("runtime finalize rc", rc, 0);
    suite::taskset_down(ts);
    spurs->finalize();
    return result ? result : suite::ok();
}

SUITE_ENTRY_POINT(row_main)
