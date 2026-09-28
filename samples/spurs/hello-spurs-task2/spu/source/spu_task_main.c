/* hello-spurs-task2 - SPU-side Task2 body.
 *
 * Loaded by the Task2 parent on the Taskset2; the crt (__spurs_task_start)
 * calls cellSpursTaskMain with argTask in r3 and argTaskset in r4.  The
 * return value of cellSpursTaskMain becomes the task's exit code, so the
 * PPU's joinTask2(id, &code) observes it.
 *
 * PPU passes the task number in argTask.u32[0]; this task returns
 * 100 + <task number>.
 */
#include <stdint.h>
#include <cell/spurs/spu_task.h>

int cellSpursTaskMain(qword argTask, uint64_t argTaskset)
{
    (void)argTaskset;

    /* argTask is a CellSpursTaskArgument (union u32[4] / u64[2]).
     * The PPU packed the task number into u32[0]. */
    CellSpursTaskArgument arg;
    *(qword *)&arg = argTask;

    return 100 + arg.u32[0];
}
