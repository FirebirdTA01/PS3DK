/* cell/spurs/task_exit_code.h - CellSpursTaskExitCode type and constants.
 *
 * Included directly by code that needs the task exit code surface
 * (CellSpursTaskExitCode, CELL_SPURS_TASK_EXIT_CODE_ALIGN,
 * CELL_SPURS_TASK_EXIT_CODE_SIZE) but does not pull in the full
 * cell/spurs/task.h.
 *
 * Defined in task_types.h; this header forwards there.
 */
#ifndef __PS3DK_CELL_SPURS_TASK_EXIT_CODE_H__
#define __PS3DK_CELL_SPURS_TASK_EXIT_CODE_H__

#include <cell/spurs/task_types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int cellSpursTaskExitCodeInitialize(CellSpursTaskExitCode *exitCode);
extern int cellSpursTaskExitCodeGet(CellSpursTaskExitCode *exitCode,
                                    int *value);
extern int cellSpursTaskExitCodeTryGet(CellSpursTaskExitCode *exitCode,
                                       int *value);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_TASK_EXIT_CODE_H__ */
