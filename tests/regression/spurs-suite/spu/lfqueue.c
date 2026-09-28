/* spurs-suite lock-free queue, SPU side.  argTask: u64[0] = result slot
 * array EA, u32[2] = queue block EA (see lfqueue.h), u32[3] = kind.  A kind
 * reports status 0 on success, else the failing call's rc in status, the
 * step number in value and a detail word in extra. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/lfqueue.h>
#include "../lfqueue.h"

#define TAG 2
#define ERR_AGAIN        ((int)0x80410901u)
#define ERR_INVAL        ((int)0x80410902u)
#define ERR_ALIGN        ((int)0x80410910u)
#define ERR_NULL_POINTER ((int)0x80410911u)

static result_slot out;
static lq_entry entry;

static void report(uint64_t slots, unsigned kind, int status, unsigned value, unsigned extra)
{
    out.magic = RESULT_MAGIC | kind;
    out.status = (unsigned)status;
    out.value = value;
    out.extra = extra;
    mfc_put(&out, slots + kind * sizeof(result_slot), sizeof out, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

static int push(uint64_t q, unsigned seq, unsigned block)
{
    CellSpursLFQueuePushContainer c;
    int rc;
    entry.seq = seq;
    entry.magic = LQ_SEQ_MAGIC | seq;
    entry.producer = cellSpursGetTaskId();
    cellSpursLFQueuePushContainerInitialize(&c, &entry, TAG);
    rc = _cellSpursLFQueuePushBeginBody(q, &c, block);
    return rc ? rc : cellSpursLFQueuePushEnd(q, &c);
}

static int pop(uint64_t q, unsigned block)
{
    CellSpursLFQueuePopContainer c;
    int rc;
    entry.seq = entry.magic = 0xdeadbeefu;
    cellSpursLFQueuePopContainerInitialize(&c, &entry, TAG);
    rc = _cellSpursLFQueuePopBeginBody(q, &c, block);
    return rc ? rc : cellSpursLFQueuePopEnd(q, &c);
}

static int check_entry(unsigned seq)
{
    return entry.seq == seq && entry.magic == (LQ_SEQ_MAGIC | seq);
}

/* fail with step `s` unless `cond`; rc and detail go to the report */
#define STEP(s, cond, rc_, detail) \
    do { if (!(cond)) { *step = (s); *detail_out = (detail); return (rc_) ? (rc_) : -1; } } while (0)

static int api(uint64_t base, unsigned *step, unsigned *detail_out)
{
    uint64_t q = LQ_QUEUE_EA(base, LQ_SPU_INIT), ring = LQ_RING_EA(base, LQ_SPU_INIT), ts = 0;
    unsigned n = 0;
    CellSpursLFQueueDirection dir = CELL_SPURS_LFQUEUE_ANY2ANY;
    int rc;

    rc = cellSpursLFQueueInitialize(q, ring, LQ_ENTRY, 2, CELL_SPURS_LFQUEUE_SPU2SPU);
    STEP(1, rc == 0, rc, 0);
    rc = cellSpursLFQueueSize(q, &n);
    STEP(2, rc == 0 && n == 0, rc, n);
    rc = cellSpursLFQueueDepth(q, &n);
    STEP(3, rc == 0 && n == 2, rc, n);
    rc = cellSpursLFQueueGetEntrySize(q, &n);
    STEP(4, rc == 0 && n == LQ_ENTRY, rc, n);
    rc = cellSpursLFQueueGetDirection(q, &dir);
    STEP(5, rc == 0 && dir == CELL_SPURS_LFQUEUE_SPU2SPU, rc, (unsigned)dir);
    rc = cellSpursLFQueueGetTasksetAddress(q, &ts);
    STEP(6, rc == 0 && ts == cellSpursGetTasksetAddress(), rc, (unsigned)ts);

    rc = pop(q, 0);
    STEP(7, rc == ERR_AGAIN, 0, (unsigned)rc);            /* empty */
    rc = push(q, 100, 0);
    STEP(8, rc == 0, rc, 0);
    rc = push(q, 101, 0);
    STEP(9, rc == 0, rc, 0);
    rc = push(q, 102, 0);
    STEP(10, rc == ERR_AGAIN, 0, (unsigned)rc);           /* full */
    rc = cellSpursLFQueueSize(q, &n);
    STEP(11, rc == 0 && n == 2, rc, n);
    rc = pop(q, 1);
    STEP(12, rc == 0 && check_entry(100), rc, entry.seq);
    rc = pop(q, 1);
    STEP(13, rc == 0 && check_entry(101), rc, entry.seq);
    rc = cellSpursLFQueueSize(q, &n);
    STEP(14, rc == 0 && n == 0, rc, n);

    /* wrap the indices past 2 * depth */
    for (unsigned i = 0; i < 5; ++i) {
        rc = push(q, 200 + i, 1);
        STEP(15, rc == 0, rc, i);
        rc = pop(q, 1);
        STEP(16, rc == 0 && check_entry(200 + i), rc, entry.seq);
    }

    rc = push(q, 7, 0);
    STEP(17, rc == 0, rc, 0);
    rc = cellSpursLFQueueClear(q);
    STEP(18, rc == 0, rc, 0);
    rc = cellSpursLFQueueSize(q, &n);
    STEP(19, rc == 0 && n == 0, rc, n);

    rc = cellSpursLFQueueInitialize(q + 0x40, ring, LQ_ENTRY, 2, CELL_SPURS_LFQUEUE_SPU2SPU);
    STEP(20, rc == ERR_ALIGN, 0, (unsigned)rc);
    rc = cellSpursLFQueueInitialize(0, ring, LQ_ENTRY, 2, CELL_SPURS_LFQUEUE_SPU2SPU);
    STEP(21, rc == ERR_NULL_POINTER, 0, (unsigned)rc);
    return 0;
}

static int produce(uint64_t q, unsigned *step, unsigned *detail_out)
{
    for (unsigned i = 0; i < LQ_ROUNDS; ++i) {
        int rc = push(q, i, 1);
        STEP(1, rc == 0, rc, i);
    }
    return 0;
}

static int consume(uint64_t q, unsigned *step, unsigned *detail_out)
{
    for (unsigned i = 0; i < LQ_ROUNDS; ++i) {
        int rc = pop(q, 1);
        STEP(1, rc == 0 && check_entry(i), rc, (i << 16) | (entry.seq & 0xffff));
    }
    return 0;
}

void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    CellSpursTaskArgument arg;
    uint64_t slots, base;
    unsigned kind, step = 0, detail = 0;
    int rc = -1;
    (void)argTaskset;
    *(qword *)&arg = argTask;
    slots = arg.u64[0];
    base = arg.u32[2];
    kind = arg.u32[3];

    switch (kind) {
    case LQ_API:              rc = api(base, &step, &detail); break;
    case LQ_SPU2SPU_PRODUCER: rc = produce(LQ_QUEUE_EA(base, LQ_SPU2SPU), &step, &detail); break;
    case LQ_SPU2SPU_CONSUMER: rc = consume(LQ_QUEUE_EA(base, LQ_SPU2SPU), &step, &detail); break;
    case LQ_SPU2PPU_PRODUCER: rc = produce(LQ_QUEUE_EA(base, LQ_SPU2PPU), &step, &detail); break;
    case LQ_PPU2SPU_CONSUMER: rc = consume(LQ_QUEUE_EA(base, LQ_PPU2SPU), &step, &detail); break;
    }
    report(slots, kind, rc, step, detail);
}
