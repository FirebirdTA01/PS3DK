/* spurs-suite Task2: kinds, parameters and codes. */
#ifndef SPURS_SUITE_TASK2_H
#define SPURS_SUITE_TASK2_H

#include "common.h"

/* argTask: u64[0] = result slot array EA, u32[2] = parameter block EA (the
 * parent) or the exit code to return (a child), u32[3] = kind. */
#define T2_PARENT  0    /* runs the steps; reports in slot 0 */
#define T2_CHILD   1    /* returns u32[2] as its exit code */
#define T2_SLOTS   1

/* The parent reports status = first failed step (0 = none), value = the
 * rc or code that step got, extra = what it wanted; its exit code is: */
#define T2_PARENT_CODE 0x7a5c

/* the exit code of the task the PPU creates with an exit-code container */
#define T2_PPU_CHILD_CODE 0x2468

typedef struct t2_params {
    unsigned long long elf;          /* the suite image (parent and children) */
    unsigned long long plainTaskset; /* a CellSpursTaskset that is not a Taskset2 */
    unsigned long long binInfo;      /* CellSpursTaskBinInfo of the image */
    unsigned long long exitCode;     /* a 128-byte exit-code container */
    unsigned long long context;      /* a full context save area for one child */
    unsigned long long pad[3];
} __attribute__((aligned(128))) t2_params;

#endif
