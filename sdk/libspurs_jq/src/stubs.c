/* libspurs_jq stubs.c - placeholder bodies for the SPU job-queue
 * runtime entry points.
 *
 * Each entry point is declared in cell/spurs/job_queue*.h and
 * referenced from the host stub archive libspurs_jq_stub.a.  This
 * file ships return-CELL_OK / return-INVAL stubs so that an SPU
 * binary destined for a JQ workload can LINK; runtime correctness
 * is staged in incrementally over later changes.
 *
 * House rule: every stub returning CELL_SPURS_JOB_ERROR_INVAL marks
 * an unimplemented path the SPU runtime work will fill in. The default
 * entry adapter lives separately in job_queue_main.c and requires an
 * application-defined cellSpursJobQueueMain; it supplies no dummy job.
 */

#include <stdint.h>
#include <stddef.h>

#include <cell/spurs/error.h>
#include <cell/spurs/types.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/job_context.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/spurs/job_queue_port2.h>
#include <cell/spurs/job_queue_semaphore.h>

#define _STUB_OK    0
#define _STUB_INVAL CELL_SPURS_JOB_ERROR_INVAL

#define _UNUSED(x) ((void)(x))

/* The default Main2-to-QueueMain adapter is a separate archive member in
 * job_queue_main.c. Direct Main2 clients can use these helpers without
 * pulling in a competing entry definition or a QueueMain dependency. */

/* CRT-Aux init/finalize - the outer CRT-layer wrappers __job_start
 * (in job_crt.S) calls before/after the SysCall layer.  Reference
 * bodies (~700 / ~250 bytes) snapshot job header + context quadwords
 * into _g_cellSpursJobMemoryCheckJob{Header,Context} markers, set
 * up trace state, configure cooperative-yield function pointers,
 * etc.  Stubbed to immediate return until those are wired. */
int _cellSpursJobCrtAuxInitialize(CellSpursJobContext2 *ctx,
                                  CellSpursJob256 *job)
{
    _UNUSED(ctx); _UNUSED(job);
    return _STUB_OK;
}

void _cellSpursJobCrtAuxFinalize(CellSpursJobContext2 *ctx)
{
    _UNUSED(ctx);
}

/* CRT0 ctors / dtors - normally driven by .ctors / .dtors lists.
 * For a freestanding JQ job there's nothing to construct or destruct,
 * so these are no-ops. */
void _init(void)  { /* no-op */ }
void _fini(void)  { /* no-op */ }

/* atexit hook - reference SDK uses this for fini-time cleanup; we
 * have no atexit registrations. */
void __do_atexit(void) { /* no-op */ }

int cellSpursJobQueueGetSuspendedJobSize(const CellSpursJobHeader *pJob,
                                         size_t sizeJobDesc,
                                         enum CellSpursJobQueueSuspendedJobAttribute attr,
                                         unsigned int *pSize)
{
    _UNUSED(pJob); _UNUSED(sizeJobDesc); _UNUSED(attr);
    if (pSize) *pSize = 0;
    return _STUB_OK;
}

/* -- JQ push body NIDs ----------------------------------------------- */

/* -- Port surface --------------------------------------------------- */

/* -- Port2 surface -------------------------------------------------- */

/* -- Memcheck / hash check / trace ---------------------------------- */

void _cellSpursJobQueueHashCheck(void)
{
    /* no-op */
}

void _cellSpursJobQueueRehash(void)
{
    /* no-op */
}

void _cellSpursJobQueueTraceDump(void)
{
    /* no-op */
}

/* -- Trace / memcheck globals --------------------------------------- */

unsigned int                    _gCellSpursJobQueueIsTraceEnabled        = 0;
unsigned int                    _gCellSpursJobQueueTraceRemainingCount   = 0;
_CellSpursJobQueueTracePacket  *_gCellSpursJobQueueTracePutPtr            = 0;
_CellSpursJobQueueTracePacket   _gCellSpursJobQueueTracePacketYield       = {{0,0,0,0,0},{0}};
_CellSpursJobQueueTracePacket   _gCellSpursJobQueueTracePacketSleep       = {{0,0,0,0,0},{0}};
_CellSpursJobQueueTracePacket   _gCellSpursJobQueueTracePacketResume      = {{0,0,0,0,0},{0}};

unsigned int _gCellSpursJobQueueIsMemCheckEnabled __attribute__((aligned(16))) = 0;
unsigned int _gCellSpursJobQueueYieldHasAuxProc   __attribute__((aligned(16))) = 0;
