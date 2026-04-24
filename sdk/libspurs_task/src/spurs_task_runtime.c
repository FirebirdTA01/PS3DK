/* libspurs_task SPU runtime dispatch glue.
 *
 * The Spurs SPRX reserves LS bytes 0..0x3000 for its own code + task
 * control blocks.  Two well-known offsets matter to tasks:
 *
 *   0x2fb0 (CONTROL): 16-byte vector.  Bytes [8..11] hold
 *   `dispatch_base` - a 4-byte LS pointer to a table maintained by
 *   the SPRX for the currently-running task.
 *
 *   0x2fd0 (STATE): 16-byte vector.  Bytes [0..3] are where the task
 *   writes its exit code before calling the exit dispatcher.
 *
 * Relative to `dispatch_base`:
 *   +0xb8: eaTaskset (uint64_t, the PPU-side CellSpursTaskset EA)
 *   +0xc4: 4-byte function pointer - call with r3=service ID:
 *          0 = exit, 1 = yield, 3 = taskPoll, ...
 *   +0xd4: taskId (uint32_t)
 *
 * cellSpursExit / cellSpursTaskExit are in spurs_task_exit.S - they
 * hit the critical path, and the C compiler can't interleave the
 * load/store/dispatch chain as tightly as the hand scheduling in
 * the reference libspurs.a.  Leaf helpers below stay in C - GCC
 * produces identical 6-instruction bodies.
 */
#include <stdint.h>

#define SPURS_TASK_CTRL_ADDR   0x2fb0U
#define SPURS_DISPATCH_OFF     0xc4U
#define SPURS_TASKSET_EA_OFF   0xb8U
#define SPURS_TASKID_OFF       0xd4U

#define SPURS_SVC_YIELD        1
#define SPURS_SVC_TASK_POLL    3

static inline uint32_t spurs_dispatch_base(void)
{
    return *(volatile uint32_t *)(uintptr_t)(SPURS_TASK_CTRL_ADDR + 8);
}

static inline uint32_t spurs_dispatch_call(uint32_t base, int service)
{
    typedef uint32_t (*fn_t)(int);
    fn_t fn = (fn_t)(uintptr_t)*(volatile uint32_t *)(uintptr_t)(base + SPURS_DISPATCH_OFF);
    return fn(service);
}

unsigned int cellSpursGetTaskId(void)
{
    return *(volatile uint32_t *)(uintptr_t)(spurs_dispatch_base() + SPURS_TASKID_OFF);
}

uint64_t cellSpursGetTasksetAddress(void)
{
    return *(volatile uint64_t *)(uintptr_t)(spurs_dispatch_base() + SPURS_TASKSET_EA_OFF);
}

int cellSpursYield(void)
{
    return (int)spurs_dispatch_call(spurs_dispatch_base(), SPURS_SVC_YIELD);
}

unsigned int cellSpursTaskPoll(void)
{
    return spurs_dispatch_call(spurs_dispatch_base(), SPURS_SVC_TASK_POLL);
}
