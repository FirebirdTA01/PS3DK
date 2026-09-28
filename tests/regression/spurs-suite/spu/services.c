/* spurs-suite SPU services, SPU task.  argTask: u64[0] = result slot array
 * EA, u32[2] = report block EA, u32[3] = kind (services.h). */
#include <stdint.h>
#include <stdlib.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task.h>
#include <cell/spurs/common.h>
#include <cell/spurs/control.h>
#include <cell/spurs/ready_count.h>
#include <cell/spurs/policy_module.h>
#include <cell/spurs/semaphore.h>
#include <cell/spurs/queue.h>
#include <cell/spurs/event_flag.h>
#include <cell/spurs/trace.h>
#include <cell/spurs/workload.h>
#include "../services.h"

CELL_SPU_LS_PARAM(16 * 1024, 16 * 1024);

#define CORE_INVAL   0x80410702u
#define CORE_ALIGN   0x80410710u
#define CORE_NULL    0x80410711u
#define PM_BUSY      0x8041080Au
#define TASK_NULL    0x80410911u

/* the function forms (the headers spell these as macros) */
extern int (cellSpursQueueTryPopBegin)(uint64_t ea, void *buffer, unsigned int tag);
extern int (cellSpursEventFlagTryWait)(uint64_t ea, uint16_t *bits, unsigned mode);

extern char _end[];

static result_slot out;
static s_report report_block;
static CellSpursTracePacket packet;
static qword guid_words __attribute__((aligned(128)));

