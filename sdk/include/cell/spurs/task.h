/* cell/spurs/task.h - SPURS task + taskset C API + C++ class surface.
 *
 * Clean-room header matching the reference libspurs_stub ABI.  The
 * underscore-prefixed entry points (_cellSpursTasksetAttributeInitialize,
 * _cellSpursTaskAttributeInitialize) take revision + sdkVersion
 * explicitly; the unprefixed forms are inline wrappers that supply
 * the current revision so samples don't drift when the SPRX bumps.
 *
 * The cell::Spurs::Taskset / Taskset2 class wrappers inherit from the
 * C-level opaque byte-array structs and expose inline methods that
 * forward to the C entry points.  Zero extra data members.
 */
#ifndef __PS3DK_CELL_SPURS_TASK_H__
#define __PS3DK_CELL_SPURS_TASK_H__

#include <stdint.h>
#include <cell/spurs/types.h>
#include <cell/spurs/task_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _CELL_SPURS_TASKSET_ATTRIBUTE_REVISION    0x01
#define _CELL_SPURS_TASK_ATTRIBUTE_REVISION       0x01

/* Optional save-config block referenced by TaskAttribute.  When null
 * the SPRX picks a default; otherwise it reads sizeContext + lsPattern
 * from here and DMAs the context to/from eaContext on suspend/resume. */
typedef struct CellSpursTaskSaveConfig {
    void                          *eaContext;
    uint32_t                       sizeContext;
    const CellSpursTaskLsPattern  *lsPattern;
} CellSpursTaskSaveConfig;

/* Convenience constant: "use the entire task area from TOP downward"
 * for lsPattern.  Defined inline so samples don't need to open-code it. */
static const CellSpursTaskLsPattern gCellSpursTaskLsAll = {
    { CELL_SPURS_TASK_TOP_MASK, 0xffffffffu, 0xffffffffu, 0xffffffffu }
};

/* -- Taskset attribute ---------------------------------------------- */
extern int _cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute *pAttr,
                                                unsigned int   revision,
                                                unsigned int   sdkVersion,
                                                uint64_t       argTaskset,
                                                const uint8_t  priority[8],
                                                unsigned int   maxContention);
extern int cellSpursTasksetAttributeSetName(CellSpursTasksetAttribute *attr,
                                            const char *name);
extern int cellSpursTasksetAttributeSetTasksetSize(CellSpursTasksetAttribute *attr,
                                                   size_t size);
extern int cellSpursTasksetAttributeEnableClearLS(CellSpursTasksetAttribute *pAttr,
                                                  int enable);

/* -- Taskset lifecycle ---------------------------------------------- */
extern int cellSpursCreateTasksetWithAttribute(CellSpurs *spurs,
                                               CellSpursTaskset *taskset,
                                               const CellSpursTasksetAttribute *attr);
extern int cellSpursCreateTaskset(CellSpurs *spurs,
                                  CellSpursTaskset *taskset,
                                  uint64_t argTaskset,
                                  const uint8_t priority[8],
                                  unsigned int maxContention);
extern int cellSpursShutdownTaskset(CellSpursTaskset *taskset);
extern int cellSpursJoinTaskset(CellSpursTaskset *taskset);
extern int cellSpursLookUpTasksetAddress(CellSpurs *spurs,
                                         CellSpursTaskset **taskset,
                                         CellSpursWorkloadId id);
extern int cellSpursGetTasksetId(const CellSpursTaskset *taskset,
                                 CellSpursWorkloadId *id);
extern int cellSpursTasksetGetSpursAddress(const CellSpursTaskset *taskset,
                                           CellSpurs **ppSpurs);

/* -- Taskset2 (extended class-1 taskset) ---------------------------- */
extern void _cellSpursTasksetAttribute2Initialize(CellSpursTasksetAttribute2 *pAttr,
                                                  uint32_t revision);
extern int cellSpursCreateTaskset2(CellSpurs *spurs,
                                   CellSpursTaskset2 *taskset,
                                   const CellSpursTasksetAttribute2 *pAttr);
extern int cellSpursDestroyTaskset2(CellSpursTaskset2 *taskset);

/* -- Task attribute -------------------------------------------------- */
extern int _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute *pAttr,
                                             unsigned int revision,
                                             unsigned int sdkVersion,
                                             const void *eaElf,
                                             const CellSpursTaskSaveConfig *saveConfig,
                                             const CellSpursTaskArgument *argument);
extern void _cellSpursTaskAttribute2Initialize(CellSpursTaskAttribute2 *pAttr,
                                               uint32_t revision);
extern int cellSpursTaskAttributeSetExitCodeContainer(CellSpursTaskAttribute *pAttr,
                                                      CellSpursTaskExitCode *exitCodeContainer);

