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
#define Q_MAGIC     0x0e0e0000u
#define Q_MAX_DESC  256

/* the task: argTask u64[0] = result slot EA, u32[2] = semaphore EA,
 * u32[3] = job queue EA.  It reports value = 1 while it waits in
 * Acquire, then status = first failed step, value = 2 when done. */
#define Q_ACQUIRE   2

#endif
