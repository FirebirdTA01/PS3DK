/* spurs-suite barrier, SPU task (C++: exercises cell::Spurs::BarrierStub).
 * argTask: u64[0] = result slot array EA, u32[2] = barrier EA,
 * u32[3] = task index.  The shared notify counter is the first word of
 * the line after the barrier. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task.h>
#include "../barrier.h"

#define TASK_BUSY ((int)0x8041090Au)
#define TASK_NULL ((int)0x80410911u)
#define TASK_STAT ((int)0x8041090Fu)

static result_slot out;
static uint32_t line[32] __attribute__((aligned(128)));

static void report(uint64_t slots, unsigned slot, int status, unsigned value, unsigned extra)
{
    out.magic = RESULT_MAGIC | slot;
    out.status = (unsigned)status;
    out.value = value;
    out.extra = extra;
    mfc_put(&out, slots + slot * sizeof(result_slot), sizeof out, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

/* atomically add 1 to the shared counter; returns the new value */
static unsigned bump(uint64_t counter)
{
    uint64_t lineEa = counter & ~127ull;
    unsigned idx = (unsigned)(counter & 127) / 4, v;
    do {
        mfc_getllar(line, lineEa, 0, 0);
        (void)mfc_read_atomic_status();
        v = ++line[idx];
        mfc_putllc(line, lineEa, 0, 0);
    } while (mfc_read_atomic_status() & MFC_PUTLLC_STATUS);
    return v;
}

static unsigned peek(uint64_t counter)
{
    mfc_getllar(line, counter & ~127ull, 0, 0);
    (void)mfc_read_atomic_status();
    return line[(unsigned)(counter & 127) / 4];
}

extern "C" void cellSpursMain(qword argTask, uint64_t)
{
    CellSpursTaskArgument arg;
    *(qword *)&arg = argTask;
    uint64_t slots = arg.u64[0];
    uint64_t counter = (uint64_t)arg.u32[2] + 128;
    unsigned index = arg.u32[3];
    cell::Spurs::BarrierStub barrier;
    barrier.setObject(arg.u32[2]);

    if (index == B_ERRORS) {
        cell::Spurs::BarrierStub none;
        none.setObject(0);
        uint64_t ts = 0;
        int rc = barrier.getTasksetAddress(&ts);
        if (rc || ts != cellSpursGetTasksetAddress())
            report(slots, index, rc ? rc : -1, 1, (unsigned)ts);
        else if ((rc = none.wait()) != TASK_NULL)
            report(slots, index, rc ? rc : -1, 2, 0);
        else
            report(slots, index, 0, 0, 0);
        return;
    }

    int early = barrier.tryWait();                    /* not released yet: BUSY */
    if (index == B_LATE)
        for (volatile unsigned spin = 0; spin < 2000000; ++spin)
            ;
    bump(counter);
    int rc = barrier.notify();
    if (!rc)
        rc = barrier.wait();
    report(slots, index, rc, peek(counter), (unsigned)early);
}
