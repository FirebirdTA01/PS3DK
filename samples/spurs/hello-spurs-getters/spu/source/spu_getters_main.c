/* hello-spurs-getters - SPU side.
 *
 * Calls each libspurs SPU module-runtime helper and DMAs the values
 * back to the PPU.  Layout matches the `Result` struct in
 * source/main.cpp - 64 bytes total:
 *
 *   u64 spursAddress      // cellSpursGetSpursAddress()
 *   u32 currentSpuId      // cellSpursGetCurrentSpuId()
 *   u32 tagId             // cellSpursGetTagId()
 *   u32 workloadId        // cellSpursGetWorkloadId()
 *   u32 spuCount          // cellSpursGetSpuCount()
 *   u64 elfAddress        // cellSpursGetElfAddress()
 *   u32 iwlTaskId         // _cellSpursGetIWLTaskId()
 *   u32 modulePollResult  // cellSpursModulePoll()  (0 = no preempt)
 *   u32 cellSpursPoll_b   // cellSpursPoll()
 *   u32 sentinel          // 0xC0FFEE99
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
        uint32_t pad0;
        uint64_t elfAddress;
        uint32_t iwlTaskId;
        uint32_t modulePollResult;
        uint32_t cellSpursPoll_b;
        uint32_t sentinel;
    } result;

    result.spursAddress     = cellSpursGetSpursAddress();
    result.currentSpuId     = cellSpursGetCurrentSpuId();
    result.tagId            = cellSpursGetTagId();
    result.workloadId       = cellSpursGetWorkloadId();
    result.spuCount         = cellSpursGetSpuCount();
    result.pad0             = 0;
    result.elfAddress       = cellSpursGetElfAddress();
    result.iwlTaskId        = _cellSpursGetIWLTaskId();
    result.modulePollResult = (uint32_t)cellSpursModulePoll();
    result.cellSpursPoll_b  = cellSpursPoll() ? 1u : 0u;
    result.sentinel         = 0xc0ffee99u;

    mfc_put(&result, result_ea, 64, /*tag*/0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();

    cellSpursTaskExit(0);
}
