/*! \file cell/dma.h
 \brief Thin SPU-side wrappers around the MFC DMA channels.

  Reference-SDK-compatible cellDma* surface for SPU programs. The
  implementation is a header-only set of inline wrappers over the
  standard <spu_mfcio.h> intrinsics, so there is no companion library
  to link against — every call inlines to one or two channel writes.

  Every wrapper takes the same (ls, ea, size, tag, tid, rid) shape the
  reference headers used:
    - ls   : local-store pointer (or LS address)
    - ea   : 64-bit effective address in main memory
    - size : transfer size in bytes (16 B aligned, ≤ 16 KiB per cmd)
    - tag  : MFC tag id (0..31) used for completion tracking
    - tid  : transfer-class id (0 unless explicitly tagged)
    - rid  : replacement-class id (0 unless explicitly tagged)

  cellDmaWaitTagStatusAll(mask) blocks until every outstanding DMA
  with a tag bit set in `mask` has retired.
*/

#ifndef PS3TC_CELL_DMA_H
#define PS3TC_CELL_DMA_H

#include <stdint.h>
#include <spu_mfcio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DMA-list element. The SPU MFC consumes a packed list of (size,eal)
   pairs; cellDmaListGet/Put take a pointer to an array of these. The
   bit layout matches the reference SDK / mfc_list_element_t in
   <spu_mfcio.h>; we re-typedef it under the cellDma* spelling. */
typedef mfc_list_element_t CellDmaListElement;

static inline void cellDmaGet(volatile void *ls, uint64_t ea, uint32_t size,
                              uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_get(ls, ea, size, tag, tid, rid);
}

static inline void cellDmaPut(volatile void *ls, uint64_t ea, uint32_t size,
                              uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_put(ls, ea, size, tag, tid, rid);
}

static inline void cellDmaPutf(volatile void *ls, uint64_t ea, uint32_t size,
                               uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_putf(ls, ea, size, tag, tid, rid);
}

static inline void cellDmaPutb(volatile void *ls, uint64_t ea, uint32_t size,
                               uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_putb(ls, ea, size, tag, tid, rid);
}

static inline void cellDmaGetf(volatile void *ls, uint64_t ea, uint32_t size,
                               uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_getf(ls, ea, size, tag, tid, rid);
}

static inline void cellDmaGetb(volatile void *ls, uint64_t ea, uint32_t size,
                               uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_getb(ls, ea, size, tag, tid, rid);
}

static inline void cellDmaListGet(volatile void *ls, uint64_t ea,
                                  volatile void *list, uint32_t list_size,
                                  uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_getl(ls, ea, list, list_size, tag, tid, rid);
}

static inline void cellDmaListGetf(volatile void *ls, uint64_t ea,
                                   volatile void *list, uint32_t list_size,
                                   uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_getlf(ls, ea, list, list_size, tag, tid, rid);
}

static inline void cellDmaListGetb(volatile void *ls, uint64_t ea,
                                   volatile void *list, uint32_t list_size,
                                   uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_getlb(ls, ea, list, list_size, tag, tid, rid);
}

static inline void cellDmaListPut(volatile void *ls, uint64_t ea,
                                  volatile void *list, uint32_t list_size,
                                  uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_putl(ls, ea, list, list_size, tag, tid, rid);
}

static inline void cellDmaListPutf(volatile void *ls, uint64_t ea,
                                   volatile void *list, uint32_t list_size,
                                   uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_putlf(ls, ea, list, list_size, tag, tid, rid);
}

static inline void cellDmaListPutb(volatile void *ls, uint64_t ea,
                                   volatile void *list, uint32_t list_size,
                                   uint32_t tag, uint32_t tid, uint32_t rid)
{
	mfc_putlb(ls, ea, list, list_size, tag, tid, rid);
}

static inline uint32_t cellDmaWaitTagStatusAll(uint32_t mask)
{
	mfc_write_tag_mask(mask);
	return mfc_read_tag_status_all();
}

static inline uint32_t cellDmaWaitTagStatusAny(uint32_t mask)
{
	mfc_write_tag_mask(mask);
	return mfc_read_tag_status_any();
}

static inline uint32_t cellDmaWaitTagStatusImmediate(uint32_t mask)
{
	mfc_write_tag_mask(mask);
	return mfc_read_tag_status_immediate();
}

static inline uint32_t cellDmaTagStatusAll(uint32_t mask)
{
	return cellDmaWaitTagStatusAll(mask);
}

static inline uint32_t cellDmaGetTagStatus(void)
{
	return mfc_read_tag_status();
}

static inline uint32_t cellDmaGetUnusedTagStatus(uint32_t mask)
{
	return mfc_stat_cmd_queue() & mask;
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_DMA_H */
