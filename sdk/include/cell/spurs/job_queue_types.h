/* cell/spurs/job_queue_types.h - SPURS job-queue C types + constants.
 *
 * A job queue is a multi-producer, multi-consumer SPU-resident
 * workload built on top of SPURS.  Producers submit job descriptors
 * (CellSpursJob256 / Job128 / ...) through ports; consumers (SPU
 * worker threads) pull descriptors off the queue and run them.
 *
 * This header declares the opaque container types (queue, port, port2,
 * semaphore, attribute, suspended-job, descriptor pool) along with
 * size / alignment macros, error / flag enums, and exception-handler
 * function-pointer typedefs.  The C++ wrapper classes live in
 * job_queue_cpp_types.h.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_TYPES_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_TYPES_H__

#include <stdint.h>
#include <stddef.h>
#include <cell/spurs/types.h>
#include <cell/spurs/job_descriptor.h>
#include <cell/spurs/exception_types.h>
#include <cell/trace/trace_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Revision / class-name (PRX gates ABI compatibility on these) ---- */

#define CELL_SPURS_JOBQUEUE_REVISION    2
#define CELL_SPURS_JOBQUEUE_CLASS_NAME  "JobQueue"

/* -- Capacity / cap constants ---------------------------------------- */

#define CELL_SPURS_JOBQUEUE_MAX_DEPTH                 (1 << 15)
#define CELL_SPURS_JOBQUEUE_MAX_SIZE_JOB_MEMORY       (221 * 1024)
#define CELL_SPURS_JOBQUEUE_MAX_NUM_MAX_GRAB          16

#define CELL_SPURS_JOBQUEUE_MAX_CLIENTS               127
#define CELL_SPURS_JOBQUEUE_MAX_CLIENTS_BEFORE_330    256

#define CELL_SPURS_JOBQUEUE_MAX_HANDLE                1024
#define CELL_SPURS_JOBQUEUE_HANDLE_INVALID            (-1)

#define CELL_SPURS_JOBQUEUE_MAX_TAG                   15

/* Command buffer size formula.  `x` is the application-requested queue
 * depth in entries; the buffer reserves a per-client tail-pointer
 * region (sized to the pre-3.30 ceiling so the runtime can support
 * either 127- or 256-client builds) plus the rounded-up depth in
 * uint64_t job-command slots. */
#define CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(x)                          \
    ((CELL_SPURS_JOBQUEUE_MAX_CLIENTS_BEFORE_330 + (((x) + 15) & ~15))      \
     * sizeof(uint64_t))

#define CELL_SPURS_JOBQUEUE_COMMAND_BUFFER_ALIGN  128
#define _CELL_SPURS_JOBQUEUE_WAITING_QUEUE_ALIGN  128

/* -- Enums ----------------------------------------------------------- */

enum CellSpursJobQueueWaitingMode {
    CELL_SPURS_JOBQUEUE_WAITING_MODE_SLEEP = 0,
    CELL_SPURS_JOBQUEUE_WAITING_MODE_BUSY  = 1
};

enum CellSpursJobQueueSuspendedJobAttribute {
    CELL_SPURS_JOBQUEUE_JOB_SAVE_ALL      = 0,
    CELL_SPURS_JOBQUEUE_JOB_SAVE_WRITABLE = 1
};

/* -- Job-descriptor pool --------------------------------------------- */

#define CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_ALIGN 128

/* Pool-size formula: for each present descriptor class (nJobN > 0), the
 * pool reserves a 128-byte-aligned per-client waiter list followed by
 * `nJobN` job-descriptor slots of that size.  Classes with count 0
 * contribute nothing. */
#define _CELL_SPURS_JOBQUEUE_POOL_CLIENT_LIST                               \
    ((CELL_SPURS_JOBQUEUE_MAX_CLIENTS * sizeof(uint64_t)                    \
      + _CELL_SPURS_JOBQUEUE_WAITING_QUEUE_ALIGN - 1)                       \
     & ~(_CELL_SPURS_JOBQUEUE_WAITING_QUEUE_ALIGN - 1))

