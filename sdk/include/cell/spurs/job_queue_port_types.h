/* cell/spurs/job_queue_port_types.h - opaque CellSpursJobQueuePort.
 *
 * A port is a per-thread submission handle bound to a job queue.
 * Producers initialise a port, push descriptors through it, and
 * finalize when done.  Internal state is opaque; this header only
 * publishes size + alignment for stack / static reservation.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_TYPES_H__
#define __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SPURS_JOBQUEUE_PORT_ALIGN  128
#define CELL_SPURS_JOBQUEUE_PORT_SIZE   128

typedef struct CellSpursJobQueuePort {
    unsigned char skip[CELL_SPURS_JOBQUEUE_PORT_SIZE];
} __attribute__((aligned(CELL_SPURS_JOBQUEUE_PORT_ALIGN))) CellSpursJobQueuePort;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_JOB_QUEUE_PORT_TYPES_H__ */
