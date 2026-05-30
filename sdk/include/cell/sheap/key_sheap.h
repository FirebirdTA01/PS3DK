#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_H__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_H__

#include <stdint.h>
#include <cell/sheap/sheap_base.h>

typedef uint32_t CellSheapKey;

#define CELL_SHEAP_NUM_KEY_ENTRY 256

typedef struct CellKeySheapBarrier { uint64_t ea_ksheap; CellSheapKey key; uint64_t ea; } CellKeySheapBarrier;
typedef struct CellKeySheapBuffer { uint64_t ea_ksheap; CellSheapKey key; uint64_t ea; uint64_t size; } CellKeySheapBuffer;
typedef struct CellKeySheapMutex { uint64_t ea_ksheap; CellSheapKey key; uint64_t ea; } CellKeySheapMutex;
typedef struct CellKeySheapQueue { uint64_t ea_ksheap; CellSheapKey key; uint64_t ea; } CellKeySheapQueue;
typedef struct CellKeySheapRwm { uint64_t ea_ksheap; CellSheapKey key; uint64_t ea; } CellKeySheapRwm;
typedef struct CellKeySheapSemaphore { uint64_t ea_ksheap; CellSheapKey key; uint64_t ea; } CellKeySheapSemaphore;

#ifdef __cplusplus
extern "C" {
#endif

int cellKeySheapInitialize(void *ea_sheap, uint64_t size, uint32_t spu_dma_tag);

#define cellKeySheapAllocate(ea_sheap, size) cellSheapAllocate((ea_sheap), (size))
#define cellKeySheapFree(ea_sheap, ptr) cellSheapFree((ea_sheap), (ptr))
#define cellKeySheapQueryMax(ea_sheap) cellSheapQueryMax(ea_sheap)
#define cellKeySheapQueryFree(ea_sheap) cellSheapQueryFree(ea_sheap)

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SHEAP_KEY_SHEAP_H__ */