#define CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_SIZE(                       \
        nJob64, nJob128, nJob256, nJob384,                                  \
        nJob512, nJob640, nJob768, nJob896)                                 \
    ( (((nJob64)  > 0) ? (_CELL_SPURS_JOBQUEUE_POOL_CLIENT_LIST + (nJob64)  *  64) : 0) \
    + (((nJob128) > 0) ? (_CELL_SPURS_JOBQUEUE_POOL_CLIENT_LIST + (nJob128) * 128) : 0) \
    + (((nJob256) > 0) ? (_CELL_SPURS_JOBQUEUE_POOL_CLIENT_LIST + (nJob256) * 256) : 0) \
    + (((nJob384) > 0) ? (_CELL_SPURS_JOBQUEUE_POOL_CLIENT_LIST + (nJob384) * 384) : 0) \
    + (((nJob512) > 0) ? (_CELL_SPURS_JOBQUEUE_POOL_CLIENT_LIST + (nJob512) * 512) : 0) \
    + (((nJob640) > 0) ? (_CELL_SPURS_JOBQUEUE_POOL_CLIENT_LIST + (nJob640) * 640) : 0) \
    + (((nJob768) > 0) ? (_CELL_SPURS_JOBQUEUE_POOL_CLIENT_LIST + (nJob768) * 768) : 0) \
    + (((nJob896) > 0) ? (_CELL_SPURS_JOBQUEUE_POOL_CLIENT_LIST + (nJob896) * 896) : 0) )

typedef struct CellSpursJobQueueJobDescriptorPool {
    int nJob64, nJob128, nJob256, nJob384, nJob512, nJob640, nJob768, nJob896;
} CellSpursJobQueueJobDescriptorPool;

/* -- Push flags ------------------------------------------------------ */

#define CELL_SPURS_JOBQUEUE_FLAG_SYNC_JOB       (0x00000001u)
#define CELL_SPURS_JOBQUEUE_FLAG_EXCLUSIVE_JOB  (0x00000002u)
#define CELL_SPURS_JOBQUEUE_FLAG_NON_BLOCKING   (0x00000004u)

/* -- Defaults -------------------------------------------------------- */

#define CELL_SPURS_JOBQUEUE_DEFAULT_MAX_GRAB             4
#define CELL_SPURS_JOBQUEUE_DEFAULT_MAX_NUM_JOBS_ON_SPU  255

/* -- Opaque container types ------------------------------------------ */

#define CELL_SPURS_JOBQUEUE_ALIGN  128
#define CELL_SPURS_JOBQUEUE_SIZE   (128 * 16 + 2048)

typedef struct CellSpursJobQueue {
    unsigned char skip[CELL_SPURS_JOBQUEUE_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBQUEUE_ALIGN))) CellSpursJobQueue;

typedef int32_t CellSpursJobQueueHandle;

#define CELL_SPURS_JOBQUEUE_SEMAPHORE_MAX_ACQUIRE_COUNT  ((1 << (32 - 4)) - 1)
#define CELL_SPURS_JOBQUEUE_SEMAPHORE_SIZE               128
#define CELL_SPURS_JOBQUEUE_SEMAPHORE_ALIGN              128

typedef struct CellSpursJobQueueSemaphore {
    unsigned char skip[CELL_SPURS_JOBQUEUE_SEMAPHORE_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBQUEUE_SEMAPHORE_ALIGN))) CellSpursJobQueueSemaphore;

#define CELL_SPURS_JOBQUEUE_WAITING_JOB_SIZE   128
#define CELL_SPURS_JOBQUEUE_WAITING_JOB_ALIGN  128

typedef struct CellSpursJobQueueWaitingJob {
    unsigned char skip[CELL_SPURS_JOBQUEUE_WAITING_JOB_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBQUEUE_WAITING_JOB_ALIGN))) CellSpursJobQueueWaitingJob;

#define CELL_SPURS_JOBQUEUE_ATTRIBUTE_ALIGN  8
#define CELL_SPURS_JOBQUEUE_ATTRIBUTE_SIZE   512

