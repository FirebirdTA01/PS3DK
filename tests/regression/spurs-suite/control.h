/* spurs-suite SPU-side control: task kinds and result slots. */
#ifndef SPURS_SUITE_CONTROL_H
#define SPURS_SUITE_CONTROL_H

#include "common.h"

/* task kinds (argTask u32[3]; u32[2] = the object's EA); each reports in
 * the slot of the same number: status = the call's rc */
#define C_RUN_CHAIN       0   /* cellSpursRunJobChain(object) */
#define C_SHUTDOWN_CHAIN  1   /* cellSpursShutdownJobChain(object) */
#define C_GUARD_NOTIFY    2   /* cellSpursJobGuardNotify(object) */
#define C_ERRORS          3   /* argument errors; value = failing step */
#define C_SHUTDOWN_OWN    4   /* cellSpursShutdownTaskset(own taskset) */
#define C_CREATE          5   /* cellSpursCreateTask(C_CHILD); value = child id */
#define C_CHILD           6   /* created by C_CREATE; value = own task id */
#define C_CREATE_ATTR     7   /* cellSpursCreateTaskWithAttribute(C_CHILD2) */
#define C_CHILD2          8   /* created by C_CREATE_ATTR */
#define C_SLOTS           9

/* parameter block for C_CREATE / C_CREATE_ATTR (object EA, 16-aligned) */
typedef struct ctl_params {
    unsigned long long elf, context[2], slots;
} __attribute__((aligned(16))) ctl_params;

#define C_JOBS            4   /* jobs in the run-from-SPU chain */
#define C_JOB_MAGIC       0xc0a70000u

#endif
