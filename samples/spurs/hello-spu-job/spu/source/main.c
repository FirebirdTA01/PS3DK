/*
 * hello-spu-job - SPU side.
 *
 * A real cellSpursJobMain2 binary.  The dispatcher branches into our
 * libspurs_job _start trampoline, which clears .bss via
 * cellSpursJobInitialize and tail-calls into the routine below.
 *
 * The PPU passes us:
 *   job->workArea.userData[0] = EA of a 16-byte sentinel buffer
 *   job->workArea.userData[1] = a magic value the PPU expects back
 *
 * We DMA-put a 16-byte block back to that EA whose first u32 lane is
 * the magic, second lane is the jobContext->dmaTag (so the PPU has
 * proof we read the per-job context), and the remaining lanes echo the
 * job's eaBinary low/high halves.
 */

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs/job_chain.h>
#include <cell/spurs/job_context.h>

void cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    uint64_t out_ea = job->workArea.userData[0];
    uint64_t magic  = job->workArea.userData[1];

    __attribute__((aligned(16))) uint32_t buf[4];
    buf[0] = (uint32_t)magic;
    buf[1] = ctx->dmaTag;
    buf[2] = (uint32_t)(job->header.eaBinary);
    buf[3] = (uint32_t)(job->header.eaBinary >> 32);

    mfc_put(buf, out_ea, sizeof(buf), ctx->dmaTag, 0, 0);
    mfc_write_tag_mask(1u << ctx->dmaTag);
    mfc_read_tag_status_all();
}
