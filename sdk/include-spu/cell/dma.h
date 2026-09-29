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
#include <cellstatus.h>   /* CELL_OK, as callers expect */

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

/* --- Argument checks ---------------------------------------------------
 *
 * Every wrapper below checks its arguments against the MFC's rules before
 * issuing the command.  A failed check halts the SPU (a halt the PPU side
 * sees as a stopped SPU thread) instead of letting the MFC raise a DMA
 * alignment or size error later.  Define NO_CELL_DMA_ASSERT to compile the
 * checks out; define CELL_DMA_ASSERT_VERBOSE to print the failed condition
 * with printf() before halting.
 */
#if defined(NO_CELL_DMA_ASSERT)
#define cellDmaAssert(cond, ...) ((void)0)
#elif defined(CELL_DMA_ASSERT_VERBOSE)
#include <stdio.h>
#define cellDmaAssert(cond, ...)                                              \
	do {                                                                  \
		if (__builtin_expect(!(cond), 0)) {                           \
			printf("cellDma check failed at %s:%d: %s\n",          \
			       __FILE__, __LINE__, #cond);                     \
			spu_hcmpgt(1, 0);                                     \
		}                                                             \
	} while (0)
#else
#define cellDmaAssert(cond, ...) spu_hcmpgt((cond) ? 0 : 1, 0)
#endif

#define __cellDmaLow4(x) ((uint32_t)(uintptr_t)(x) & 0xfu)
#define __cellDmaTagOk(tag) ((uint32_t)(tag) < 32u)

/* A plain command: 1/2/4/8 bytes naturally aligned with LS and EA sharing
   their low four bits, or a 16-byte multiple up to 16KB, both 16-aligned. */
#define cellDmaNormalAssert(ls, ea, size, tag)                                \
	cellDmaAssert(__cellDmaTagOk(tag) && (uint32_t)(size) <= 16384u &&    \
	    ((((uint32_t)(size) & 0xfu) == 0 && __cellDmaLow4(ls) == 0 &&     \
	      __cellDmaLow4(ea) == 0) ||                                     \
	     (((uint32_t)(size) == 1 || (uint32_t)(size) == 2 ||              \
	       (uint32_t)(size) == 4 || (uint32_t)(size) == 8) &&            \
	      __cellDmaLow4(ls) == __cellDmaLow4(ea) &&                      \
	      ((uint32_t)(uint64_t)(ea) & ((uint32_t)(size) - 1)) == 0)))

/* A small command: 1, 2, 4 or 8 bytes, naturally aligned, same low bits. */
#define cellDmaSmallAssert(ls, ea, size, tag)                                 \
	cellDmaAssert(__cellDmaTagOk(tag) &&                                  \
	    ((uint32_t)(size) == 1 || (uint32_t)(size) == 2 ||                \
	     (uint32_t)(size) == 4 || (uint32_t)(size) == 8) &&              \
	    __cellDmaLow4(ls) == __cellDmaLow4(ea) &&                        \
	    ((uint32_t)(uint64_t)(ea) & ((uint32_t)(size) - 1)) == 0)

/* A list command: 8-byte aligned list of at most 2048 elements, 16-byte
   aligned LS target. */
#define cellDmaListAssert(ls, ea, la, lsize, tag)                             \
	cellDmaAssert(__cellDmaTagOk(tag) && __cellDmaLow4(ls) == 0 &&        \
	    ((uint32_t)(uintptr_t)(la) & 7u) == 0 &&                          \
	    ((uint32_t)(lsize) & 7u) == 0 && (uint32_t)(lsize) <= 16384u)

/* Lock-line commands move exactly one 128-byte line. */
#define cellDmaAtomicAssert(ls, ea)                                           \
	cellDmaAssert(((uint32_t)(uintptr_t)(ls) & 0x7fu) == 0 &&             \
	    ((uint32_t)(uint64_t)(ea) & 0x7fu) == 0)
#define cellDmaPutqllucAssert(ls, ea, tag)                                    \
	cellDmaAssert(__cellDmaTagOk(tag) &&                                  \
	    ((uint32_t)(uintptr_t)(ls) & 0x7fu) == 0 &&                       \
	    ((uint32_t)(uint64_t)(ea) & 0x7fu) == 0)

/* A scalar at ea: naturally aligned for its size. */
#define cellDmaDataAssert(ea, size, tag)                                      \
	cellDmaAssert(__cellDmaTagOk(tag) &&                                  \
	    ((uint32_t)(uint64_t)(ea) & ((uint32_t)(size) - 1)) == 0)

/* Large transfers: both addresses 16-byte aligned (the size is split into
   16KB commands and must be a multiple of 16). */
#define cellDmaLargeAssert(ls, ea, tag)                                       \
	cellDmaAssert(__cellDmaTagOk(tag) && __cellDmaLow4(ls) == 0 &&        \
	    __cellDmaLow4(ea) == 0)

/* Unaligned transfers: any size, LS and EA sharing their low four bits. */
#define cellDmaUnalignedAssert(ls, ea, tag)                                   \
	cellDmaAssert(__cellDmaTagOk(tag) &&                                  \
	    __cellDmaLow4(ls) == __cellDmaLow4(ea))

/* The LS address inside the 16-byte buffer `ls` whose low four bits match
   `ea`, i.e. where a small transfer of ea lands in that buffer. */
#define cellDmaEa2Ls(ea, ls) ((uintptr_t)(ls) + __cellDmaLow4(ea))

/* --- Small transfers (1, 2, 4 or 8 bytes) ------------------------------ */

static inline void __cellDmaSmall(uintptr_t ls, uint64_t ea, uint32_t size,
                                  uint32_t tag, uint32_t cmd)
{
	cellDmaSmallAssert(ls, ea, size, tag);
	spu_mfcdma64((void *)ls, mfc_ea2h(ea), mfc_ea2l(ea), size, tag, cmd);
}
#define cellDmaSmallGet(ls, ea, size, tag, tid, rid)                          \
	__cellDmaSmall((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_GET_CMD))
#define cellDmaSmallGetf(ls, ea, size, tag, tid, rid)                         \
	__cellDmaSmall((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_GETF_CMD))
#define cellDmaSmallGetb(ls, ea, size, tag, tid, rid)                         \
	__cellDmaSmall((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_GETB_CMD))
#define cellDmaSmallPut(ls, ea, size, tag, tid, rid)                          \
	__cellDmaSmall((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_PUT_CMD))
#define cellDmaSmallPutf(ls, ea, size, tag, tid, rid)                         \
	__cellDmaSmall((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_PUTF_CMD))
#define cellDmaSmallPutb(ls, ea, size, tag, tid, rid)                         \
	__cellDmaSmall((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_PUTB_CMD))

/* --- Lock-line (atomic) commands --------------------------------------- */

static inline void __cellDmaGetllar(volatile void *ls, uint64_t ea,
                                    uint32_t tid, uint32_t rid)
{
	cellDmaAtomicAssert(ls, ea);
	mfc_getllar(ls, ea, tid, rid);
}
static inline void __cellDmaPutllc(volatile void *ls, uint64_t ea,
                                   uint32_t tid, uint32_t rid)
{
	cellDmaAtomicAssert(ls, ea);
	mfc_putllc(ls, ea, tid, rid);
}
static inline void __cellDmaPutlluc(volatile void *ls, uint64_t ea,
                                    uint32_t tid, uint32_t rid)
{
	cellDmaAtomicAssert(ls, ea);
	mfc_putlluc(ls, ea, tid, rid);
}
static inline void __cellDmaPutqlluc(volatile void *ls, uint64_t ea,
                                     uint32_t tag, uint32_t tid, uint32_t rid)
{
	cellDmaPutqllucAssert(ls, ea, tag);
	mfc_putqlluc(ls, ea, tag, tid, rid);
}
#define cellDmaGetllar(ls, ea, tid, rid)  __cellDmaGetllar((ls), (ea), (tid), (rid))
#define cellDmaPutllc(ls, ea, tid, rid)   __cellDmaPutllc((ls), (ea), (tid), (rid))
#define cellDmaPutlluc(ls, ea, tid, rid)  __cellDmaPutlluc((ls), (ea), (tid), (rid))
#define cellDmaPutqlluc(ls, ea, tag, tid, rid) \
	__cellDmaPutqlluc((ls), (ea), (tag), (tid), (rid))
/* The result of the last getllar/putllc/putlluc (MFC_GETLLAR_STATUS etc.). */
#define cellDmaWaitAtomicStatus() mfc_read_atomic_status()

/* --- Tag-status helpers ------------------------------------------------ */

/* Withdraw a pending tag-status update request (for example one left by
   an "any" or "all" request that was interrupted) and discard a status
   the MFC already posted, so a new request starts clean. */
static inline void cellDmaCancelTagStatusUpdate(void)
{
	spu_writech(MFC_WrTagUpdate, 0);
	while (spu_readchcnt(MFC_WrTagUpdate) == 0)
		;
	if (spu_readchcnt(MFC_RdTagStat) != 0)
		(void)spu_readch(MFC_RdTagStat);
}
static inline uint32_t cellDmaCancelAndWaitTagStatusAny(uint32_t mask)
{
	cellDmaCancelTagStatusUpdate();
	mfc_write_tag_mask(mask);
	return mfc_read_tag_status_any();
}
static inline uint32_t cellDmaCancelAndWaitTagStatusAll(uint32_t mask)
{
	cellDmaCancelTagStatusUpdate();
	mfc_write_tag_mask(mask);
	return mfc_read_tag_status_all();
}

/* --- Extern functions (implemented in libdma.a) --- */

void cellDmaAndWait(uintptr_t ls, uint64_t ea, uint32_t size,
                    uint32_t tag, uint32_t cmd);
void cellDmaLargeCmd(uintptr_t ls, uint64_t ea, uint32_t size,
                     uint32_t tag, uint32_t cmd);
void cellDmaUnalignedCmd(uintptr_t ls, uint64_t ea, uint32_t size,
                         uint32_t tag, uint32_t cmd);

/* --- Large transfers: any 16-byte multiple, split into 16KB commands --- */

static inline void __cellDmaLarge(uintptr_t ls, uint64_t ea, uint32_t size,
                                  uint32_t tag, uint32_t cmd)
{
	cellDmaLargeAssert(ls, ea, tag);
	cellDmaLargeCmd(ls, ea, size, tag, cmd);
}
#define cellDmaLargeGet(ls, ea, size, tag, tid, rid)                          \
	__cellDmaLarge((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_GET_CMD))
#define cellDmaLargeGetf(ls, ea, size, tag, tid, rid)                         \
	__cellDmaLarge((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_GETF_CMD))
#define cellDmaLargeGetb(ls, ea, size, tag, tid, rid)                         \
	__cellDmaLarge((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_GETB_CMD))
#define cellDmaLargePut(ls, ea, size, tag, tid, rid)                          \
	__cellDmaLarge((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_PUT_CMD))
#define cellDmaLargePutf(ls, ea, size, tag, tid, rid)                         \
	__cellDmaLarge((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_PUTF_CMD))
#define cellDmaLargePutb(ls, ea, size, tag, tid, rid)                         \
	__cellDmaLarge((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_PUTB_CMD))

/* --- Unaligned transfers: any size, LS and EA sharing their low bits --- */

static inline void __cellDmaUnaligned(uintptr_t ls, uint64_t ea, uint32_t size,
                                      uint32_t tag, uint32_t cmd)
{
	cellDmaUnalignedAssert(ls, ea, tag);
	cellDmaUnalignedCmd(ls, ea, size, tag, cmd);
}
#define cellDmaUnalignedGet(ls, ea, size, tag, tid, rid)                      \
	__cellDmaUnaligned((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_GET_CMD))
#define cellDmaUnalignedGetf(ls, ea, size, tag, tid, rid)                     \
	__cellDmaUnaligned((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_GETF_CMD))
#define cellDmaUnalignedGetb(ls, ea, size, tag, tid, rid)                     \
	__cellDmaUnaligned((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_GETB_CMD))
#define cellDmaUnalignedPut(ls, ea, size, tag, tid, rid)                      \
	__cellDmaUnaligned((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_PUT_CMD))
#define cellDmaUnalignedPutf(ls, ea, size, tag, tid, rid)                     \
	__cellDmaUnaligned((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_PUTF_CMD))
#define cellDmaUnalignedPutb(ls, ea, size, tag, tid, rid)                     \
	__cellDmaUnaligned((uintptr_t)(ls), (ea), (size), (tag), MFC_CMD_WORD((tid), (rid), MFC_PUTB_CMD))

/* --- Single scalars in main memory --------------------------------------
 *
 * cellDmaGetUintN(ea, tag, tid, rid) reads one naturally aligned N-bit
 * value at ea and returns it; cellDmaPutUintN(value, ea, tag, tid, rid)
 * writes one.  Both go through a 16-byte local buffer at ea's offset and
 * wait for their own tag before returning, so the buffer never outlives the
 * transfer.  cellDmaGetUintTemplate / cellDmaPutUintTemplate take the width
 * (8, 16, 32 or 64) as their first argument.
 */
#define __CELL_DMA_SCALAR(N)                                                  \
static inline uint##N##_t __cellDmaGetUint##N(uint64_t ea, uint32_t tag,      \
                                              uint32_t tid, uint32_t rid)     \
{                                                                             \
	uint8_t buf[16] __attribute__((aligned(16)));                         \
	uintptr_t ls = cellDmaEa2Ls(ea, buf);                                 \
	cellDmaDataAssert(ea, sizeof(uint##N##_t), tag);                      \
	cellDmaAndWait(ls, ea, sizeof(uint##N##_t), tag,                      \
	               MFC_CMD_WORD(tid, rid, MFC_GET_CMD));                  \
	return *(volatile uint##N##_t *)ls;                                   \
}                                                                             \
static inline void __cellDmaPutUint##N(uint##N##_t value, uint64_t ea,        \
                                       uint32_t tag, uint32_t tid,            \
                                       uint32_t rid)                          \
{                                                                             \
	uint8_t buf[16] __attribute__((aligned(16)));                         \
	uintptr_t ls = cellDmaEa2Ls(ea, buf);                                 \
	cellDmaDataAssert(ea, sizeof(uint##N##_t), tag);                      \
	*(volatile uint##N##_t *)ls = value;                                  \
	cellDmaAndWait(ls, ea, sizeof(uint##N##_t), tag,                      \
	               MFC_CMD_WORD(tid, rid, MFC_PUT_CMD));                  \
}
__CELL_DMA_SCALAR(8)
__CELL_DMA_SCALAR(16)
__CELL_DMA_SCALAR(32)
__CELL_DMA_SCALAR(64)
#undef __CELL_DMA_SCALAR

#define cellDmaGetUintTemplate(SIZE, ea, tag, tid, rid)                       \
	__cellDmaGetUint##SIZE((ea), (tag), (tid), (rid))
#define cellDmaPutUintTemplate(SIZE, value, ea, tag, tid, rid)                \
	__cellDmaPutUint##SIZE((uint##SIZE##_t)(value), (ea), (tag), (tid), (rid))
#define cellDmaGetUint8(ea, tag, tid, rid)   cellDmaGetUintTemplate(8, ea, tag, tid, rid)
#define cellDmaGetUint16(ea, tag, tid, rid)  cellDmaGetUintTemplate(16, ea, tag, tid, rid)
#define cellDmaGetUint32(ea, tag, tid, rid)  cellDmaGetUintTemplate(32, ea, tag, tid, rid)
#define cellDmaGetUint64(ea, tag, tid, rid)  cellDmaGetUintTemplate(64, ea, tag, tid, rid)
#define cellDmaPutUint8(value, ea, tag, tid, rid)  cellDmaPutUintTemplate(8, value, ea, tag, tid, rid)
#define cellDmaPutUint16(value, ea, tag, tid, rid) cellDmaPutUintTemplate(16, value, ea, tag, tid, rid)
#define cellDmaPutUint32(value, ea, tag, tid, rid) cellDmaPutUintTemplate(32, value, ea, tag, tid, rid)
#define cellDmaPutUint64(value, ea, tag, tid, rid) cellDmaPutUintTemplate(64, value, ea, tag, tid, rid)

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_DMA_H */
