/* cell/spurs/spu_task.h - SPU-side Spurs task runtime surface.
 *
 * Include in SPU ELFs that ship as Spurs tasks.  Declares the
 * user-side entry point (cellSpursMain) the crt calls, plus the
 * runtime helpers implemented in libspurs_task.a.
 *
 * Build expectations:
 *   spu-elf-gcc -nostartfiles -T $(PS3DK)/spu/ldscripts/spurs_task.ld
 *               -lspurs_task
 *
 * The user provides:
 *   void cellSpursMain(qword argTask, uint64_t argTaskset);
 *
 * argTask arrives in r3 (full 16-byte vector),
 * argTaskset arrives in r4 preferred slot (64-bit EA).
 */
#ifndef __PS3DK_CELL_SPURS_SPU_TASK_H__
#define __PS3DK_CELL_SPURS_SPU_TASK_H__

#ifndef __SPU__
#error "cell/spurs/spu_task.h is SPU-only"
#endif

#include <stdint.h>
#include <spu_intrinsics.h>

#ifdef __cplusplus
extern "C" {
#endif

/* User-provided entry; crt branches here on task start. */
void cellSpursMain(qword argTask, uint64_t argTaskset);

/* Runtime dispatch - all implemented in libspurs_task.a via the
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

#endif /* __PS3DK_CELL_SPURS_SPU_TASK_H__ */
