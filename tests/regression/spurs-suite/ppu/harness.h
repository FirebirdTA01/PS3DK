/* spurs-suite PPU harness: SPURS bring-up, a taskset, task launch,
 * bounded polling and the row verdict.  Every row prints exactly one of
 *   SPURS_SUITE <row> OK
 *   SPURS_SUITE <row> FAIL <step> got=<x> want=<y>
 *   SPURS_SUITE <row> INVALID <step> rc=<rc>     (setup could not run)
 * and returns 0, 1 or 2.  Verdicts are flushed at once: a failing row may
 * leave a task blocked, and then its teardown never returns. */
#ifndef SPURS_SUITE_HARNESS_H
#define SPURS_SUITE_HARNESS_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/process.h>
#include <sys/timer.h>
#include <sys/ppu_thread.h>
#include <cell/spurs.h>
#include "../common.h"

#ifndef SUITE_ROW
#error SUITE_ROW must name the row
#endif

namespace suite {

inline int invalid(const char *step, int rc)
{
    std::printf("SPURS_SUITE %s INVALID %s rc=%#x\n", SUITE_ROW, step, rc);
    std::fflush(stdout);
    return 2;
}

inline int fail(const char *step, unsigned got, unsigned want)
{
    std::printf("SPURS_SUITE %s FAIL %s got=%#x want=%#x\n", SUITE_ROW, step, got, want);
    std::fflush(stdout);
    return 1;
}

inline int ok()
{
    std::printf("SPURS_SUITE %s OK\n", SUITE_ROW);
    std::fflush(stdout);
    return 0;
}

/* Watchdog: a row still running after `secs` prints a FAIL verdict, runs
 * the row's dump (queue lines, result slots) and exits the process, so a
 * task or PPU call that never returns fails the row instead of hanging it. */
inline void (*&watchdog_dump())() { static void (*dump)() = nullptr; return dump; }

inline void watchdog_entry(uint64_t secs)
{
    sys_timer_sleep(secs);
    std::printf("SPURS_SUITE %s FAIL watchdog got=%#x want=0\n", SUITE_ROW, (unsigned)secs);
    if (watchdog_dump())
        watchdog_dump()();
    std::fflush(stdout);
    sys_process_exit(1);
}

inline void watchdog(unsigned secs, void (*dump)() = nullptr)
{
    sys_ppu_thread_t t;
    watchdog_dump() = dump;
    sys_ppu_thread_create(&t, watchdog_entry, secs, 1000, 0x4000, 0, "suite watchdog");
}

/* poll pred() for up to ~5 s */
template <typename F> inline bool wait_for(F pred, int ms = 5000)
{
    for (int i = 0; i < ms; ++i) {
        __sync_synchronize();
        if (pred())
            return true;
        sys_timer_usleep(1000);
    }
    return false;
}

inline bool slot_done(const volatile result_slot &s, unsigned kind)
{
    return s.magic == (RESULT_MAGIC | kind);
}

/* A SPURS instance with 4 SPUs on an exclusive, non-context thread group. */
inline int spurs_up(cell::Spurs::Spurs2 *spurs, const char *prefix)
{
    cell::Spurs::SpursAttribute attr;
    int rc = cell::Spurs::SpursAttribute::initialize(&attr, 4, 100, 2, false);
    if (!rc) rc = attr.setNamePrefix(prefix, std::strlen(prefix));
    if (!rc) rc = attr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    if (!rc) rc = cell::Spurs::Spurs2::initialize(spurs, &attr);
    return rc;
}

inline cell::Spurs::Taskset *taskset_up(cell::Spurs::Spurs *spurs, int *rc)
{
    static const uint8_t prio[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    auto *ts = static_cast<cell::Spurs::Taskset *>(::aligned_alloc(CELL_SPURS_TASKSET_ALIGN, CELL_SPURS_TASKSET_SIZE));
    *rc = ts ? cell::Spurs::Taskset::create(spurs, ts, 0, prio, 4) : -1;
    return *rc ? nullptr : ts;
}

/* Launch one task of `elf` with argTask {u64 a0, u32 a2, u32 a3}.  Every
 * task gets a full context save area so it may block. */
inline int launch(cell::Spurs::Taskset *ts, const void *elf, uint64_t a0, uint32_t a2, uint32_t a3,
                  CellSpursTaskId *outId = nullptr)
{
    CellSpursTaskLsPattern ls = { { CELL_SPURS_TASK_TOP_MASK, 0xffffffffU, 0xffffffffU, 0xffffffffU } };
    void *ctx = ::aligned_alloc(CELL_SPURS_TASK_CONTEXT_ALIGN, CELL_SPURS_TASK_CONTEXT_SIZE_ALL);
    CellSpursTaskArgument arg;
    CellSpursTaskId id;
    if (!ctx)
        return -1;
    std::memset(&arg, 0, sizeof arg);
    arg.u64[0] = a0;
    arg.u32[2] = a2;
    arg.u32[3] = a3;
    int rc = ts->createTask(&id, elf, ctx, CELL_SPURS_TASK_CONTEXT_SIZE_ALL, &ls, &arg);
    if (!rc && outId)
        *outId = id;
    return rc;
}

inline void taskset_down(cell::Spurs::Taskset *ts)
{
    ts->shutdown();
    ts->join();
}

} // namespace suite

#endif
