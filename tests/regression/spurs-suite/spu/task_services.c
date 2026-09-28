/* spurs-suite task services, SPU side.  argTask: u64[0] = result slot
 * array EA, u32[2] = semaphore EA (kinds 4/5), u32[3] = kind.  Built twice:
 * linked by hand (spurs_task.ld + libspurs_task) and with -mspurs-task. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/semaphore.h>
#include "../common.h"
#include "../task_services.h"

static result_slot out;

static void report(uint64_t slots, unsigned kind, int status, unsigned value, unsigned extra)
{
    out.magic = RESULT_MAGIC | kind;
    out.status = (unsigned)status;
    out.value = value;
    out.extra = extra;
    mfc_put(&out, slots + kind * sizeof(result_slot), sizeof out, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    CellSpursTaskArgument arg;
    uint64_t slots;
    unsigned kind, sp;
    int rc = 0;
    *(qword *)&arg = argTask;
    slots = arg.u64[0];
    kind = arg.u32[3];
    __asm__ volatile ("ori %0, $1, 0" : "=r"(sp));

    switch (kind) {
    case TS_GETTERS:
        /* value = task id, extra = stack pointer; the PPU compares the rest */
        report(slots, TS_GETTERS_TASKSET, 0, (unsigned)cellSpursGetTasksetAddress(),
               (unsigned)argTaskset);
        report(slots, TS_GETTERS_SPURS, 0, (unsigned)cellSpursGetSpursAddress(),
               cellSpursGetCurrentSpuId() | (cellSpursGetWorkloadId() << 8));
        /* the back chain in cellSpursMain's frame is the crt's SP */
        report(slots, TS_CRT_FRAME, 0, *(volatile unsigned *)(uintptr_t)sp, 0);
        report(slots, kind, 0, cellSpursGetTaskId(), sp);
        break;
    case TS_YIELD:
        rc = cellSpursYield();
        report(slots, kind, rc, 0, sp);
        break;
    case TS_SIGNAL_SELF:
        rc = cellSpursSendSignal(cellSpursGetTasksetAddress(), cellSpursGetTaskId());
        if (!rc)
            rc = cellSpursWaitSignal();   /* already pending: returns without blocking */
        report(slots, kind, rc, 0, sp);
        break;
    case TS_WAIT_SIGNAL:
        report(slots, TS_WAIT_SIGNAL_READY, 0, cellSpursGetTaskId(), sp);
        rc = cellSpursWaitSignal();       /* blocks until the PPU signals */
        report(slots, kind, rc, 0, sp);
        break;
    case TS_SEM_CONSUMER: {
        unsigned i;
        for (i = 0; i < TS_SEM_ROUNDS && !rc; ++i)
            rc = cellSpursSemaphoreP(arg.u32[2]);   /* blocks until the producer V's */
        report(slots, kind, rc, i, sp);
        break;
    }
    case TS_SEM_PRODUCER: {
        unsigned i;
        for (i = 0; i < TS_SEM_ROUNDS && !rc; ++i) {
            rc = cellSpursSemaphoreV(arg.u32[2]);
            cellSpursYield();
        }
        report(slots, kind, rc, i, sp);
        break;
    }
    default:
        report(slots, kind, -1, 0, sp);
        break;
    }
    cellSpursTaskExit(0);
}
