/* spurs-suite job extras, SPU task.  argTask: u64[0] = result slot EA,
 * u32[2] = parameter block EA (job_extras.h). */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task.h>
#include <cell/spurs/job_chain.h>
#include <cell/spurs/job_commands.h>
#include <cell/spurs/job_descriptor.h>
#include "../job_extras.h"

#define JOB_INVAL 0x80410A02u
#define JOB_BUSY  0x80410A0Au
#define JOB_ALIGN 0x80410A10u
#define JOB_NULL  0x80410A11u

static result_slot out;
static x_params params;
static uint8_t header[0x30] __attribute__((aligned(128)));

static void report(uint64_t slot, unsigned step, unsigned got, unsigned want)
{
    out.magic = RESULT_MAGIC;
    out.status = step;
    out.value = got;
    out.extra = want;
    mfc_put(&out, slot, sizeof out, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

#define EXPECT(step, got, want) do { \
        unsigned g_ = (unsigned)(got), w_ = (unsigned)(want); \
        if (g_ != w_) { report(slot, (step), g_, w_); return; } } while (0)

static void run(uint64_t slot, uint64_t eaParams)
{
    unsigned i;
    mfc_get(&params, eaParams, sizeof params, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();

    /* grab limit */
    EXPECT(1, cellSpursJobSetMaxGrab(params.chain, 0), JOB_INVAL);
    EXPECT(2, cellSpursJobSetMaxGrab(params.chain, 17), JOB_INVAL);
    EXPECT(3, cellSpursJobSetMaxGrab(0, 4), JOB_NULL);
    EXPECT(4, cellSpursJobSetMaxGrab(params.chain + 8, 4), JOB_ALIGN);
    EXPECT(5, cellSpursJobSetMaxGrab(params.chain, X_MAX_GRAB), 0);

    /* urgent commands: a job, a call, two more jobs; a fifth finds no slot */
    EXPECT(6, cellSpursAddUrgentCommand(params.chain, CELL_SPURS_JOB_COMMAND_JOB(params.jobs[0])), 0);
    EXPECT(7, cellSpursAddUrgentCall(params.chain, params.callList), 0);
    EXPECT(8, cellSpursAddUrgentCall(params.chain, params.callList + 4), JOB_ALIGN);
    EXPECT(9, cellSpursAddUrgentCall(params.chain, 0), JOB_NULL);
    for (i = 2; i < X_URGENT_JOBS; ++i)
        EXPECT(10 + i, cellSpursAddUrgentCommand(params.chain, CELL_SPURS_JOB_COMMAND_JOB(params.jobs[i])), 0);
    EXPECT(14, cellSpursAddUrgentCommand(params.chain, CELL_SPURS_JOB_COMMAND_JOB(params.jobs[0])), JOB_BUSY);

    /* SetJobbin2Param: the PPU compares the header with its own */
    for (i = 0; i < sizeof header; ++i)
        header[i] = 0;
    EXPECT(15, cellSpursJobHeaderSetJobbin2Param((CellSpursJobHeader *)header, 0), JOB_NULL);
    EXPECT(16, cellSpursJobHeaderSetJobbin2Param((CellSpursJobHeader *)header, params.jobbin2 + 4), JOB_ALIGN);
    EXPECT(17, cellSpursJobHeaderSetJobbin2Param((CellSpursJobHeader *)header, eaParams), JOB_INVAL);
    EXPECT(18, cellSpursJobHeaderSetJobbin2Param((CellSpursJobHeader *)header, params.jobbin2), 0);
    mfc_put(header, params.header, sizeof header, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();

    report(slot, 0, 0, 0);
}

int cellSpursTaskMain(qword argTask, uint64_t argTaskset)
{
    struct { uint64_t slot; uint32_t params, kind; } arg __attribute__((aligned(16)));
    (void)argTaskset;
    *(qword *)&arg = argTask;
    run(arg.slot, arg.params);
    return 0;
}