/* -- Task lifecycle -------------------------------------------------- */
extern int cellSpursCreateTaskWithAttribute(CellSpursTaskset *taskset,
                                            CellSpursTaskId *id,
                                            const CellSpursTaskAttribute *attr);
extern int cellSpursCreateTask(CellSpursTaskset *taskset,
                               CellSpursTaskId *id,
                               const void *eaElf,
                               const void *eaContext,
                               uint32_t sizeContext,
                               const CellSpursTaskLsPattern *lsPattern,
                               const CellSpursTaskArgument *argument);
extern int _cellSpursSendSignal(CellSpursTaskset *taskset, CellSpursTaskId id);

extern int cellSpursCreateTask2(CellSpursTaskset2 *taskset,
                                CellSpursTaskId *id,
                                const void *eaElf,
                                const CellSpursTaskArgument *argument,
                                const CellSpursTaskAttribute2 *attr);
extern int cellSpursCreateTask2WithBinInfo(CellSpursTaskset2 *taskset,
                                           CellSpursTaskId *id,
                                           const CellSpursTaskBinInfo *binInfo,
                                           const CellSpursTaskArgument *argument,
                                           void *contextBuffer,
                                           const char *name,
                                           void *__reserved__);
extern int cellSpursJoinTask2(CellSpursTaskset2 *taskset,
                              CellSpursTaskId idTask,
                              int *exitCode);
extern int cellSpursTryJoinTask2(CellSpursTaskset2 *taskset,
                                 CellSpursTaskId idTask,
                                 int *exitCode);

/* -- Exception handling --------------------------------------------- */
extern int cellSpursTasksetSetExceptionEventHandler(CellSpursTaskset *taskset,
                                                    CellSpursTasksetExceptionEventHandler handler,
                                                    void *arg);
extern int cellSpursTasksetUnsetExceptionEventHandler(CellSpursTaskset *taskset);

/* -- Introspection --------------------------------------------------- */
extern int cellSpursGetTasksetInfo(const CellSpursTaskset *taskset,
                                   CellSpursTasksetInfo *info);
extern int cellSpursTaskGenerateLsPattern(CellSpursTaskLsPattern *pattern,
                                          uint32_t start, uint32_t size);
extern int cellSpursTaskGetReadOnlyAreaPattern(CellSpursTaskLsPattern *pattern,
                                               const void *elf);
extern int cellSpursTaskGetLoadableSegmentPattern(CellSpursTaskLsPattern *pattern,
                                                  const void *elf);
extern int cellSpursTaskGetContextSaveAreaSize(uint32_t *size,
                                               const CellSpursTaskLsPattern *lsPattern);

/* -- Inline wrappers over the revisioned _ entry points ------------- */
static inline int
cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute *pAttr,
                                    uint64_t argTaskset,
                                    const uint8_t priority[8],
                                    unsigned int maxContention)
{
    return _cellSpursTasksetAttributeInitialize(pAttr,
                                                _CELL_SPURS_TASKSET_ATTRIBUTE_REVISION,
                                                _CELL_SPURS_INTERNAL_VERSION,
                                                argTaskset, priority, maxContention);
}

static inline int
cellSpursTaskAttributeInitialize(CellSpursTaskAttribute *pAttr,
                                 const void *eaElf,
                                 const CellSpursTaskSaveConfig *saveConfig,
                                 const CellSpursTaskArgument *argument)
{
    return _cellSpursTaskAttributeInitialize(pAttr,
                                             _CELL_SPURS_TASK_ATTRIBUTE_REVISION,
                                             _CELL_SPURS_INTERNAL_VERSION,
                                             eaElf, saveConfig, argument);
}

static inline void
cellSpursTaskAttribute2Initialize(CellSpursTaskAttribute2 *pAttr)
{
    _cellSpursTaskAttribute2Initialize(pAttr, CELL_SPURS_TASK2_REVISION);
}

static inline void
cellSpursTasksetAttribute2Initialize(CellSpursTasksetAttribute2 *pAttr)
{
    _cellSpursTasksetAttribute2Initialize(pAttr, CELL_SPURS_TASK2_REVISION);
}

static inline int
cellSpursSendSignal(CellSpursTaskset *taskset, CellSpursTaskId id)
{
    return _cellSpursSendSignal(taskset, id);
}

#ifdef __cplusplus
}   /* extern "C" */

/* -- C++ class wrappers --------------------------------------------- */
namespace cell {
namespace Spurs {

class Taskset : public CellSpursTaskset {
public:
    static const uint32_t kSize  = CELL_SPURS_TASKSET_SIZE;
    static const uint32_t kAlign = CELL_SPURS_TASKSET_ALIGN;

