/* cell/sheap/key_sheap_rwm.h -- SPU keyed shared reader/writer buffer.
 *
 * A CellSyncRwm guarding `buffer_size` bytes: the Rwm header at the start
 * of the block and the data 128 bytes in.  The inline operations forward
 * to libsync with the caller's DMA tag.
 */
#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_RWM_H_SPU__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_RWM_H_SPU__

#include <cell/sheap/key_sheap.h>
#include <cell/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapRwmNew(CellKeySheapRwm *obj, uint64_t ea_ksheap,
                       CellSheapKey key, uint32_t buffer_size);
void cellKeySheapRwmDelete(CellKeySheapRwm *obj);

static inline int cellKeySheapRwmReadBegin(CellKeySheapRwm *obj, void *buf,
                                           unsigned int tag)
{
    return cellSyncRwmReadBegin(obj->ea, buf, tag);
}

static inline int cellKeySheapRwmTryReadBegin(CellKeySheapRwm *obj, void *buf,
                                              unsigned int tag)
{
    return cellSyncRwmTryReadBegin(obj->ea, buf, tag);
}

static inline int cellKeySheapRwmReadEnd(CellKeySheapRwm *obj, unsigned int tag)
{
    return cellSyncRwmReadEnd(obj->ea, tag);
}

static inline int cellKeySheapRwmWrite(CellKeySheapRwm *obj, void *buf,
                                       unsigned int tag)
{
    return cellSyncRwmWrite(obj->ea, buf, tag);
}

static inline int cellKeySheapRwmTryWrite(CellKeySheapRwm *obj, void *buf,
                                          unsigned int tag)
{
    return cellSyncRwmTryWrite(obj->ea, buf, tag);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_RWM_H_SPU__ */
