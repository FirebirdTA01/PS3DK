/* cell/spurs/task_types.h - SPURS task + taskset opaque containers.
 *
 * Clean-room header.  A taskset is a scheduling container on top of a
 * workload; each task is one SPU ELF plus a context save area.  The
 * SPRX owns the live state; PPU-side visible storage is just byte
 * arrays of the sizes the SPRX expects.
 */
#ifndef __PS3DK_CELL_SPURS_TASK_TYPES_H__
#define __PS3DK_CELL_SPURS_TASK_TYPES_H__

#include <stddef.h>
#include <stdint.h>

/* ATTRIBUTE_PRXPTR comes from <ppu-types.h> on PPU so cross-SPRX
 * pointer fields shrink from our LP64 8-byte storage to the 4-byte
 * EA the SPRX expects.  On SPU (natively 32-bit pointers) it is a
 * no-op - the storage width already matches. */
#ifdef __PPU__
#include <ppu-types.h>
#include <cell/spurs/types.h>
#else
#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif
/* SPU side doesn't need the full types.h (no CellSpursAttribute, etc) -
 * just the handful of typedefs sub-structures here reference. */
typedef unsigned int CellSpursWorkloadId;
typedef unsigned int CellSpursTaskId;
struct CellSpurs;
typedef struct CellSpurs CellSpurs;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decl - CellSpursExceptionInfo lives in
 * cell/spurs/exception_common_types.h which we haven't ported yet.
 * Callbacks that take it by pointer only need the tag name. */
struct CellSpursExceptionInfo;
typedef struct CellSpursExceptionInfo CellSpursExceptionInfo;

#define CELL_SPURS_MAX_TASK                       128
#define CELL_SPURS_TASK_TOP                       0x3000
#define CELL_SPURS_TASK_BOTTOM                    0x40000
#define CELL_SPURS_CONTEXT_SIZE2BITS(size)        ((size) >> 11)
#define CELL_SPURS_TASK_TOP_BIT                   CELL_SPURS_CONTEXT_SIZE2BITS(CELL_SPURS_TASK_TOP)
#define CELL_SPURS_TASK_TOP_MASK                  (0xffffffffu >> CELL_SPURS_TASK_TOP_BIT)

#define CELL_SPURS_TASK_CONTEXT_ALIGN             128

#define CELL_SPURS_TASK_EXECUTION_CONTEXT_SIZE    1024
#define CELL_SPURS_TASK_MEMORY_CONTEXT_SIZE(size) (((size) + 2047) & ~2047)
#define CELL_SPURS_TASK_CONTEXT_SIZE(size)        (CELL_SPURS_TASK_EXECUTION_CONTEXT_SIZE + \
                                                   CELL_SPURS_TASK_MEMORY_CONTEXT_SIZE(size))
#define CELL_SPURS_TASK_CONTEXT_SIZE_ALL          (CELL_SPURS_TASK_CONTEXT_SIZE(CELL_SPURS_TASK_BOTTOM - CELL_SPURS_TASK_TOP))

#define CELL_SPURS_TASK_CEIL_2048(arg)            (((arg) + 2047) & ~2047)
#define CELL_SPURS_TASK_FLOOR_2048(arg)           ((arg) & ~2047)

/* Task argument: 16 bytes, interpreted as 4x u32 or 2x u64 by callers. */
typedef union CellSpursTaskArgument {
    uint32_t u32[4];
    uint64_t u64[2];
} CellSpursTaskArgument;

/* LS-pattern bitmap: marks which 2KB slots of the SPU LS the task's
 * context-save area spans.  4x u32 bit fields covering 0..256KB in
 * 2KB quantums. */
typedef union CellSpursTaskLsPattern {
    uint32_t u32[4];
    uint64_t u64[2];
} CellSpursTaskLsPattern;

/* Task attribute: 256 bytes, 16-byte aligned.  Opaque to callers; the
 * SPRX reads fields at fixed offsets after Initialize.  AttributeSet*
 * helpers poke them. */
#define CELL_SPURS_TASK_ATTRIBUTE_ALIGN           16
#define CELL_SPURS_TASK_ATTRIBUTE_SIZE            256

typedef struct CellSpursTaskAttribute {
    unsigned char skip[CELL_SPURS_TASK_ATTRIBUTE_SIZE];
} __attribute__((aligned(CELL_SPURS_TASK_ATTRIBUTE_ALIGN))) CellSpursTaskAttribute;

#define CELL_SPURS_TASK_ATTRIBUTE2_ALIGN          CELL_SPURS_TASK_ATTRIBUTE_ALIGN
#define CELL_SPURS_TASK_ATTRIBUTE2_SIZE           CELL_SPURS_TASK_ATTRIBUTE_SIZE

