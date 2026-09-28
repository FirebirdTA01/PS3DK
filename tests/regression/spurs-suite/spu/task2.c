/* spurs-suite Task2, SPU task.  argTask: u64[0] = result slot array EA,
 * u32[2] = parameter block EA (parent) or exit code (child), u32[3] = kind
 * (task2.h).  The parent runs on a Taskset2 with a context save area, so
 * it may block in the joins. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task.h>
#include <cell/spurs/task_exit_code.h>
#include "../task2.h"

#define TASK_AGAIN    ((int)0x80410901u)
#define TASK_INVAL    ((int)0x80410902u)
#define TASK_NOEXEC   ((int)0x80410907u)
#define TASK_PERM     ((int)0x80410909u)
#define TASK_STAT     ((int)0x8041090Fu)
#define TASK_NULL     ((int)0x80410911u)

static result_slot out;
static t2_params params;
static CellSpursTaskAttribute2 attr2;
static CellSpursTaskAttribute attr;

static void report(uint64_t slots, unsigned step, unsigned got, unsigned want)
{
    out.magic = RESULT_MAGIC | T2_PARENT;
    out.status = step;
    out.value = got;
    out.extra = want;
    mfc_put(&out, slots, sizeof out, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

static qword child_arg(uint64_t slots, unsigned code)
{
    struct { uint64_t slots; uint32_t code, kind; } a __attribute__((aligned(16)));
    a.slots = slots;
    a.code = code;
    a.kind = T2_CHILD;
    return *(qword *)&a;
}

#define EXPECT(step, got, want) do { \
        unsigned g_ = (unsigned)(got), w_ = (unsigned)(want); \
        if (g_ != w_) { report(slots, (step), g_, w_); return 1; } } while (0)

static int parent(uint64_t slots, uint64_t eaParams)
{
    const uint64_t ts = cellSpursGetTasksetAddress();
    CellSpursTaskId id, id2;
    int code = 0, rc;
    unsigned spins;

    mfc_get(&params, eaParams, sizeof params, 1, 0, 0);
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();

    /* create without an attribute, join */
    EXPECT(1, cellSpursCreateTask2(ts, &id, params.elf, child_arg(slots, 0x1111), 0), 0);
    EXPECT(2, cellSpursJoinTask2(ts, id, &code), 0);
    EXPECT(3, code, 0x1111);
    /* its id is released: joining it again finds nothing to join */
    EXPECT(4, cellSpursTryJoinTask2(ts, id, &code), TASK_STAT);

    /* create with a context and a name, try-join until it exits */
    cellSpursTaskAttribute2Initialize(&attr2);
    attr2.sizeContext = CELL_SPURS_TASK_CONTEXT_SIZE_ALL;
    attr2.eaContext = params.context;
    attr2.lsPattern.u32[0] = CELL_SPURS_TASK_TOP_MASK;
    attr2.lsPattern.u32[1] = attr2.lsPattern.u32[2] = attr2.lsPattern.u32[3] = 0xffffffffu;
    attr2.name = "suite-t2-child";
    EXPECT(5, cellSpursCreateTask2(ts, &id2, params.elf, child_arg(slots, 0x2222), &attr2), 0);
    for (spins = 0; (rc = cellSpursTryJoinTask2(ts, id2, &code)) == TASK_AGAIN && spins < 100000; ++spins)
        cellSpursYield();
    EXPECT(6, rc, 0);
    EXPECT(7, code, 0x2222);

    /* create from a binary info block */
    EXPECT(8, cellSpursCreateTask2WithBinInfo(ts, &id, params.binInfo, child_arg(slots, 0x3333), 0,
                                              "suite-t2-bininfo", 0), 0);
    EXPECT(9, cellSpursJoinTask2(ts, id, &code), 0);
    EXPECT(10, code, 0x3333);

    /* argument errors */
    EXPECT(11, cellSpursCreateTask2(params.plainTaskset, &id, params.elf, child_arg(slots, 0), 0), TASK_PERM);
    EXPECT(12, cellSpursJoinTask2(ts, 200, &code), TASK_INVAL);
    EXPECT(13, cellSpursJoinTask2(ts, id, 0), TASK_NULL);
    EXPECT(14, cellSpursCreateTask2(ts, &id, eaParams, child_arg(slots, 0), 0), TASK_NOEXEC);
    EXPECT(15, cellSpursCreateTask2WithBinInfo(ts, &id, params.binInfo, child_arg(slots, 0), 0, 0, &code),
           TASK_INVAL);

    /* an exit-code container */
    EXPECT(16, cellSpursTaskExitCodeInitialize(params.exitCode), 0);
    EXPECT(17, cellSpursTaskExitCodeTryGet(params.exitCode, &code), TASK_STAT);   /* nothing attached */
    EXPECT(18, cellSpursTaskAttributeInitialize(&attr, params.elf, 0, child_arg(slots, 0x4444)), 0);
    EXPECT(19, cellSpursTaskAttributeSetExitCodeContainer(&attr, params.exitCode), 0);
    EXPECT(20, cellSpursCreateTaskWithAttribute(params.plainTaskset, &id, &attr), TASK_PERM);
    EXPECT(21, cellSpursCreateTaskWithAttribute(ts, &id, &attr), 0);
    EXPECT(22, cellSpursTaskExitCodeGet(params.exitCode, &code), 0);
    EXPECT(23, code, 0x4444);
    EXPECT(24, cellSpursTaskExitCodeTryGet(params.exitCode, &code), TASK_STAT);   /* consumed */

    report(slots, 0, 0, 0);
    return T2_PARENT_CODE;
}

int cellSpursTaskMain(qword argTask, uint64_t argTaskset)
{
    struct { uint64_t slots; uint32_t a2, kind; } arg __attribute__((aligned(16)));
    (void)argTaskset;
    *(qword *)&arg = argTask;
    if (arg.kind == T2_CHILD)
        return (int)arg.a2;
    return parent(arg.slots, arg.a2);
}
