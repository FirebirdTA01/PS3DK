/* spurs-suite SPU services: slots, the report block and constants. */
#ifndef SPURS_SUITE_SERVICES_H
#define SPURS_SUITE_SERVICES_H

#include "common.h"

/* argTask: u64[0] = result slot array EA, u32[2] = report block EA,
 * u32[3] = kind.  The task reports in slot 0: status = first failed step,
 * value = what it got, extra = what it wanted. */
#define S_TASK      0
#define S_SLOTS     1

#define S_TRACE_MAGIC   0x5eed7ace
#define S_PRIORITY      5
#define S_CONTENTION    3
#define S_HEAP          (16 * 1024)
#define S_STACK         (16 * 1024)

typedef struct s_report {
    unsigned int wid;               /* the task's workload */
    unsigned int waiting;           /* 1 while it waits in WaitSignal2 */
    unsigned int idleSpuRc;         /* _cellSpursRequestIdleSpu's rc */
    unsigned int pad;
    unsigned long long semaphore;   /* EA of a 128-byte line for the semaphore */
    unsigned long long pad2[13];
} __attribute__((aligned(128))) s_report;

#endif
