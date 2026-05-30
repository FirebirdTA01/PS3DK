#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_BUFFER_H__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_BUFFER_H__

#include <cell/sheap/key_sheap.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapBufferNew(CellKeySheapBuffer *obj, void *ksheap, CellSheapKey key, uint64_t size);
void cellKeySheapBufferDelete(CellKeySheapBuffer *obj);

#ifdef __cplusplus
}
#endif

static inline void *cellKeySheapBufferGetEa(CellKeySheapBuffer *obj) { return (void *)(uintptr_t)obj->ea; }
static inline uint64_t cellKeySheapBufferGetSize(CellKeySheapBuffer *obj) { return obj->size; }

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_BUFFER_H__ */
