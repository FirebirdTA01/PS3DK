/* cell/spurs/ready_count.h - workload ready counts.
 *
 * A workload's ready count (0..255) tells the SPURS kernel how many SPUs
 * the workload can use.
 * SPU: the _cellSpursReadyCount* calls change it atomically in the SPURS
 * instance and return the previous value.  The cellSpursReadyCount*
 * inlines keep the older interface, whose LS buffer and instance EA
 * arguments are no longer needed.
 * PPU: cellSpursReadyCount* change it from the PPU; the swap, compare-and-
 * swap and add forms store the previous value to *old.
 */
#ifndef __PS3DK_CELL_SPURS_READY_COUNT_H__
#define __PS3DK_CELL_SPURS_READY_COUNT_H__

#include <stdint.h>
#include <cell/spurs/types.h>
#include <cell/spurs/error.h>

#ifdef __SPU__

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    _CELL_SPURS_READY_COUNT_SWAP,   /* set value1 */
    _CELL_SPURS_READY_COUNT_CAS,    /* set value2 if the count is value1 */
    _CELL_SPURS_READY_COUNT_ADD     /* add value1, clamped to 0..255 */
} _CellSpursReadyCountOpcode;

unsigned _cellSpursReadyCountOperator(CellSpursWorkloadId id, _CellSpursReadyCountOpcode opcode,
                                      unsigned value1, unsigned value2);
unsigned _cellSpursReadyCountSwap(CellSpursWorkloadId id, unsigned value);
unsigned _cellSpursReadyCountCompareAndSwap(CellSpursWorkloadId id, unsigned compare, unsigned swap);
unsigned _cellSpursReadyCountAdd(CellSpursWorkloadId id, int value);

#ifdef __cplusplus
}   /* extern "C" */
#endif

static inline int
cellSpursReadyCountSwap(unsigned char *ls, uint64_t eaSpurs, CellSpursWorkloadId id,
                        unsigned *old, unsigned value)
{
    (void)ls; (void)eaSpurs;
    if (!old)
        return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if (id >= CELL_SPURS_MAX_WORKLOAD2 || value > 255)
        return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;
    *old = _cellSpursReadyCountOperator(id, _CELL_SPURS_READY_COUNT_SWAP, value, 0);
    return CELL_OK;
}

static inline int
cellSpursReadyCountCompareAndSwap(unsigned char *ls, uint64_t eaSpurs, CellSpursWorkloadId id,
                                  unsigned *old, unsigned compare, unsigned swap)
{
    (void)ls; (void)eaSpurs;
    if (!old)
        return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if (id >= CELL_SPURS_MAX_WORKLOAD2 || compare > 255 || swap > 255)
        return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;
    *old = _cellSpursReadyCountOperator(id, _CELL_SPURS_READY_COUNT_CAS, compare, swap);
    return CELL_OK;
}

static inline int
cellSpursReadyCountAdd(unsigned char *ls, uint64_t eaSpurs, CellSpursWorkloadId id,
                       unsigned *old, int value)
{
    (void)ls; (void)eaSpurs;
    if (!old)
        return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if (id >= CELL_SPURS_MAX_WORKLOAD2)
        return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;
    *old = _cellSpursReadyCountOperator(id, _CELL_SPURS_READY_COUNT_ADD, (unsigned)value, 0);
    return CELL_OK;
}

static inline int
cellSpursReadyCountStore(unsigned char *ls, uint64_t eaSpurs, CellSpursWorkloadId id, unsigned value)
{
    (void)ls; (void)eaSpurs;
    if (id >= CELL_SPURS_MAX_WORKLOAD2 || value > 255)
        return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;
    (void)_cellSpursReadyCountOperator(id, _CELL_SPURS_READY_COUNT_SWAP, value, 0);
    return CELL_OK;
}

#else /* PPU */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CellSpurs CellSpurs;

int cellSpursReadyCountStore(CellSpurs *spurs, CellSpursWorkloadId id, unsigned int value);
int cellSpursReadyCountSwap(CellSpurs *spurs, CellSpursWorkloadId id, unsigned int *old, unsigned int swap);
int cellSpursReadyCountCompareAndSwap(CellSpurs *spurs, CellSpursWorkloadId id, unsigned int *old,
                                      unsigned int compare, unsigned int swap);
int cellSpursReadyCountAdd(CellSpurs *spurs, CellSpursWorkloadId id, unsigned int *old, int value);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __SPU__ */

#endif /* __PS3DK_CELL_SPURS_READY_COUNT_H__ */
