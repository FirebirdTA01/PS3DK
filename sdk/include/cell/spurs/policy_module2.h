/* cell/spurs/policy_module2.h - SPURS policy modules, status interface.
 *
 * The kernel enters a status-form policy module at
 * cellSpursModuleEntryStatus with the reason it was dispatched
 * (CellSpursModulePollStatus bits); the module adjusts its workload's
 * ready count and receives the workload flag through the calls below.
 */
#ifndef __PS3DK_CELL_SPURS_POLICY_MODULE2_H__
#define __PS3DK_CELL_SPURS_POLICY_MODULE2_H__

#include <stdint.h>
#include <cell/spurs/types.h>
#include <cell/spurs/error.h>
#include <cell/spurs/common.h>
#include <cell/spurs/policy_module.h>
#include <cell/spurs/ready_count.h>

#ifdef __cplusplus
extern "C" {
#endif

void     cellSpursModuleEntryStatus(uintptr_t context, uint64_t arg, CellSpursModulePollStatus status)
         __attribute__((noreturn));
void     cellSpursModuleMainStatus(CellSpursModulePollStatus status, uint64_t arg);
int      _cellSpursWorkloadFlagReceiver2(CellSpursWorkloadId id, unsigned set);
uint64_t _cellSpursGetWorkloadFlag(void);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#define cellSpursModuleExit(context)    cellSpursModuleExit()

#define cellSpursModuleReadyCountSwap(context, value) \
    _cellSpursReadyCountOperator(cellSpursGetWorkloadId(), _CELL_SPURS_READY_COUNT_SWAP, (value), 0)
#define cellSpursModuleReadyCountCompareAndSwap(context, compare, swap) \
    _cellSpursReadyCountOperator(cellSpursGetWorkloadId(), _CELL_SPURS_READY_COUNT_CAS, (compare), (swap))
#define cellSpursModuleReadyCountAdd(context, value) \
    _cellSpursReadyCountOperator(cellSpursGetWorkloadId(), _CELL_SPURS_READY_COUNT_ADD, (value), 0)

static inline int cellSpursModuleSetWorkloadFlagReceiver2(void)
{
    return _cellSpursWorkloadFlagReceiver2(cellSpursGetWorkloadId(), 1);
}

static inline int cellSpursModuleUnsetWorkloadFlagReceiver2(void)
{
    return _cellSpursWorkloadFlagReceiver2(cellSpursGetWorkloadId(), 0);
}

static inline int cellSpursGetWorkloadFlag(uint64_t *flag)
{
    if (!flag)
        return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    *flag = _cellSpursGetWorkloadFlag();
    return CELL_OK;
}

#endif /* __PS3DK_CELL_SPURS_POLICY_MODULE2_H__ */
