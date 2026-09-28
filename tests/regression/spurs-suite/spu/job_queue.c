/* spurs-suite job queue, SPU job (built with -mspurs-job-initialize; the
 * runtime's cellSpursJobMain2 calls this entry).
 * workArea.userData[0] = result slot EA, userData[1] = magic. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_context.h>

void cellSpursJobQueueMain(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    __attribute__((aligned(16))) uint32_t buf[4];
    if (!job)
        return;
    buf[0] = (uint32_t)job->workArea.userData[1];
    buf[1] = ctx ? ctx->dmaTag : 0xdeadbeefu;
    buf[2] = (uint32_t)job->header.eaBinary;
    buf[3] = 0x10b6u;
    mfc_put(buf, job->workArea.userData[0], sizeof buf, 0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();
}
