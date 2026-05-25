/* cell/sync/barrier_types.h -- CellSyncBarrier type layout.
 *
 * A 32-bit union: count (arrivals remaining) and total_count (reset
 * value).  Instances live at an EA and are manipulated by the
 * cellSyncBarrier* functions via MFC atomics.
 */
#ifndef __CELL_SYNC_BARRIER_TYPES_H_SPU__
#define __CELL_SYNC_BARRIER_TYPES_H_SPU__

#include <stdint.h>

typedef union {
    struct {
        uint16_t count;
        uint16_t total_count;
    };
    uint32_t uint_val;
} CellSyncBarrier;

#endif /* __CELL_SYNC_BARRIER_TYPES_H_SPU__ */
