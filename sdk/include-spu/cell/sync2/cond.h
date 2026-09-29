/* cell/sync2/cond.h - SPU cond API (libsync2.a).
 *
 * The object is named by its effective address; dmaTag is the MFC tag the
 * call may use (0..31).
 */
#ifndef __PS3DK_CELL_SYNC2_COND_H_SPU__
#define __PS3DK_CELL_SYNC2_COND_H_SPU__

#include <stdint.h>
#include <cell/sync2/cond_types.h>
#include <cell/sync2/thread_types.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellSync2CondWait(uint64_t eaCondition, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2CondSignal(uint64_t eaCondition, const CellSync2ThreadConfig *config, unsigned int dmaTag);
int cellSync2CondSignalAll(uint64_t eaCondition, const CellSync2ThreadConfig *config, unsigned int dmaTag);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYNC2_COND_H_SPU__ */
