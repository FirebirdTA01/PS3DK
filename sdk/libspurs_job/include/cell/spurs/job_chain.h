/*
 * cell/spurs/job_chain.h (SPU side) - job-runtime entry-point
 * declarations.
 *
 * On SPU side this header declares the user-supplied entry point the
 * dispatcher trampolines into:
 *
 *   void cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job);
 *
 * The trampoline itself ships in libspurs_job.a as the _start symbol
 * inside .before_text - the SPRX dispatcher branches there, and that
 * stub forwards to cellSpursJobMain2.  cellSpursJobInitialize zeroes
 * the .bss of the job binary before main runs (also in libspurs_job.a).
 *
 * The job-chain control entry points (cellSpursRunJobChain,
 * cellSpursShutdownJobChain, cellSpursAddUrgentCommand, etc.) are
 * declared with the same name on the PPU side at
 * <cell/spurs/job_chain.h>; the SPU-side prototypes live here for
 * symmetry but are intended for callers running on PPU - including
 * them from SPU code does no harm.
 */

#ifndef __PS3DK_CELL_SPURS_JOB_CHAIN_H_SPU__
#define __PS3DK_CELL_SPURS_JOB_CHAIN_H_SPU__

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs/job_context.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The user job binary's main entry.  The dispatcher passes a per-job
 * I/O context (ioBuffer / cacheBuffer / dmaTag layout) plus a pointer
 * to the in-LS copy of the job descriptor.
 */
void cellSpursJobMain2(CellSpursJobContext2 *jobContext,
                       CellSpursJob256      *job) __attribute__((unused));

/*
 * Runtime entry called once at job-binary startup to clear .bss.  Our
 * libspurs_job.a's _start trampoline calls this before tail-calling
 * cellSpursJobMain2 - jobs do not need to invoke it explicitly.
 */
void cellSpursJobInitialize(void);

/*
 * cellSpursJobStartNextJob - acknowledge the runtime-issued list-stall
 * and let the dispatcher start grabbing the next job in the chain
 * while we keep running.  Only legal when the current job's
 * jobType has CELL_SPURS_JOB_TYPE_STALL_SUCCESSOR set.
 *
 * The encoded sequence: pull the current list-stall tag mask out of
 * the channel, find the highest-numbered tag bit set (cntlz + 31-N),
 * and write it back as the ack token.  `mfc_write_list_stall_ack`
 * takes an integer tag, not a mask.
 */
static inline int cellSpursJobStartNextJob(CellSpursJob256 *job)
{
    if (__builtin_expect(!(job->header.jobType & CELL_SPURS_JOB_TYPE_STALL_SUCCESSOR), 0))
        return CELL_SPURS_JOB_ERROR_INVAL;

    unsigned mask = mfc_read_list_stall_status();
    /* Index of the highest set bit in `mask` is 31 minus its leading
     * zero count.  spu_promote/spu_cntlz/spu_extract is a one-cycle
     * vector ladder. */
    unsigned tag  = 31u - spu_extract(spu_cntlz(spu_promote(mask, 0)), 0);
    mfc_write_list_stall_ack(tag);
    return CELL_OK;
}

/*
 * Job-chain control - PPU-callable but declared here for symmetry
 * with the reference SDK's SPU-side header layout.
 */
int cellSpursKickJobChain      (uint64_t eaJobChain, uint8_t numReadyCount);
int cellSpursRunJobChain       (uint64_t eaJobChain);
int cellSpursShutdownJobChain  (uint64_t eaJobChain);
int cellSpursJobSetMaxGrab     (uint64_t eaJobChain, unsigned int maxGrab);

/* Job-guard control. */
int cellSpursJobGuardInitialize(uint64_t eaJobChain,
                                uint64_t eaJobGuard,
                                uint32_t notifyCount,
                                uint8_t  requestSpuCount,
                                uint8_t  autoReset,
                                unsigned int tag);
int cellSpursJobGuardReset (uint64_t eaJobGuard);
int cellSpursJobGuardNotify(uint64_t eaJobGuard);

/* Urgent-command injection. */
int cellSpursAddUrgentCommand(uint64_t eaJobChain, uint64_t command);
int cellSpursAddUrgentCall   (uint64_t eaJobChain, uint64_t commandList);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_JOB_CHAIN_H_SPU__ */
