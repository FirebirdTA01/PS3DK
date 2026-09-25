/* cell/spurs/task.h - SPU-side Spurs task runtime surface.
 *
 * Canonical include for SPU ELFs that ship as Spurs tasks.  Declares
 * the canonical task entry (cellSpursTaskMain), the legacy entry the CRT
 * calls (cellSpursMain), and
 * the runtime helpers implemented in libspurs_task.a.
 *
 * Build expectations:
 *   spu-elf-gcc -nostartfiles -T $(PS3DK)/spu/ldscripts/spurs_task.ld
 *               -lspurs_task
 *
 * The user provides:
 *   int cellSpursTaskMain(qword argTask, uint64_t argTaskset);
 * Its result is passed to cellSpursTaskExit. A user-provided legacy
 * cellSpursMain overrides the archive bridge, including when both entries
 * are defined. Link application objects before libspurs_task.a.
 *
 * argTask arrives in r3 (full 16-byte vector),
 * argTaskset arrives in r4 preferred slot (64-bit EA).
 */
#ifndef __PS3DK_CELL_SPURS_TASK_H__
#define __PS3DK_CELL_SPURS_TASK_H__

#ifndef __SPU__
#error "cell/spurs/task.h is SPU-only"
#endif

#include <stdint.h>
#include <spu_intrinsics.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical entry: return the task exit code. Both entries have C linkage. */
int cellSpursTaskMain(qword argTask, uint64_t argTaskset);
/* Legacy override; crt branches here on task start. */
void cellSpursMain(qword argTask, uint64_t argTaskset);

/* Runtime dispatch -- all implemented in libspurs_task.a via the
 * 0x2fb0 control block. */
void     cellSpursExit(void)           __attribute__((noreturn));
void     cellSpursTaskExit(int code)   __attribute__((noreturn));
unsigned cellSpursGetTaskId(void);
uint64_t cellSpursGetTasksetAddress(void);
int      cellSpursYield(void);
unsigned cellSpursTaskPoll(void);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_TASK_H__ */
