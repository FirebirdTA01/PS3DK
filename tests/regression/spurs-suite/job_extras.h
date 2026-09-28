/* spurs-suite job extras: parameters, job modes and constants. */
#ifndef SPURS_SUITE_JOB_EXTRAS_H
#define SPURS_SUITE_JOB_EXTRAS_H

#include "common.h"

/* argTask: u64[0] = result slot EA, u32[2] = parameter block EA.  The
 * task reports: status = first failed step, value = got, extra = want. */
#define X_URGENT_JOBS   4       /* urgent slots the task fills */
#define X_MAX_GRAB      4
#define X_JOB_MAGIC     0x70b0000u

/* job modes (workArea.userData[2]) */
#define X_JOB_PLAIN     0
#define X_JOB_MEMCHECK  1

typedef struct x_params {
    unsigned long long chain;       /* created, not yet run */
    unsigned long long jobs[X_URGENT_JOBS];   /* job descriptors for the urgent slots */
    unsigned long long callList;    /* command list: JOB(jobs[1]), RET */
    unsigned long long jobbin2;     /* the job image */
    unsigned long long header;      /* 0x30 bytes: the task's SetJobbin2Param result */
    unsigned long long pad[7];
} __attribute__((aligned(128))) x_params;

#endif