#define CELL_SPURS_TASK2_REVISION                 0

/* TaskAttribute2: public struct (not opaque), fields are poked by
 * inline initializers.  Pointer fields use ATTRIBUTE_PRXPTR so they
 * occupy 4 bytes at the offsets the SPRX (compiled against the PPU32
 * reference toolchain) expects, regardless of our LP64 host ABI. */
typedef struct CellSpursTaskAttribute2 {
    uint32_t                revision;
    uint32_t                sizeContext;
    uint64_t                eaContext;
    CellSpursTaskLsPattern  lsPattern;
    const char             *name ATTRIBUTE_PRXPTR;
    uint8_t                 __reserved__[CELL_SPURS_TASK_ATTRIBUTE2_SIZE
                                         - sizeof(uint32_t) * 2
                                         - sizeof(uint64_t)
                                         - sizeof(CellSpursTaskLsPattern)
                                         - 4 /* PRXPTR name */];
} __attribute__((aligned(CELL_SPURS_TASK_ATTRIBUTE2_ALIGN))) CellSpursTaskAttribute2;

/* Taskset exception-event callback. */
struct CellSpursTaskset;
typedef void (*CellSpursTasksetExceptionEventHandler)(CellSpurs *,
                                                      struct CellSpursTaskset *,
                                                      CellSpursTaskId,
                                                      const CellSpursExceptionInfo *,
                                                      void *);

/* Per-task introspection record returned by GetTasksetInfo. */
typedef enum CellSpursTaskState {
    CELL_SPURS_TASK_STATE_NO_EXISTENT = 0,
    CELL_SPURS_TASK_STATE_RUNNING     = 1,
    CELL_SPURS_TASK_STATE_WAITING     = 2,
    CELL_SPURS_TASK_STATE_READY       = 3
} CellSpursTaskState;

typedef struct CellSpursTaskExitCode {
    unsigned char skip[128];
} __attribute__((aligned(128))) CellSpursTaskExitCode;

#define CELL_SPURS_TASK_EXIT_CODE_ALIGN 128
#define CELL_SPURS_TASK_EXIT_CODE_SIZE  128

typedef struct CellSpursTaskInfo {
    CellSpursTaskLsPattern       lsPattern;
    CellSpursTaskArgument        argument;
    const void                  *eaElf           ATTRIBUTE_PRXPTR;
    const void                  *eaContext       ATTRIBUTE_PRXPTR;
    uint32_t                     sizeContext;
    uint8_t                      state;
    uint8_t                      hasSignal;
    uint8_t                      padding[2];
    const CellSpursTaskExitCode *eaTaskExitCode  ATTRIBUTE_PRXPTR;
    uint8_t                      guid[8];
    uint8_t                      reserved[72
                                          - sizeof(CellSpursTaskLsPattern)
                                          - sizeof(CellSpursTaskArgument)
                                          - 4 * 3 /* PRXPTR eaElf + eaContext + eaTaskExitCode */
                                          - sizeof(uint32_t)
                                          - sizeof(uint8_t) * 4
                                          - sizeof(uint8_t) * 8];
} CellSpursTaskInfo;

typedef struct CellSpursTasksetInfo {
    CellSpursTaskInfo                     taskInfo[CELL_SPURS_MAX_TASK];
    uint64_t                              argument;
    CellSpursWorkloadId                   idWorkload;
    CellSpursTaskId                       idLastScheduledTask;
    const char                           *name                          ATTRIBUTE_PRXPTR;
    CellSpursTasksetExceptionEventHandler exceptionEventHandler         ATTRIBUTE_PRXPTR;
    void                                 *exceptionEventHandlerArgument ATTRIBUTE_PRXPTR;
    uint32_t                              sizeTaskset;   /* Sony uses size_t which is 32-bit on PPU32; pinned. */
    uint8_t                               reserved[9360
                                                   - sizeof(CellSpursTaskInfo) * CELL_SPURS_MAX_TASK
                                                   - sizeof(uint64_t)
                                                   - sizeof(CellSpursWorkloadId)
                                                   - sizeof(CellSpursTaskId)
                                                   - 4 * 3 /* PRXPTR name + handler + handlerArg */
                                                   - sizeof(uint32_t) /* sizeTaskset */];
} CellSpursTasksetInfo;

/* Taskset storage: class-0 is the v1 layout, class-1 adds extended
 * state for Taskset2 (task2 barriers, joinable tasks, etc). */
