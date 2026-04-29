/* hello-spurs-getters - SPU side.
 *
 * Calls each of the libspurs SPU module-runtime getters and DMAs
 * the values back to the PPU as a 32-byte block.  Layout matches
 * the `Result` struct in source/main.cpp:
 *
 *   u64 spursAddress      // cellSpursGetSpursAddress()
 *   u32 currentSpuId      // cellSpursGetCurrentSpuId()
 *   u32 tagId             // cellSpursGetTagId()
 *   u32 workloadId        // cellSpursGetWorkloadId()
 *   u32 spuCount          // cellSpursGetSpuCount()
 *   u32 sentinel          // 0xC0FFEE99 - lets the PPU tell "task ran"
 *                         //              from "task got skipped"
 *   u32 _pad              // alignment
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs/spu_task.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/common.h>

void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    (void)argTaskset;

    CellSpursTaskArgument arg;
    *(qword *)&arg = argTask;
    uint64_t result_ea = arg.u64[0];

    __attribute__((aligned(16))) struct {
        uint64_t spursAddress;
        uint32_t currentSpuId;
        uint32_t tagId;
        uint32_t workloadId;
        uint32_t spuCount;
        uint32_t sentinel;
        uint32_t pad;
    } result;

    result.spursAddress = cellSpursGetSpursAddress();
    result.currentSpuId = cellSpursGetCurrentSpuId();
    result.tagId        = cellSpursGetTagId();
    result.workloadId   = cellSpursGetWorkloadId();
    result.spuCount     = cellSpursGetSpuCount();
    result.sentinel     = 0xc0ffee99u;
    result.pad          = 0;

    mfc_put(&result, result_ea, 32, /*tag*/0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();

    cellSpursTaskExit(0);
}
