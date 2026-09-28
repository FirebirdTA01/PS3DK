/* spurs-suite SPU job-queue runtime, SPU task: a job-queue semaphore.
 * argTask: u64[0] = result slot EA, u32[2] = semaphore EA, u32[3] = job
 * queue EA (jq_spu.h).  Then the job queue's descriptor pool through a
 * Port2: allocate it empty, give it back through PushAndRelease jobs,
 * allocate it again, and CopyPush while slow jobs hold every descriptor
 * (CopyPush waits in the pool's waiter ring for them).  While the pool
 * phase runs, extra carries a progress marker for a timeout report. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task.h>
#include <cell/spurs/job_queue_semaphore.h>
#include <cell/spurs/job_queue_port2.h>
#include "../jq_spu.h"

#define JOB_AGAIN 0x80410A01u
#define JOB_INVAL 0x80410A02u
#define JOB_ALIGN 0x80410A10u
#define JOB_NULL  0x80410A11u
#define JOB_PERM  0x80410A09u

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

    static jq_task_params prm __attribute__((aligned(128)));
    static uint8_t plain[128] __attribute__((aligned(128)));
    static uint8_t slow[128] __attribute__((aligned(128)));
    static uint64_t desc[Q_POOL + 1];
    uint64_t ea;
    unsigned i, j, round;
    mfc_get(&prm, arg.sem + 128, sizeof prm, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    mfc_get(plain, prm.plain, 128, 1, 0, 0);
    mfc_get(slow, prm.slow, 128, 1, 0, 0);
    mfc_read_tag_status_all();
    const uint64_t port = prm.port2;

    EXPECT(20, cellSpursJobQueuePort2Create(port, arg.jq), 0);
    EXPECT(21, cellSpursJobQueuePort2AllocateJobDescriptor(port, 256, 2, 4, &ea), JOB_NULL);  /* no 256-byte pool */
    EXPECT(22, cellSpursJobQueuePort2AllocateJobDescriptor(port, 96, 2, 4, &ea), JOB_INVAL);
    EXPECT(23, cellSpursJobQueuePort2AllocateJobDescriptor(port, 128, 2, 1, &ea), JOB_INVAL);
    EXPECT(24, cellSpursJobQueuePort2AllocateJobDescriptor(port, 128, 2, 4, 0), JOB_NULL);
    for (round = 0; round < 2; ++round) {
        const uint8_t *tmpl = round ? slow : plain;
        for (i = 0; i < Q_POOL; ++i) {
            EXPECT(30 + round, cellSpursJobQueuePort2AllocateJobDescriptor(port, 128, 2, 4, &desc[i]), 0);
            EXPECT(32 + round, (desc[i] & 0x7f) == 0 && desc[i] != 0, 1);
            for (j = 0; j < i; ++j)
                EXPECT(34 + round, desc[j] != desc[i], 1);
        }
        EXPECT(36 + round, cellSpursJobQueuePort2AllocateJobDescriptor(port, 128, 2, 4, &desc[Q_POOL]), JOB_AGAIN);
        for (i = 0; i < Q_POOL; ++i) {
            mfc_put((volatile void *)tmpl, desc[i], 128, 2, 0, 0);
            mfc_write_tag_mask(1u << 2);
            mfc_read_tag_status_all();
            EXPECT(38 + round, cellSpursJobQueuePort2PushAndReleaseJob(port, desc[i], 128, 0, 2, 1), 0);
            /* the command ring holds 16: flush half-way so a flush always
               fits (pushed jobs need not run before a flush) */
            if (i == Q_POOL / 2 - 1)
                EXPECT(44 + round, cellSpursJobQueuePort2PushFlush(port, 2, 0), 0);
        }
        report(slot, 0, 2, 0x100 + round * 0x10 + 1);       /* progress: round pushed */
        EXPECT(40 + round, cellSpursJobQueuePort2PushFlush(port, 2, 0), 0);
        if (round == 0)
            EXPECT(42, cellSpursJobQueuePort2Sync(port, 0), 0);    /* the pool is whole again */
        report(slot, 0, 2, 0x100 + round * 0x10 + 2);       /* progress: round done */
    }
    /* sixteen slow jobs hold the pool: each CopyPush waits for one */
    for (i = 0; i < Q_COPIES; ++i) {
        report(slot, 0, 2, 0x200 + i);                      /* progress: CopyPush i */
        EXPECT(50, cellSpursJobQueuePort2CopyPushJob(port, (const CellSpursJobHeader *)plain, 128, 128, 0, 2, 1), 0);
    }
    report(slot, 0, 2, 0x300);                              /* progress: copies pushed */
    EXPECT(51, cellSpursJobQueuePort2PushFlush(port, 2, 0), 0);
    EXPECT(52, cellSpursJobQueuePort2Sync(port, 0), 0);
    EXPECT(53, cellSpursJobQueuePort2Destroy(port), 0);
    report(slot, 0, 3, 0);
    return 0;
}
