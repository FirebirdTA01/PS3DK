/* spurs-suite SPU-side control, SPU task.  argTask: u64[0] = result slot
 * array EA, u32[2] = the object's EA, u32[3] = kind (see control.h). */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/spu_task.h>
#include <cell/spurs/task_types.h>
#include <cell/spurs/task.h>
#include <cell/spurs/job_chain.h>
#include "../control.h"

#define TASK_ALIGN ((int)0x80410910u)
#define TASK_NULL  ((int)0x80410911u)
#define JOB_PERM   ((int)0x80410A09u)
#define JOB_ALIGN  ((int)0x80410A10u)
#define JOB_NULL   ((int)0x80410A11u)

extern int cellSpursShutdownTaskset(uint64_t eaTaskset);

static result_slot out;

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

/* argument errors; the object is a job chain of the newer revision */
static int errors(uint64_t chain, unsigned *step, unsigned *got)
{
    const struct { int rc, want; } cases[] = {
        { cellSpursShutdownTaskset(0), TASK_NULL },
        { cellSpursShutdownTaskset(chain | 0x40), TASK_ALIGN },
        { cellSpursRunJobChain(0), JOB_NULL },
        { cellSpursShutdownJobChain(chain | 0x40), JOB_ALIGN },
        { cellSpursKickJobChain(chain, 1), JOB_PERM },       /* newer chains are run */
        { cellSpursJobGuardNotify(0), JOB_NULL },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        if (cases[i].rc != cases[i].want) {
            *step = i + 1;
            *got = (unsigned)cases[i].rc;
            return -1;
        }
    return 0;
}

/* create a child task of this taskset (kind `child`, context area `ctx`),
 * directly or through a task attribute; value = the child's id */
static int create_child(uint64_t eaParams, uint64_t taskset, int useAttr, unsigned *id)
{
    static ctl_params p;
    static CellSpursTaskAttribute attr;
    CellSpursTaskArgument child;
    CellSpursTaskSaveConfig save;
    CellSpursTaskId tid = 0xff;
    unsigned kind = useAttr ? C_CHILD2 : C_CHILD;
    int rc;
    mfc_get(&p, eaParams, sizeof p, 2, 0, 0);
    mfc_write_tag_mask(1u << 2);
    mfc_read_tag_status_all();
    child.u64[0] = p.slots;
    child.u32[2] = 0;
    child.u32[3] = kind;
    if (useAttr) {
        save.eaContext = p.context[1];
        save.sizeContext = CELL_SPURS_TASK_CONTEXT_SIZE_ALL;
        save.lsPattern = CELL_SPURS_TASK_LS_ALL;
        rc = cellSpursTaskAttributeInitialize(&attr, p.elf, &save, *(qword *)&child);
        if (!rc)
            rc = cellSpursCreateTaskWithAttribute(taskset, &tid, &attr);
    } else {
        rc = cellSpursCreateTask(taskset, &tid, p.elf, p.context[0], CELL_SPURS_TASK_CONTEXT_SIZE_ALL,
                                 CELL_SPURS_TASK_LS_ALL, *(qword *)&child);
    }
    *id = tid;
    return rc;
}

void cellSpursMain(qword argTask, uint64_t argTaskset)
{
    CellSpursTaskArgument arg;
    uint64_t slots, object;
    unsigned kind, step = 0, got = 0;
    int rc = -1;
    (void)argTaskset;              /* the taskset's argument, not its address */
    *(qword *)&arg = argTask;
    slots = arg.u64[0];
    object = arg.u32[2];
    kind = arg.u32[3];

    switch (kind) {
    case C_RUN_CHAIN:      rc = cellSpursRunJobChain(object); break;
    case C_SHUTDOWN_CHAIN: rc = cellSpursShutdownJobChain(object); break;
    case C_GUARD_NOTIFY:   rc = cellSpursJobGuardNotify(object); break;
    case C_ERRORS:         rc = errors(object, &step, &got); break;
    case C_SHUTDOWN_OWN:   rc = cellSpursShutdownTaskset(cellSpursGetTasksetAddress()); break;
    case C_CREATE:         rc = create_child(object, cellSpursGetTasksetAddress(), 0, &step); break;
    case C_CREATE_ATTR:    rc = create_child(object, cellSpursGetTasksetAddress(), 1, &step); break;
    case C_CHILD:
    case C_CHILD2:         rc = 0; step = cellSpursGetTaskId(); break;
    }
    report(slots, kind, rc, step, got);
}
