/* spu-fiber-signal row, SPU task: signal a PPU fiber waiting in
 * cellFiberPpuWaitSignal, either through the fiber utility worker control
 * (FS_UTIL_SIGNAL) or with cellFiberPpuSendSignal and an explicit worker
 * wake-up (FS_PLAIN_SIGNAL). */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/task.h>
#include <cell/fiber.h>
#include <cell/fiber/ppu_fiber_worker_control.h>
#include "../fiber_signal.h"

static fs_box box;

int cellSpursTaskMain(qword argTask, uint64_t argTaskset)
{
    struct { uint64_t box; uint32_t a2, kind; } arg __attribute__((aligned(16)));
    unsigned int numWorker = 0xffffffffu;
    uint32_t scheduler = 0;
    int rc;
    (void)argTaskset;
    *(qword *)&arg = argTask;
    mfc_get(&box, arg.box, 64, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();

    rc = cellFiberPpuGetScheduler(box.fiber, &scheduler);
    if (!rc) {
        if (arg.kind == FS_UTIL_SIGNAL) {
            rc = cellFiberPpuUtilWorkerControlSendSignal(box.fiber, &numWorker);
        } else {
            rc = cellFiberPpuSendSignal(box.fiber, &numWorker);
            if (!rc)
                rc = cellFiberPpuUtilWorkerControlWakeup(box.runtime);
        }
    }
    box.state = 2;
    box.rc = (uint32_t)rc;
    box.numWorker = numWorker;
    box.scheduler = scheduler;
    mfc_put(&box.state, arg.box + 64, 16, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    return 0;
}