    static int createWithAttribute(CellSpurs *spurs,
                                   CellSpursTaskset *taskset,
                                   const CellSpursTasksetAttribute *attr)
    { return cellSpursCreateTasksetWithAttribute(spurs, taskset, attr); }

    static int create(CellSpurs *spurs,
                      CellSpursTaskset *taskset,
                      uint64_t argTaskset,
                      const uint8_t priority[8],
                      unsigned int maxContention)
    { return cellSpursCreateTaskset(spurs, taskset, argTaskset, priority, maxContention); }

    int shutdown()
    { return cellSpursShutdownTaskset(this); }

    int join()
    { return cellSpursJoinTaskset(this); }

    static int lookUpTasksetAddress(CellSpurs *spurs,
                                    CellSpursTaskset **taskset,
                                    CellSpursWorkloadId id)
    { return cellSpursLookUpTasksetAddress(spurs, taskset, id); }

    int getTasksetId(CellSpursWorkloadId *wid) const
    { return cellSpursGetTasksetId(this, wid); }

    int getSpursAddress(CellSpurs **ppSpurs) const
    { return cellSpursTasksetGetSpursAddress(this, ppSpurs); }

    int createTaskWithAttribute(CellSpursTaskId *tid,
                                const CellSpursTaskAttribute *attr)
    { return cellSpursCreateTaskWithAttribute(this, tid, attr); }

    int createTask(CellSpursTaskId *tid,
                   const void *eaElf,
                   const void *eaContext,
                   uint32_t sizeContext,
                   const CellSpursTaskLsPattern *lsPattern,
                   const CellSpursTaskArgument *arg)
    { return cellSpursCreateTask(this, tid, eaElf, eaContext,
                                 sizeContext, lsPattern, arg); }

    int sendSignal(CellSpursTaskId id)
    { return cellSpursSendSignal(this, id); }

    int setExceptionEventHandler(CellSpursTasksetExceptionEventHandler handler,
                                 void *arg)
    { return cellSpursTasksetSetExceptionEventHandler(this, handler, arg); }

    int unsetExceptionEventHandler()
    { return cellSpursTasksetUnsetExceptionEventHandler(this); }

    int getTasksetInfo(CellSpursTasksetInfo *info) const
    { return cellSpursGetTasksetInfo(this, info); }
};

struct TasksetAttribute2 : public CellSpursTasksetAttribute2 {
    static void initialize(CellSpursTasksetAttribute2 *attr)
    { cellSpursTasksetAttribute2Initialize(attr); }
};

struct TaskAttribute2 : public CellSpursTaskAttribute2 {
    static void initialize(CellSpursTaskAttribute2 *attr)
    { cellSpursTaskAttribute2Initialize(attr); }
};

class Taskset2 : public CellSpursTaskset2 {
public:
    static const uint32_t kSize  = CELL_SPURS_TASKSET2_SIZE;
    static const uint32_t kAlign = CELL_SPURS_TASKSET2_ALIGN;

    static int create(CellSpurs *spurs,
                      CellSpursTaskset2 *taskset,
                      const CellSpursTasksetAttribute2 *attr)
    { return cellSpursCreateTaskset2(spurs, taskset, attr); }

    int destroy()
    { return cellSpursDestroyTaskset2(this); }

    int createTask2(CellSpursTaskId *id,
                    const void *eaElf,
                    CellSpursTaskArgument *argument,
                    const CellSpursTaskAttribute2 *attr)
    { return cellSpursCreateTask2(this, id, eaElf, argument, attr); }

    int createTask2(CellSpursTaskId *id,
                    const CellSpursTaskBinInfo *binInfo,
                    const CellSpursTaskArgument *argument,
                    void *contextBuffer,
                    const char *name,
                    void *__reserved__ = 0)
    { return cellSpursCreateTask2WithBinInfo(this, id, binInfo, argument,
                                             contextBuffer, name, __reserved__); }

    int createTask2WithBinInfo(CellSpursTaskId *id,
                               const CellSpursTaskBinInfo *binInfo,
                               const CellSpursTaskArgument *argument,
                               void *contextBuffer,
                               const char *name,
                               void *__reserved__ = 0)
    { return cellSpursCreateTask2WithBinInfo(this, id, binInfo, argument,
                                             contextBuffer, name, __reserved__); }

    int joinTask2(CellSpursTaskId id, int *exitCode)
    { return cellSpursJoinTask2(this, id, exitCode); }

    int tryJoinTask2(CellSpursTaskId id, int *exitCode)
    { return cellSpursTryJoinTask2(this, id, exitCode); }
};

}   /* namespace Spurs */
}   /* namespace cell */

#endif   /* __cplusplus */

#endif   /* __PS3DK_CELL_SPURS_TASK_H__ */