static void report(uint64_t slots, unsigned step, unsigned got, unsigned want)
{
    out.magic = RESULT_MAGIC | S_TASK;
    out.status = step;
    out.value = got;
    out.extra = want;
    mfc_put(&out, slots, sizeof out, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

static void put_report(uint64_t ea)
{
    mfc_put(&report_block, ea, sizeof report_block, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

#define EXPECT(step, got, want) do { \
        unsigned g_ = (unsigned)(got), w_ = (unsigned)(want); \
        if (g_ != w_) { report(slots, (step), g_, w_); return; } } while (0)

static void run(uint64_t slots, uint64_t eaReport)
{
    const unsigned wid = cellSpursGetWorkloadId();
    const uint64_t ts = cellSpursGetTasksetAddress();
    void *area = 0;
    uint32_t size = 0, words[4];
    uint64_t guid = 0;
    unsigned old, i;
    uint16_t bits = 0;

    mfc_get(&report_block, eaReport, sizeof report_block, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
    report_block.wid = wid;

    /* the "2" task services */
    EXPECT(1, cellSpursYield2(), 0);
    (void)cellSpursTaskPoll2();
    report_block.waiting = 1;
    put_report(eaReport);
    EXPECT(2, cellSpursWaitSignal2(), 0);       /* the PPU signals us */
    report_block.waiting = 0;

    /* workload flag receiver: one receiver at a time */
    EXPECT(3, _cellSpursWorkloadFlagReceiver2(wid, 1), 0);
    EXPECT(4, _cellSpursWorkloadFlagReceiver2(wid, 1), PM_BUSY);
    EXPECT(5, _cellSpursWorkloadFlagReceiver2(wid, 0), 0);
    EXPECT(6, _cellSpursWorkloadFlagReceiver2(wid, 0), PM_BUSY);

    /* volatile area: between heap end and stack bottom */
    EXPECT(7, cellSpursGetTaskVolatileArea(&area, &size), 0);
    EXPECT(8, (uintptr_t)area, S_HEAP + (uintptr_t)_end);
    EXPECT(9, size, 0x40000 - S_STACK - (S_HEAP + (uintptr_t)_end));

    /* SPU GUID: four ila $2 words carrying 16 bits each */
    for (i = 0; i < 4; ++i)
        words[i] = 0x42000002u | (i << 7) | ((0x1234u * (i + 1)) << 9);
    guid_words = (qword)(vec_uint4){ words[0], words[1], words[2], words[3] };
    EXPECT(10, cellSpursGetSpuGuid(&guid_words, &guid), 0);
    EXPECT(11, (uint32_t)(guid >> 32), (0x1234u << 16) | 0x2468u);
    EXPECT(12, (uint32_t)guid, (0x369cu << 16) | 0x48d0u);
    guid_words = (qword)(vec_uint4){ words[0], 0, words[2], words[3] };
    EXPECT(13, cellSpursGetSpuGuid(&guid_words, &guid), CORE_INVAL);
    EXPECT(14, cellSpursGetSpuGuid(0, &guid), CORE_NULL);

    /* ready count of our own workload, restored as it was */
    old = _cellSpursReadyCountCompareAndSwap(wid, 256, 0);     /* never matches: a read */
    EXPECT(15, _cellSpursReadyCountAdd(wid, 1), old);
    EXPECT(16, _cellSpursReadyCountAdd(wid, -1), old + 1 > 255 ? 255 : old + 1);
    EXPECT(17, _cellSpursReadyCountSwap(wid, 255), old);
    EXPECT(18, _cellSpursReadyCountAdd(wid, 10), 255);            /* clamps at 255 */
    EXPECT(19, _cellSpursReadyCountOperator(wid, _CELL_SPURS_READY_COUNT_CAS, 255, old), 255);
    EXPECT(20, _cellSpursReadyCountCompareAndSwap(wid, 256, 0), old);

    /* workload data of a taskset workload is the taskset */
    EXPECT(21, (uint32_t)_cellSpursGetWorkloadData(wid), (uint32_t)ts);

    /* contention and priorities; the PPU checks the instance afterwards */
    EXPECT(22, cellSpursSetMaxContention(wid, S_CONTENTION), 0);
    EXPECT(23, cellSpursSetMaxContention(40, 1), CORE_INVAL);
    EXPECT(24, cellSpursSetPriority(wid, 0, 16), CORE_INVAL);
    EXPECT(25, cellSpursSetPriority(wid, 9, 1), CORE_INVAL);
    EXPECT(26, cellSpursSetPriorities(wid, (vec_uchar16){ 1, 1, 1, 1, 1, 1, 1, 16 }), CORE_INVAL);
    EXPECT(27, cellSpursSetPriorities(wid, spu_splats((unsigned char)1)), 0);
    EXPECT(28, cellSpursSetPriority(wid, 0, S_PRIORITY), 0);
    report_block.idleSpuRc = (unsigned)_cellSpursRequestIdleSpu(wid, 0);

    /* semaphore from the SPU side */
    EXPECT(29, _cellSpursSemaphoreInitialize(report_block.semaphore, 2, 0), 0);
    {
        uint64_t owner = 0;
        EXPECT(30, cellSpursSemaphoreGetTasksetAddress(report_block.semaphore, &owner), 0);
        EXPECT(31, (uint32_t)owner, (uint32_t)ts);
        EXPECT(32, cellSpursSemaphoreGetTasksetAddress(0, &owner), TASK_NULL);
    }
    EXPECT(33, cellSpursSemaphoreP(report_block.semaphore), 0);
    EXPECT(34, cellSpursSemaphoreV(report_block.semaphore), 0);

    /* the function forms reach the same entry points */
    EXPECT(35, (cellSpursQueueTryPopBegin)(0, 0, 0), cellSpursQueueTryPopBegin(0, 0, 0));
    EXPECT(36, (cellSpursEventFlagTryWait)(0, &bits, 0), cellSpursEventFlagTryWait(0, &bits, 0));

    /* a user trace packet; the PPU looks for it in the trace buffer */
    packet.header.tag = CELL_SPURS_TRACE_TAG_USER;
    packet.data.user = S_TRACE_MAGIC;
    cellSpursPutTrace(&packet, 2);
    mfc_write_tag_mask(1u << 2);
    mfc_read_tag_status_all();

    put_report(eaReport);
    report(slots, 0, 0, 0);
}

int cellSpursTaskMain(qword argTask, uint64_t argTaskset)
{
    struct { uint64_t slots; uint32_t report, kind; } arg __attribute__((aligned(16)));
    (void)argTaskset;
    *(qword *)&arg = argTask;
    run(arg.slots, arg.report);
    return 0;
}
