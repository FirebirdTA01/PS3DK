/*
 * hello-spurs-jq SPU side - per-job entry function.
 *
 * Each time the JQ scheduler dispatches a job to this SPU, the
 * framework's cellSpursJobMain2 (in libspurs_jq) runs:
 *   SysCall::__initialize(ctx, job)
 *   cellSpursJobQueueMain(ctx, job)   <-- this function
 *   SysCall::__finalize(ctx)
 *
 * The user-defined cellSpursJobQueueMain receives a per-job context
 * and the job descriptor.  It uses DMAs to read input data, do work,
 * and write results back to main memory.
 *
 * Test body: read userData[0..1] from the descriptor (out_ea, magic),
 * write the magic + a few diagnostic words back to *out_ea, signal
 * the PPU via DMA-PUT.  Mirrors hello-spu-job's pattern so this is
 * directly comparable when iterating runtime issues.
 */

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_context.h>

void cellSpursJobQueueMain(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    if (job == 0)
        return;

    uint64_t out_ea = job->workArea.userData[0];
    uint64_t magic  = job->workArea.userData[1];

    __attribute__((aligned(16))) uint32_t buf[4];
    buf[0] = (uint32_t)magic;
    buf[1] = (ctx != 0) ? ctx->dmaTag : 0xDEADBEEFu;
    buf[2] = (uint32_t)(job->header.eaBinary);
    buf[3] = (uint32_t)(job->header.eaBinary >> 32);

    /* Use tag 0 - the runtime's per-job DMA tag pool isn't wired yet;
     * fixed-slot DMA works for the throughput test even with multiple
     * jobs racing because we wait for our own put to drain before
     * returning. */
    mfc_put(buf, out_ea, sizeof(buf), 0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();
}
