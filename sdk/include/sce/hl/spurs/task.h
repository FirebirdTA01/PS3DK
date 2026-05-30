#ifndef __PS3DK_SCE_HL_SPURS_TASK_H__
#define __PS3DK_SCE_HL_SPURS_TASK_H__

#include <stdint.h>
#include <cell/spurs.h>
#include <sce/hl/spurs/define.h>
#include <sce/hl/spurs/task_elf.h>
#include <sce/hl/spurs/taskset.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int cellSpursTaskExitCodeInitialize(CellSpursTaskExitCode *exitCode);
extern int cellSpursTaskExitCodeGet(CellSpursTaskExitCode *exitCode, int *value);
extern int cellSpursTaskExitCodeTryGet(CellSpursTaskExitCode *exitCode, int *value);

#ifdef __cplusplus
}

__SCE_HL_SPURS_BEGIN

class Task {
    CellSpursTaskExitCode m_exitCode;
    Taskset *m_taskset;
    CellSpursTaskId m_id;
    const void *m_saveBuffer;

public:
    static const uint32_t ALIGN = CELL_SPURS_TASK_EXIT_CODE_ALIGN;

    Task() : m_taskset(0), m_id(0), m_saveBuffer(0) {}

    static int create(Task *task, Taskset *taskset,
                      const void *elf,
                      const CellSpursTaskArgument &argument,
                      const void *context)
    { return create(task, taskset, elf, &argument, context); }

    static int create(Task *task, Taskset *taskset,
                      const void *elf,
                      const CellSpursTaskArgument *argument,
                      const void *context)
    {
        CellSpursTaskSaveConfig config = {
            const_cast<void *>(context),
            CELL_SPURS_TASK_CONTEXT_SIZE_ALL,
            &gCellSpursTaskLsAll
        };
        return create(task, taskset, elf, argument, &config);
    }

    static int create(Task *task, Taskset *taskset,
                      const void *elf,
                      const CellSpursTaskArgument &argument)
    { return create(task, taskset, elf, &argument); }

    static int create(Task *task, Taskset *taskset,
                      const void *elf,
                      const CellSpursTaskArgument *argument)
    { return create(task, taskset, elf, argument,
                    static_cast<const CellSpursTaskSaveConfig *>(0)); }

    static int create(Task *task, Taskset *taskset,
                      const TaskElf &elf,
                      const CellSpursTaskArgument &argument,
                      const void *saveBuffer,
                      unsigned saveBufferSize)
    { return create(task, taskset, elf, &argument, saveBuffer, saveBufferSize); }

    static int create(Task *task, Taskset *taskset,
                      const TaskElf &elf,
                      const CellSpursTaskArgument *argument,
                      const void *saveBuffer,
                      unsigned saveBufferSize)
    {
        CellSpursTaskSaveConfig config = {
            const_cast<void *>(saveBuffer),
            saveBufferSize,
            elf.lsPattern()
        };
        return create(task, taskset, elf.elf(), argument, &config);
    }

    static int create(Task *task, Taskset *taskset,
                      const void *elf,
                      const CellSpursTaskArgument *argument,
                      const CellSpursTaskSaveConfig *config)
    {
        CellSpursTaskAttribute attr;
        __SCE_SPURS_UTIL_RETURN_IF(cellSpursTaskAttributeInitialize(
            &attr, elf, config, argument));
        __SCE_SPURS_UTIL_RETURN_IF(cellSpursTaskExitCodeInitialize(&task->m_exitCode));
        __SCE_SPURS_UTIL_RETURN_IF(cellSpursTaskAttributeSetExitCodeContainer(
            &attr, &task->m_exitCode));
        task->m_taskset = taskset;
        task->m_saveBuffer = config ? config->eaContext : 0;
        return taskset->createTaskWithAttribute(&task->m_id, &attr);
    }

    CellSpursTaskId id() const { return m_id; }
    void *saveBuffer() const { return const_cast<void *>(m_saveBuffer); }

    int join(int *value = 0)
    {
        int dummy = 0;
        return cellSpursTaskExitCodeGet(&m_exitCode, value ? value : &dummy);
    }

    int tryJoin(int *value = 0)
    {
        int dummy = 0;
        return cellSpursTaskExitCodeTryGet(&m_exitCode, value ? value : &dummy);
    }
} __attribute__((aligned(CELL_SPURS_TASK_EXIT_CODE_ALIGN)));

__SCE_HL_SPURS_END
#endif /* __cplusplus */

#endif /* __PS3DK_SCE_HL_SPURS_TASK_H__ */