typedef struct CellSpursJobQueueAttribute {
    unsigned char skip[CELL_SPURS_JOBQUEUE_ATTRIBUTE_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBQUEUE_ATTRIBUTE_ALIGN))) CellSpursJobQueueAttribute;

#define CELL_SPURS_JOBQUEUE_SUSPENDED_JOB_HEADER_ALIGN  128
#define CELL_SPURS_JOBQUEUE_SUSPENDED_JOB_HEADER_SIZE   (128 * 10)

typedef struct CellSpursJobQueueSuspendedJobHeader {
    unsigned char skip[CELL_SPURS_JOBQUEUE_SUSPENDED_JOB_HEADER_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBQUEUE_SUSPENDED_JOB_HEADER_ALIGN))) CellSpursJobQueueSuspendedJobHeader;

#define CELL_SPURS_JOBQUEUE_SUSPENDED_JOB_ALIGN  128
#define CELL_SPURS_JOBQUEUE_SUSPENDED_JOB_SIZE   (256 * 1024)

typedef struct CellSpursJobQueueSuspendedJob {
    CellSpursJobQueueWaitingJob job;
    unsigned char skip[CELL_SPURS_JOBQUEUE_SUSPENDED_JOB_SIZE
                       - CELL_SPURS_JOBQUEUE_WAITING_JOB_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBQUEUE_SUSPENDED_JOB_ALIGN))) CellSpursJobQueueSuspendedJob;

/* Trace packet emitted by the SPU job-queue runtime. */
typedef struct _CellSpursJobQueueTracePacket {
    CellTraceHeader header;
    unsigned char   skip[8];
} __attribute__((aligned(16))) _CellSpursJobQueueTracePacket;

/* -- Job-checker inline ---------------------------------------------- */

/* Front-end check before submitting a job descriptor to the queue.
 * `maxSizeJob` MUST be one of {256, 384, ..., 896} (multiples of 128
 * in [256, 1024)).  Other invariants (alignment, sizeBinary != 0,
 * sizeOut / sizeIn / sizeDmaList multiples) are enforced by the
 * shared cellSpursCheckJob helper. */
static inline int cellSpursJobQueueCheckJob(const CellSpursJob256 *pJob,
                                            unsigned int sizeJob,
                                            unsigned int maxSizeJob)
{
    if (__builtin_expect(maxSizeJob < 256 || maxSizeJob >= 1024
                         || (maxSizeJob % 128), 0))
        return CELL_SPURS_JOB_ERROR_INVAL;
    return cellSpursCheckJob(pJob, sizeJob, maxSizeJob);
}

/* -- Pipeline-info snapshot (passed to exception handlers) ----------- */

typedef struct CellSpursJobQueuePipelineInfo {
    struct {
        CellSpursJob256 *job;
        size_t           sizeJob;
        unsigned int     dmaTag;
    } fetchStage;
    struct {
        CellSpursJob256 *job;
        size_t           sizeJob;
        unsigned int     dmaTag;
    } inputStage;
    struct {
        CellSpursJob256 *job;
        size_t           sizeJob;
        uint32_t         lsaJobContext;
        unsigned int     dmaTag;
    } executeStage[2];
    struct {
        CellSpursJob256 *job;
        size_t           sizeJob;
        unsigned int     dmaTag;
    } outputStage;
} CellSpursJobQueuePipelineInfo;

/* -- Exception-handler function pointer types ------------------------ */

typedef void (*CellSpursJobQueueExceptionEventHandler)(
    CellSpurs                            *spurs,
    CellSpursJobQueue                    *jobQueue,
    const CellSpursExceptionInfo         *exceptionInfo,
    const CellSpursJobQueuePipelineInfo  *pipelineInfo,
    void                                 *arg);

typedef void (*CellSpursJobQueueExceptionEventHandler2)(
    CellSpurs                            *spurs,
    CellSpursJobQueue                    *jobQueue,
    const CellSpursExceptionInfo         *exceptionInfo,
    const CellSpursJobQueuePipelineInfo  *pipelineInfo,
    void                                 *arg,
    const void                           *eaJobBinaries[2],
    uint32_t                              lsAddrJobBinaries[2]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_TYPES_H__ */
