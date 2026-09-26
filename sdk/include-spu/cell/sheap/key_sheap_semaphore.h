/* cell/sheap/key_sheap_semaphore.h -- SPU keyed shared semaphore.
 *
 * A signed 32-bit counter in word 0 of a 128-byte block of a keyed heap,
 * set to the initial count by the creator.  P and TryP decrement it when
 * it is positive; V increments it.  All three work on the counter's line
 * with a reservation, using a 128-byte aligned line carved out of a
 * 256-byte buffer on the caller's stack (SPU stack frames are only
 * 16-byte aligned).
 */
#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_SEMAPHORE_H_SPU__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_SEMAPHORE_H_SPU__

#include <stdint.h>
#include <cell/atomic.h>
#include <cell/sync.h>
#include <cell/sheap/key_sheap.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapSemaphoreNew(CellKeySheapSemaphore *obj, uint64_t ea_ksheap,
                             CellSheapKey key, int count);
void cellKeySheapSemaphoreDelete(CellKeySheapSemaphore *obj);

#define __PS3DK_SHEAP_SEMAPHORE_LINE(raw) \
    ((uint32_t *)(((uintptr_t)(raw) + 127) & ~(uintptr_t)127))

static inline int cellKeySheapSemaphoreTryP(CellKeySheapSemaphore *obj)
{
    uint8_t raw[256];
    uint32_t *line = __PS3DK_SHEAP_SEMAPHORE_LINE(raw);
    int32_t count;

    do {
        count = (int32_t)cellAtomicLockLine32(line, obj->ea);
        if (count <= 0) {
            /* Leave the counter as it is. */
            (void)cellAtomicStoreConditional32(line, obj->ea, (uint32_t)count);
            return (int)CELL_SYNC_ERROR_BUSY;
        }
    } while (!cellAtomicStoreConditional32(line, obj->ea, (uint32_t)(count - 1)));
    return CELL_OK;
}

static inline void cellKeySheapSemaphoreP(CellKeySheapSemaphore *obj)
{
    uint8_t raw[256];
    uint32_t *line = __PS3DK_SHEAP_SEMAPHORE_LINE(raw);
    int32_t count;

    for (;;) {
        count = (int32_t)cellAtomicLockLine32(line, obj->ea);
        if (count <= 0)
            continue;
        if (cellAtomicStoreConditional32(line, obj->ea, (uint32_t)(count - 1)))
            return;
    }
}

static inline void cellKeySheapSemaphoreV(CellKeySheapSemaphore *obj)
{
    uint8_t raw[256];
    uint32_t *line = __PS3DK_SHEAP_SEMAPHORE_LINE(raw);

    (void)cellAtomicIncr32(line, obj->ea);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_SEMAPHORE_H_SPU__ */
