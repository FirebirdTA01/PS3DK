/* cell/sheap/key_sheap.h -- SPU keyed shared heap.
 *
 * A keyed heap is a shared heap with a 256-entry key table in front of
 * it.  The keyed object types (key_sheap_*.h) look an object up by key:
 * the first New for a key creates and initialises the object, later ones
 * attach to it, and the last Delete frees it.  Sharing works while the
 * users' lifetimes overlap; once every reference is gone, the next New
 * creates a fresh object.  The plain heap functions work on a keyed heap
 * unchanged, so the Allocate/Free/Query forms below are aliases.
 */
#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_H_SPU__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_H_SPU__

#include <stdint.h>
#include <cell/sheap/sheap_base.h>
#include <cell/sheap/sheap_types.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapInitialize(uint64_t ea_ksheap, uint64_t size, uint32_t spu_dma_tag);

#define cellKeySheapAllocate(ea_ksheap, size) cellSheapAllocate((ea_ksheap), (size))
#define cellKeySheapFree(ea_ksheap, ptr)      cellSheapFree((ea_ksheap), (ptr))
#define cellKeySheapQueryMax(ea_ksheap)       cellSheapQueryMax(ea_ksheap)
#define cellKeySheapQueryFree(ea_ksheap)      cellSheapQueryFree(ea_ksheap)

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_H_SPU__ */
