/* sync2-objects SPU job (built with -mspurs-job): releases a semaphore as a
 * SPURS job, which wakes the PPU thread waiting on it, and reports the rc.
 * gCellSync2ThreadConfigSpursJob is data full of addresses (the caller
 * thread type, the notifier table), so this also checks that a job's
 * pointer-holding data is valid where the job runs. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_chain.h>
#include <cell/spurs/job_context.h>
#include <cell/sync2.h>
#include "../source/common.h"

/* heap 256 B, stack 4 KB: the job binary wrapper requires the LS parameters */
__asm__(
    ".pushsection .data\n"
    ".global _cell_spu_ls_param\n"
    ".align 4\n"
    ".type _cell_spu_ls_param, @object\n"
    ".size _cell_spu_ls_param, 16\n"
    "_cell_spu_ls_param:\n"
    ".long 0x100, 0x1000, 0, 0\n"
    ".popsection\n"
    ".pushsection .cell_spu_ls_param\n"
    ".long 0x100, 0x1000\n"
    ".popsection\n");

void cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    __attribute__((aligned(16))) uint32_t out[4];
    int rc = cellSync2SemaphoreRelease(job->workArea.userData[0], (unsigned int)job->workArea.userData[1],
                                       &gCellSync2ThreadConfigSpursJob, ctx->dmaTag);
    out[0] = JOB_MAGIC;
    out[1] = (uint32_t)rc;
    out[2] = 0;
    out[3] = 0;
    mfc_put(out, job->workArea.userData[2], sizeof out, ctx->dmaTag, 0, 0);
    mfc_write_tag_mask(1u << ctx->dmaTag);
    mfc_read_tag_status_all();
}
