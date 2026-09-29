#ifndef __PS3DK_CELL_SHEAP_KEY_SHEAP_H__
#define __PS3DK_CELL_SHEAP_KEY_SHEAP_H__
#include <cellstatus.h>   /* CELL_OK */

#include <stdint.h>
#include <cell/sheap/sheap_base.h>
#include <cell/sheap/sheap_types.h>

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
