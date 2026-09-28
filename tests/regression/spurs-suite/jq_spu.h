/* spurs-suite SPU job-queue runtime: job modes, constants. */
#ifndef SPURS_SUITE_JQ_SPU_H
#define SPURS_SUITE_JQ_SPU_H

#include "common.h"

/* jobs: workArea.userData[0] = 16-byte output EA, [1] = mode, [2] = the
 * job queue EA, [3] = the SPURS EA (INFO) or the suspend buffer EA (WAIT).
 * Output: { magic, first failed step (0 = none), got, want }; WAIT puts
 * { 0, 1, 0, 0 } before it suspends and { magic, WaitSignal rc, 0, 0 }
 * after it resumes. */
#define Q_INFO      0
#define Q_WAIT      1
#define Q_PLAIN     2
#define Q_PUSH      3   /* pushes two jobs ([4], [5]) with semaphore [3] */
#define Q_PORT      4   /* port [3]: pushes [4] with sync; port [5] with a descriptor
                           buffer [7] copy-pushes descriptor [6] with sync */
#define Q_PORT2     5   /* creates Port2 [3], pushes job [4] and job list [5] with sync */
#define Q_PORT2S    6   /* syncs (without blocking) and destroys Port2 [3] made by the PPU */
#define Q_SLOW      7   /* like PLAIN, but runs for a few milliseconds first */
#define Q_SIGNAL    8   /* wakes the suspended WAIT job ([3] = its suspend buffer) */
#define Q_SUSPSIZE  9   /* GetSuspendedJobSize of descriptors [4] (Q_SS_CASES x 256 bytes)
                           with sizes [6]: puts { rc, size } for attr 0 and 1 to [5] */
#define Q_SS_CASES  6
#define Q_CPP       10  /* C++ checks (jq_spu_cpp.cpp); a global's destructor puts
                           { Q_CPP_DTOR, objects built } to [3] after the job */
#define Q_CPP_DTOR  0xd70a0000u
#define Q_MAGIC     0x0e0e0000u
#define Q_MAX_DESC  256

/* the task: argTask u64[0] = result slot EA, u32[2] = semaphore EA,
 * u32[3] = job queue EA.  It reports value = 1 while it waits in
 * Acquire, then status = first failed step, value = 2 when done. */
#define Q_ACQUIRE   2

/* then the descriptor pool through a Port2 (value = 3 when done); its
 * parameters sit in the line after the semaphore */
typedef struct jq_task_params {
    uint32_t port2;         /* Port2 EA */
    uint32_t plain;         /* 128-byte Q_PLAIN job to copy into the pool */
    uint32_t slow;          /* 128-byte Q_SLOW job */
    uint32_t pad;
} jq_task_params;
#define Q_POOL      16      /* 128-byte descriptors in the pool */
#define Q_COPIES    8       /* CopyPushes made while the pool is used up */

#endif
