/* spurs-suite SPU job-queue runtime, SPU task: a job-queue semaphore.
 * argTask: u64[0] = result slot EA, u32[2] = semaphore EA, u32[3] = job
 * queue EA (jq_spu.h). */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task.h>
#include <cell/spurs/job_queue_semaphore.h>
#include "../jq_spu.h"

#define JOB_AGAIN 0x80410A01u
#define JOB_INVAL 0x80410A02u
#define JOB_ALIGN 0x80410A10u
#define JOB_NULL  0x80410A11u

static result_slot out;

static void report(uint64_t slot, unsigned step, unsigned value, unsigned extra)
{
    out.magic = RESULT_MAGIC;
    out.status = step;
    out.value = value;
    out.extra = extra;
    mfc_put(&out, slot, sizeof out, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

#define EXPECT(step, got, want) do { \
        unsigned g_ = (unsigned)(got), w_ = (unsigned)(want); \
        if (g_ != w_) { report(slot, (step), 2, g_); return 0; } } while (0)

int cellSpursTaskMain(qword argTask, uint64_t argTaskset)
{
    struct { uint64_t slot; uint32_t sem, jq; } arg __attribute__((aligned(16)));
    uint64_t slot;
    (void)argTaskset;
    *(qword *)&arg = argTask;
    slot = arg.slot;

    EXPECT(1, cellSpursJobQueueSemaphoreInitialize(0, arg.jq), JOB_NULL);
    EXPECT(2, cellSpursJobQueueSemaphoreInitialize(arg.sem + 16, arg.jq), JOB_ALIGN);
    EXPECT(3, cellSpursJobQueueSemaphoreInitialize(arg.sem, arg.jq), 0);
    EXPECT(4, cellSpursJobQueueSemaphoreTryAcquire(arg.sem, 1), JOB_AGAIN);
    EXPECT(5, cellSpursJobQueueSemaphoreAcquire(arg.sem, 0x10000000u), JOB_INVAL);
    report(slot, 0, 1, 0);                  /* about to wait */
    EXPECT(6, cellSpursJobQueueSemaphoreAcquire(arg.sem, Q_ACQUIRE), 0);
    report(slot, 0, 2, 0);
    return 0;
}
