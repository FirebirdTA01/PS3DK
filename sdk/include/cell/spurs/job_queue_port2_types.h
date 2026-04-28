/* cell/spurs/job_queue_port2_types.h - opaque CellSpursJobQueuePort2.
 *
 * Port2 is the multi-tag-aware revision of CellSpursJobQueuePort that
 * exposes per-handle tag-mask sync.  Same opaque-blob shape as the
 * original port.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_TYPES_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SPURS_JOBQUEUE_PORT2_ALIGN  128
#define CELL_SPURS_JOBQUEUE_PORT2_SIZE   128

typedef struct CellSpursJobQueuePort2 {
    unsigned char skip[CELL_SPURS_JOBQUEUE_PORT2_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBQUEUE_PORT2_ALIGN))) CellSpursJobQueuePort2;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_PORT2_TYPES_H__ */
