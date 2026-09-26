/* cell/sheap/key_sheap_buffer.h -- SPU keyed shared buffer.
 *
 * A raw block of `size` bytes in a keyed heap.  The size recorded in the
 * object is the one this caller passed to New; an attacher that passes 0
 * sees 0 even though the block is the creator's size.
 */
#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_BUFFER_H_SPU__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_BUFFER_H_SPU__

#include <cell/sheap/key_sheap.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapBufferNew(CellKeySheapBuffer *obj, uint64_t ea_ksheap,
                          CellSheapKey key, uint64_t size);
void cellKeySheapBufferDelete(CellKeySheapBuffer *obj);

static inline uint64_t cellKeySheapBufferGetEa(CellKeySheapBuffer *obj)
{
    return obj->ea;
}

static inline uint64_t cellKeySheapBufferGetSize(CellKeySheapBuffer *obj)
{
    return obj->size;
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_BUFFER_H_SPU__ */
