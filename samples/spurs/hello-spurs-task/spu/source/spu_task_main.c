/* hello-ppu-spurs-task - SPU-side task code.
 *
 * The Spurs SPRX loads this ELF, sets SP + argTask (r3) + argTaskset
 * (r4) per the Cell OS SPU ABI, and branches to our crt at
 * __spurs_task_start, which falls through to cellSpursMain below.
 *
 * argTask.u64[0] is a PPU-visible EA of a 16-byte flag buffer.  We
 * DMA 0xC0FFEE into it, so the PPU can observe the task ran.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs/spu_task.h>
#include <cell/spurs/task_types.h>

void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    (void)argTaskset;

    /* argTask is a CellSpursTaskArgument (union u32[4] / u64[2]).
     * The first u64 carries the flag EA. */
    CellSpursTaskArgument arg;
    *(qword *)&arg = argTask;
    uint64_t flag_ea = arg.u64[0];

    __attribute__((aligned(16))) uint32_t buf[4] = { 0xc0ffeeU, 0, 0, 0 };
    mfc_put(buf, flag_ea, 16, /*tag*/0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();

    cellSpursTaskExit(0);
}
