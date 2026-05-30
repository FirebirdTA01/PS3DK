#ifndef __PS3DK_SCE_HL_SPURS_TASKSET_H__
#define __PS3DK_SCE_HL_SPURS_TASKSET_H__

#include <stdint.h>
#include <string.h>
#include <cell/spurs.h>
#include <sce/hl/spurs/define.h>
#include <sce/hl/spurs/spurs.h>

#ifdef __cplusplus
__SCE_HL_SPURS_BEGIN

class Taskset : public cell::Spurs::Taskset2 {
public:
    static const uint32_t ALIGN = CELL_SPURS_TASKSET_ALIGN;

    Taskset() {}

    static int create(Taskset *taskset,
                      const char *name,
                      Spurs *spurs,
                      uint64_t tasksetArgument,
                      uint8_t priority = CELL_SPURS_MAX_PRIORITY - 1,
                      uint8_t spuMask = 0xff)
    {
        uint8_t priorities[CELL_SPURS_MAX_SPU];
        for (unsigned i = 0; i < CELL_SPURS_MAX_SPU; ++i) {
            priorities[i] = (spuMask & (1u << i)) ? priority : 0;
        }
        return create(taskset, name, spurs, tasksetArgument, priorities, CELL_SPURS_MAX_SPU);
    }

    static int create(Taskset *taskset,
                      const char *name,
                      Spurs *spurs,
                      uint64_t tasksetArgument,
                      const uint8_t priority[CELL_SPURS_MAX_SPU],
                      unsigned maxContention)
    {
        CellSpursTasksetAttribute2 attr;
        __builtin_memset(&attr, 0, sizeof(attr));
        cellSpursTasksetAttribute2Initialize(&attr);
        attr.name = name;
        attr.argTaskset = tasksetArgument;
        for (unsigned i = 0; i < CELL_SPURS_MAX_SPU; ++i) {
            attr.priority[i] = priority[i];
        }
        attr.maxContention = maxContention;
        attr.enableClearLs = 0;
        attr.taskNameBuffer = 0;
        return cell::Spurs::Taskset2::create(spurs, taskset, &attr);
    }

    int getInfo(CellSpursTasksetInfo *info)
    { return cellSpursGetTasksetInfo(this, info); }

    int shutdown()
    { return cellSpursShutdownTaskset(this); }

    int join()
    { return cellSpursJoinTaskset(this); }

    int getTasksetId(CellSpursWorkloadId *wid) const
    { return cellSpursGetTasksetId(this, wid); }

    int getSpursAddress(CellSpurs **ppSpurs) const
    { return cellSpursTasksetGetSpursAddress(this, ppSpurs); }

    int createTaskWithAttribute(CellSpursTaskId *tid,
                                const CellSpursTaskAttribute *attr)
    { return cellSpursCreateTaskWithAttribute(this, tid, attr); }

    int sendSignal(CellSpursTaskId id)
    { return cellSpursSendSignal(this, id); }

    int setExceptionEventHandler(CellSpursTasksetExceptionEventHandler handler,
                                 void *arg)
    { return cellSpursTasksetSetExceptionEventHandler(this, handler, arg); }

    int unsetExceptionEventHandler()
    { return cellSpursTasksetUnsetExceptionEventHandler(this); }
} __attribute__((aligned(CELL_SPURS_TASKSET_ALIGN)));

__SCE_HL_SPURS_END
#endif /* __cplusplus */

#endif /* __PS3DK_SCE_HL_SPURS_TASKSET_H__ */
