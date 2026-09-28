/* spurs-suite job extras, SPU job (built with -mspurs-job).
 * workArea.userData[0] = 16-byte output EA, [1] = magic, [2] = mode.
 * X_JOB_PLAIN writes { magic, 0, 0, 0 }.  X_JOB_MEMCHECK writes
 * { magic, Initialize rc for a plain job, Initialize rc for a null
 * context, Test rc before Initialize | cause << 16 }. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_chain.h>
#include <cell/spurs/job_context.h>
#include "../job_extras.h"

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
    __attribute__((aligned(16))) uint32_t buf[4] = { 0, 0, 0, 0 };
    buf[0] = (uint32_t)job->workArea.userData[1];
    if (job->workArea.userData[2] == X_JOB_MEMCHECK) {
        /* the paths that write no guards: a plain job's header is refused,
           a null argument too, and Test before any Initialize */
        uint16_t cause = 0xffff;
        buf[1] = (uint32_t)cellSpursJobMemoryCheckInitialize(ctx, &job->header);
        buf[2] = (uint32_t)cellSpursJobMemoryCheckInitialize(0, &job->header);
        buf[3] = ((uint32_t)cellSpursJobMemoryCheckTest(&cause) & 0xffffu) | ((uint32_t)cause << 16);
    }
    mfc_put(buf, job->workArea.userData[0], sizeof buf, ctx->dmaTag, 0, 0);
    mfc_write_tag_mask(1u << ctx->dmaTag);
    mfc_read_tag_status_all();
}
