/* SPU side of hello-spurs-semaphore.
 *
 * One ELF, two roles selected by argTask.u32[3] high half:
 *   kind == 0  producer:  V() the semaphore N times
 *   kind == 1  consumer:  P() N times, after each P bump a counter
 *                         in main memory (PPU watches it).
 *
 * argTask.u64[0] = EA of CellSpursSemaphore (16-byte aligned, 128B)
 * argTask.u32[2] = EA of consumer counter buffer (low 32 bits)
 * argTask.u32[3] = (role << 16) | iterations
 */

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs/spu_task.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/semaphore.h>

static inline void dma_put_counter(uint64_t ea, uint32_t value)
{
    __attribute__((aligned(16))) uint32_t buf[4] = { value, 0, 0, 0 };
    mfc_put(buf, ea, 16, /*tag=*/0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();
}

void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    (void)argTaskset;

    CellSpursTaskArgument arg;
    *(qword *)&arg = argTask;

    uint64_t sem_ea     = arg.u64[0];
    uint64_t counter_ea = (uint64_t)arg.u32[2];
    uint32_t kind       = arg.u32[3] >> 16;
    uint32_t iters      = arg.u32[3] & 0xffffU;

    if (kind == 0) {
        for (uint32_t i = 0; i < iters; ++i) {
            int rc = cellSpursSemaphoreV(sem_ea);
            if (rc) cellSpursTaskExit(0x100 | (int)i);
        }
        cellSpursTaskExit(0);
    } else {
        for (uint32_t i = 0; i < iters; ++i) {
            int rc = cellSpursSemaphoreP(sem_ea);
            if (rc) {
                /* Stash error in counter for PPU visibility. */
                dma_put_counter(counter_ea, 0xfa11ed00u | (uint32_t)rc);
                cellSpursTaskExit(0x200 | (int)i);
            }
            dma_put_counter(counter_ea, i + 1);
        }
        cellSpursTaskExit(0);
    }
}
