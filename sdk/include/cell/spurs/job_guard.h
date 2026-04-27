/* cell/spurs/job_guard.h - SPURS job-guard opaque container.
 *
 * A guard sits at a chosen point in a job chain (placed via
 * CELL_SPURS_JOB_COMMAND_GUARD) and blocks chain progress until N
 * notifies have arrived from elsewhere.  The 128-byte storage matches
 * the SPRX wire layout - all live state is owned by the SPRX, the host
 * supplies only the byte container.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_GUARD_H__
#define __PS3DK_CELL_SPURS_JOB_GUARD_H__

#include <stdint.h>

#define CELL_SPURS_JOB_GUARD_ALIGN  128
#define CELL_SPURS_JOB_GUARD_SIZE   128

typedef struct CellSpursJobGuard {
    unsigned char skip[CELL_SPURS_JOB_GUARD_SIZE];
} __attribute__((aligned(CELL_SPURS_JOB_GUARD_ALIGN))) CellSpursJobGuard;

#endif /* __PS3DK_CELL_SPURS_JOB_GUARD_H__ */