#define CELL_SPURS_TASKSET_CLASS0_SIZE            (128 * 50)
#define CELL_SPURS_TASKSET_SIZE                   CELL_SPURS_TASKSET_CLASS0_SIZE
#define CELL_SPURS_TASKSET_ALIGN                  128
#define _CELL_SPURS_TASKSET_CLASS1_EXTENDED_SIZE  (128 + 128 * 16 + 128 * 15)
#define CELL_SPURS_TASKSET_CLASS1_SIZE            (CELL_SPURS_TASKSET_CLASS0_SIZE + _CELL_SPURS_TASKSET_CLASS1_EXTENDED_SIZE)
#define CELL_SPURS_TASKSET_CLASS_NAME             "taskset"

#define CELL_SPURS_TASKSET_ATTRIBUTE_ALIGN        8
#define CELL_SPURS_TASKSET_ATTRIBUTE_SIZE         512

typedef struct CellSpursTasksetAttribute {
    unsigned char skip[CELL_SPURS_TASKSET_ATTRIBUTE_SIZE];
} __attribute__((aligned(CELL_SPURS_TASKSET_ATTRIBUTE_ALIGN))) CellSpursTasksetAttribute;

/* Flesh out the CellSpursTaskset declaration from types.h.  The
 * forward-decl in types.h is compatible - C permits defining a
 * previously-forward-declared struct at any point in its scope. */
struct CellSpursTaskset {
    unsigned char skip[CELL_SPURS_TASKSET_SIZE];
} __attribute__((aligned(CELL_SPURS_TASKSET_ALIGN)));

#define CELL_SPURS_TASKSET2_SIZE                  CELL_SPURS_TASKSET_CLASS1_SIZE
#define CELL_SPURS_TASKSET2_ALIGN                 128

/* Task-binary embedding descriptor.  The reference SDK's ELF→object
 * tool emits one of these alongside the SPU ELF payload; Task2
 * variants accept it to avoid re-parsing the ELF at runtime. */
typedef struct CellSpursTaskBinInfo {
    uint64_t               eaElf;
    uint32_t               sizeContext;
    uint32_t               __reserved__;
    CellSpursTaskLsPattern lsPattern;
} __attribute__((aligned(16))) CellSpursTaskBinInfo;

#define CELL_SPURS_MAX_TASK_NAME_LENGTH 32

typedef struct CellSpursTaskNameBuffer {
    char taskName[CELL_SPURS_MAX_TASK][CELL_SPURS_MAX_TASK_NAME_LENGTH];
} __attribute__((aligned(16))) CellSpursTaskNameBuffer;

#define CELL_SPURS_TASKSET_ATTRIBUTE2_ALIGN       CELL_SPURS_TASKSET_ATTRIBUTE_ALIGN
#define CELL_SPURS_TASKSET_ATTRIBUTE2_SIZE        CELL_SPURS_TASKSET_ATTRIBUTE_SIZE

typedef struct CellSpursTasksetAttribute2 {
    uint32_t                 revision;
    const char              *name           ATTRIBUTE_PRXPTR;
    uint64_t                 argTaskset;
    uint8_t                  priority[8];
    uint32_t                 maxContention;
    int32_t                  enableClearLs;
    CellSpursTaskNameBuffer *taskNameBuffer ATTRIBUTE_PRXPTR;
    uint8_t                  __reserved__[CELL_SPURS_TASKSET_ATTRIBUTE2_SIZE
                                          - (sizeof(uint32_t)
                                             + 4 /* PRXPTR name */
                                             + sizeof(uint64_t)
                                             + sizeof(uint8_t) * 8
                                             + sizeof(uint32_t)
                                             + sizeof(int32_t)
                                             + 4 /* PRXPTR taskNameBuffer */)];
} __attribute__((aligned(CELL_SPURS_TASKSET_ATTRIBUTE2_ALIGN))) CellSpursTasksetAttribute2;

/* Taskset2: extends Taskset with the class-1 extended bytes.  The
 * C++ derived form below lets a CellSpursTaskset2* convert to a
 * CellSpursTaskset* without a pointer cast; the C branch is flat. */
#ifndef __cplusplus
typedef struct CellSpursTaskset2 {
    unsigned char skip[CELL_SPURS_TASKSET2_SIZE];
} __attribute__((aligned(CELL_SPURS_TASKSET2_ALIGN))) CellSpursTaskset2;
#endif

#ifdef __cplusplus
}   /* extern "C" */

struct CellSpursTaskset2 : public CellSpursTaskset {
    unsigned char skip[CELL_SPURS_TASKSET2_SIZE - CELL_SPURS_TASKSET_SIZE];
} __attribute__((aligned(CELL_SPURS_TASKSET2_ALIGN)));

#endif   /* __cplusplus */

#endif   /* __PS3DK_CELL_SPURS_TASK_TYPES_H__ */
