/* spurs-suite LS patterns: kinds, report block and steps. */
#ifndef SPURS_SUITE_LS_PATTERN_H
#define SPURS_SUITE_LS_PATTERN_H

#include "common.h"

/* argTask: u64[0] = result slot array EA, u32[2] = report block EA,
 * u32[3] = kind.  A task reports in slot <kind>: status = the first step
 * that failed (0 = none), value = its task id, extra = what that step got. */
#define LP_CONTEXT  0   /* generate / save-area size / get / set / yield */
#define LP_ELF      1   /* patterns from the task's own ELF, error paths */
#define LP_SLOTS    2

/* LP_ELF puts the patterns it read here; the PPU checks them against its
 * own reading of the same ELF */
typedef struct lp_report {
    unsigned int loadable[4], readonly[4];
    unsigned int loadable_buf[4], readonly_buf[4];
} __attribute__((aligned(128))) lp_report;

#endif
