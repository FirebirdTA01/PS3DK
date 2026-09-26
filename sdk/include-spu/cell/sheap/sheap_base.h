/* cell/sheap/sheap_base.h -- SPU shared heap (libsheap.a).
 *
 * A shared heap is a region of main memory, 128-byte aligned, that PPU
 * and SPU code allocate from concurrently.  The heap is named by its
 * effective address; blocks are 128-byte aligned powers of two.  A heap
 * can be initialised on either processor (the PPU side is the firmware's
 * cellSheap), and the two sides then share one lock and one allocation
 * tree.  The DMA tag given at initialisation is used for all SPU traffic
 * to the heap.  The queries return int, so sizes of 2 GB and above are
 * truncated.
 *
 * Link with -lsheap -lsync -ldma.
 */
#ifndef __PS3DK_CELL_SHEAP_BASE_H_SPU__
#define __PS3DK_CELL_SHEAP_BASE_H_SPU__

#include <stdint.h>
#include <cell/sheap/error.h>

#define CELL_SHEAP_MIN_HEAP_SIZE  (2 * 1024)
#define CELL_SHEAP_MIN_BLOCK_SIZE 128

#ifdef __cplusplus
extern "C" {
#endif

int cellSheapInitialize(uint64_t ea_sheap, uint64_t size, uint32_t spu_dma_tag);
uint64_t cellSheapAllocate(uint64_t ea_sheap, uint64_t size);
int cellSheapFree(uint64_t ea_sheap, uint64_t ptr);
int cellSheapQueryMax(uint64_t ea_sheap);
int cellSheapQueryFree(uint64_t ea_sheap);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SHEAP_BASE_H_SPU__ */
