/* spurs-suite event flag, SPU task.  argTask: u64[0] = event flag EA,
 * u32[2] = result slot EA, u32[3] = kind (values.h). */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/event_flag.h>
#include "../event_flag.h"

static result_slot out;

static void report(uint64_t ea, unsigned kind, int status, unsigned bits, unsigned extra)
{
    out.magic = RESULT_MAGIC | kind;
    out.status = (unsigned)status;
    out.value = bits;
    out.extra = extra;
    mfc_put(&out, ea, sizeof out, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

/* the u16 at `off` inside the 128-byte flag */
static uint16_t flag_u16(uint64_t ef, unsigned off)
{
    static uint8_t line[128] __attribute__((aligned(128)));
    mfc_get(line, ef, 128, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    return (uint16_t)((line[off] << 8) | line[off + 1]);
}

void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    CellSpursTaskArgument arg;
    uint64_t ef, slot;
    unsigned kind;
    uint16_t bits = 0;
    int rc;
    (void)argTaskset;
    *(qword *)&arg = argTask;
    ef = arg.u64[0];
    slot = arg.u32[2];
    kind = arg.u32[3];

    switch (kind) {
    case KIND_SET_SPU2PPU: {
        unsigned spins = 0;
        while (flag_u16(ef, 0x04) == 0 && ++spins < 2000000)   /* ppuWaitMask */
            ;
        rc = cellSpursEventFlagSet(ef, 0x0003);
        report(slot, kind, rc, 0x0003, spins);
        break;
    }
    case KIND_WAIT_PPU2SPU:
        bits = 0x00f0;
        rc = cellSpursEventFlagWait(ef, &bits, CELL_SPURS_EVENT_FLAG_OR);
        report(slot, kind, rc, bits, 0);
        break;
    case KIND_WAIT_SPU2SPU: {
        uint16_t busy = 0x0400;
        CellSpursEventFlagDirection dir = (CellSpursEventFlagDirection)0xff;
        CellSpursEventFlagClearMode mode = (CellSpursEventFlagClearMode)0xff;
        int trc;
        bits = 0x0300;
        rc = cellSpursEventFlagWait(ef, &bits, CELL_SPURS_EVENT_FLAG_AND);
        trc = cellSpursEventFlagTryWait(ef, &busy, CELL_SPURS_EVENT_FLAG_OR);
        if (!rc)
            rc = cellSpursEventFlagClear(ef, 0x0100);
        if (!rc)
            rc = cellSpursEventFlagGetDirection(ef, &dir);
        if (!rc)
            rc = cellSpursEventFlagGetClearMode(ef, &mode);
        report(slot, kind, rc, bits,
               (((unsigned)trc & 0xffffu) << 16) | (((unsigned)dir & 0xffu) << 8) | ((unsigned)mode & 0xffu));
        break;
    }
    case KIND_SET_SPU2SPU:
        rc = cellSpursEventFlagSet(ef, 0x0100);
        if (!rc)
            rc = cellSpursEventFlagSet(ef, 0x0200);
        report(slot, kind, rc, 0x0300, 0);
        break;
    case KIND_DIAG_YIELD: {
        unsigned sp;
        __asm__ volatile ("ori %0, $1, 0" : "=r"(sp));    /* this task's stack pointer */
        report(slot, 0x100 | kind, 0, sp, 0x11);          /* progress: before */
        rc = cellSpursYield();
        report(slot, kind, rc, sp, 0x12);
        break;
    }
    case KIND_DIAG_SIGNAL_SELF:
        report(slot, 0x100 | kind, 0, 0, 0x21);
        rc = cellSpursSendSignal(cellSpursGetTasksetAddress(), cellSpursGetTaskId());
        report(slot, 0x100 | kind, rc, 0, 0x22);
        if (!rc)
            rc = cellSpursWaitSignal();
        report(slot, kind, rc, 0, 0x23);
        break;
    case KIND_DIAG_WAIT_SIGNAL:
        report(slot, 0x100 | kind, 0, cellSpursGetTaskId(), 0x31);  /* PPU signals after this */
        rc = cellSpursWaitSignal();
        report(slot, kind, rc, 0, 0x32);
        break;
    default:
        report(slot, kind, -1, 0, 0);
        break;
    }
    cellSpursTaskExit(0);
}
