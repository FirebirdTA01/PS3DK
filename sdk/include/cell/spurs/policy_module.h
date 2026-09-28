/* cell/spurs/policy_module.h - SPURS policy-module surface.
 *
 * A policy module (PM) is the SPU program the SPURS kernel runs for a
 * workload; it decides what work the workload does on each SPU it gets.
 * The kernel loads it at LS 0xa00 and enters it at its entry point
 * (cellSpursModuleEntry; link with -nostartfiles -Ttext=0xa00 and name
 * the entry) with its context and the workload argument; it polls the
 * kernel for higher-priority work and leaves with cellSpursModuleExit.
 * (-mcustom-module builds the relocatable images such a module loads.)
 * <cell/spurs/policy_module2.h> has the status form of the interface.
 */
#ifndef __PS3DK_CELL_SPURS_POLICY_MODULE_H__
#define __PS3DK_CELL_SPURS_POLICY_MODULE_H__

#include <stdint.h>

/* the reason bits cellSpursModulePollStatus reports */
typedef uint32_t CellSpursModulePollStatus;
enum {
    CELL_SPURS_MODULE_POLL_STATUS_READYCOUNT = 1 << 0,
    CELL_SPURS_MODULE_POLL_STATUS_SIGNAL     = 1 << 1,
    CELL_SPURS_MODULE_POLL_STATUS_FLAG       = 1 << 2
};

#ifdef __SPU__
#include <cell/spurs/policy_module2.h>

#ifdef __cplusplus
extern "C" {
#endif

void cellSpursModuleEntry(uintptr_t context, uint64_t arg) __attribute__((noreturn));
void cellSpursModuleMain(uintptr_t context, uint64_t arg);
int  _cellSpursRequestIdleSpu(CellSpursWorkloadId id, unsigned count);
int  _cellSpursWorkloadFlagReceiver(CellSpursWorkloadId id, unsigned set);

#ifdef __cplusplus
}   /* extern "C" */
#endif

/* the older context-argument forms */
#define cellSpursModulePoll(context)            cellSpursModulePoll()
#define cellSpursModuleGetSpuId(context)        cellSpursGetCurrentSpuId()
#define cellSpursModuleGetWorkloadId(context)   cellSpursGetWorkloadId()
#define cellSpursModuleRequestIdleSpu(context, count) \
    (void)_cellSpursRequestIdleSpu(cellSpursGetWorkloadId(), (count))

static inline int
cellSpursRequestIdleSpu(unsigned char *ls, uint64_t eaSpurs, CellSpursWorkloadId id, unsigned count)
{
    (void)ls; (void)eaSpurs;
    return _cellSpursRequestIdleSpu(id, count);
}

static inline int cellSpursModuleSetWorkloadFlagReceiver(void)
{
    return _cellSpursWorkloadFlagReceiver(cellSpursGetWorkloadId(), 1);
}

static inline int cellSpursModuleUnsetWorkloadFlagReceiver(void)
{
    return _cellSpursWorkloadFlagReceiver(cellSpursGetWorkloadId(), 0);
}
#endif /* __SPU__ */

#endif /* __PS3DK_CELL_SPURS_POLICY_MODULE_H__ */
