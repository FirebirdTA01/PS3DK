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
 * Until the SPU runtime is fleshed out beyond stubs, the SysCall
 * init/finalize bodies don't actually populate ctx; this function
 * is reached only via the cellSpursJobMain2 wrapper and won't see
 * meaningful arguments yet.  Kept minimal so the link succeeds and
 * a future RPCS3 run can validate the wrapper plumbing.
 */

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_context.h>

void cellSpursJobQueueMain(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
{
    (void)ctx;
    (void)job;
    /* TODO: real job body once SysCall::__initialize populates ctx.
     * For now this is just the link anchor that brings in
     * libspurs_jq's cellSpursJobMain2 wrapper. */
}
