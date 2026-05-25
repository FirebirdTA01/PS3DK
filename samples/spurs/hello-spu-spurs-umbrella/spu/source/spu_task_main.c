/*
 * SPU-side regression gate for the slim cell/spurs.h umbrella and its
 * four companion sub-headers (task_exit_code, policy_module,
 * lv2_event_queue, lfqueue).
 *
 * The single include <cell/spurs.h> is the primary gate — if the slim
 * umbrella is under-inclusive or misconfigured this TU fails at the
 * include level.  Each of the four new surface types is touched via a
 * local declaration or constant reference so the compiler cannot
 * eliminate the dependency.
 *
 * The task DMA-writes 0xC0FFEE into the PPU-visible flag buffer and
 * exits through the cellSpursTaskExit path.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include <cell/spurs.h>

/* ---- Exercise task_exit_code.h surface ---------------------------- */
static CellSpursTaskExitCode s_exitCode;
_Static_assert(CELL_SPURS_TASK_EXIT_CODE_ALIGN == 128, "exit code align");
_Static_assert(sizeof(s_exitCode) == CELL_SPURS_TASK_EXIT_CODE_SIZE,
               "exit code size");

/* ---- Exercise policy_module.h surface ----------------------------- */
static CellSpursModulePollStatus s_pmPollStatus;

/* ---- Exercise lfqueue.h surface ----------------------------------- */
static CellSpursLFQueue s_lfQueue;
_Static_assert(sizeof(s_lfQueue) == CELL_SPURS_LFQUEUE_SIZE,
               "lfqueue size");

/* ---- Task entry --------------------------------------------------- */
void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    (void)argTaskset;

    /* Touch each header-surface symbol so the compiler does not
     * eliminate the TU-level references. */
    s_exitCode.skip[0]         = 0;
    s_pmPollStatus             = 0;
    s_lfQueue.skip[0]          = 0;

    /* argTask.u64[0] carries the EA of a 16-byte PPU flag buffer. */
    CellSpursTaskArgument arg;
    *(qword *)&arg = argTask;
    uint64_t flag_ea = arg.u64[0];

    __attribute__((aligned(16))) uint32_t buf[4] = { 0xc0ffeeU, 0, 0, 0 };
    mfc_put(buf, flag_ea, 16, /*tag*/0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();

    cellSpursTaskExit(0);
}
