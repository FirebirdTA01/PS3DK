/* spurs-suite PPU harness: SPURS bring-up, a taskset, task launch,
 * bounded polling and the row verdict.  Every row prints exactly one of
 *   SPURS_SUITE <row> OK
 *   SPURS_SUITE <row> FAIL <step> got=<x> want=<y>
 *   SPURS_SUITE <row> INVALID <step> rc=<rc>     (setup could not run)
 * and returns 0, 1 or 2.  Verdicts are flushed at once: a failing row may
 * leave a task blocked, and then its teardown never returns.
 *
 * A row is built standalone (its own self, main()) or, with SUITE_EMBEDDED,
 * as entry function SUITE_ENTRY of the on-screen status sample.  Either
 * way the harness keeps a live view of the row (suite::live()): the
 * current activity, a short activity log, and the SPU task result slots
 * the row registered, which the status sample draws every frame. */
#ifndef SPURS_SUITE_HARNESS_H
#define SPURS_SUITE_HARNESS_H

#include <cstdarg>
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

/* ---- live view --------------------------------------------------------- */

enum { LOG_LINES = 6, TEXT = 72, MAX_SLOTS = 16 };

struct live_view {
    const char *row;                          /* current row */
    volatile unsigned seq;                    /* bumps on every update */
    char activity[TEXT];                      /* what the PPU is doing now */
    char log[LOG_LINES][TEXT];                /* recent activities, ring */
    volatile unsigned logCount;
    const volatile result_slot *slots;        /* the row's SPU result slots */
    const char *const *slotNames;
    unsigned nSlots;
    char verdict[TEXT * 2];                   /* the row's verdict line */
    volatile int code;                        /* -1 running, 0 OK, 1 FAIL, 2 INVALID */
};

/* shared by every row linked into one program */
live_view &live();

/* Record what the row is doing (shown live by the status sample) */
inline void activity(const char *fmt, ...)
{
    live_view &v = live();
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(v.activity, sizeof v.activity, fmt, ap);
    va_end(ap);
    std::memcpy(v.log[v.logCount % LOG_LINES], v.activity, sizeof v.activity);
    v.logCount = v.logCount + 1;
    v.seq = v.seq + 1;
}

/* Show the row's task result slots (names optional, one per slot) */
inline void watch_slots(const volatile result_slot *slots, unsigned n, const char *const *names = nullptr)
{
    live_view &v = live();
    v.slots = slots;
    v.nSlots = n > (unsigned)MAX_SLOTS ? (unsigned)MAX_SLOTS : n;
    v.slotNames = names;
    v.seq = v.seq + 1;
}

/* ---- verdicts ---------------------------------------------------------- */

inline int verdict(int code, const char *fmt, ...)
{
    live_view &v = live();
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(v.verdict, sizeof v.verdict, fmt, ap);
    va_end(ap);
    std::printf("SPURS_SUITE %s %s\n", v.row, v.verdict);
    std::fflush(stdout);
    v.code = code;
    v.seq = v.seq + 1;
    return code;
}

inline int invalid(const char *step, int rc)
{
    return verdict(2, "INVALID %s rc=%#x", step, rc);
}

inline int fail(const char *step, unsigned got, unsigned want)
{
    return verdict(1, "FAIL %s got=%#x want=%#x", step, got, want);
}

inline int ok()
{
    return verdict(0, "OK");
}

/* Watchdog: a standalone row still running after `secs` prints a FAIL
 * verdict, runs the row's dump (queue lines, result slots) and exits the
 * process, so a task or PPU call that never returns fails the row instead
 * of hanging it.  Embedded rows are timed by the status sample instead. */
inline void (*&watchdog_dump())() { static void (*dump)() = nullptr; return dump; }

inline void watchdog_entry(uint64_t secs)
{
    sys_timer_sleep(secs);
    fail("watchdog", (unsigned)secs, 0);
    if (watchdog_dump())
        watchdog_dump()();
    std::fflush(stdout);
    sys_process_exit(1);
}

inline void watchdog(unsigned secs, void (*dump)() = nullptr)
{
#ifdef SUITE_EMBEDDED
    (void)secs;
    (void)dump;
#else
    sys_ppu_thread_t t;
    watchdog_dump() = dump;
    sys_ppu_thread_create(&t, watchdog_entry, secs, 1000, 0x4000, 0, "suite watchdog");
#endif
}

/* ---- SPURS helpers ----------------------------------------------------- */

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
    activity("starting SPURS (4 SPUs)");
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
    activity("creating taskset");
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
    activity("launched SPU task %u (kind %u) rc=%#x", rc ? 0xffu : (unsigned)id, a3, rc);
    return rc;
}

inline void taskset_down(cell::Spurs::Taskset *ts)
{
    activity("shutting down taskset");
    ts->shutdown();
    activity("joining taskset");
    ts->join();
}

} // namespace suite

/* The row's entry point: main() standalone, SUITE_ENTRY embedded. */
#ifdef SUITE_EMBEDDED
#define SUITE_ENTRY_POINT(fn) \
    int SUITE_ENTRY() { suite::live().row = SUITE_ROW; suite::live().code = -1; return fn(); }
#else
#define SUITE_ENTRY_POINT(fn) \
    SYS_PROCESS_PARAM(1001, 0x10000) \
    suite::live_view &suite::live() { static live_view v; return v; } \
    int main() { suite::live().row = SUITE_ROW; suite::live().code = -1; return fn(); }
#endif

#endif
