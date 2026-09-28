/* spurs-suite job chain, SPU job (built with -mspurs-job).
 * workArea.userData[0] = result slot EA, userData[1] = magic. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_chain.h>
#include <cell/spurs/job_context.h>

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
    __attribute__((aligned(16))) uint32_t buf[4];
    buf[0] = (uint32_t)job->workArea.userData[1];
    buf[1] = ctx->dmaTag;
    buf[2] = (uint32_t)job->header.eaBinary;
    buf[3] = 0x10b5u;
    mfc_put(buf, job->workArea.userData[0], sizeof buf, ctx->dmaTag, 0, 0);
    mfc_write_tag_mask(1u << ctx->dmaTag);
    mfc_read_tag_status_all();
}
