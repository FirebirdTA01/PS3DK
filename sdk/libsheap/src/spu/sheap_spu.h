/* sheap_spu.h -- internal interfaces of the SPU libsheap.
 *
 * Only the 18 cellSheap and cellKeySheap functions are public; everything
 * here uses the reserved __sheap_ prefix.  The library is not reentrant:
 * each call finishes its DMA before returning and the LS buffers below are
 * shared by consecutive calls of one SPU program.
 */
#ifndef PS3DK_SHEAP_SPU_H
#define PS3DK_SHEAP_SPU_H

#include <stdint.h>
#include <cell/error.h>
#include <cell/sheap/error.h>
#include <cell/sheap/sheap_types.h>
#include "../sheap_layout.h"
#include "../core/sheap_core.h"

/* Keep the compiler from moving LS accesses across a DMA command. */
#define SHEAP_DMA_BARRIER() __asm__ __volatile__("" ::: "memory")

/* ---- header line (lockline.c) ------------------------------------- */

/* Take the heap lock (header word 0) and leave a snapshot of the header
 * in *line.  Spins while another SPU or the PPU holds it. */
void __sheap_lock(uint64_t ea, sheap_header *line);
/* Release: write the snapshot back with the lock word cleared. */
void __sheap_unlock(uint64_t ea, sheap_header *line);
/* Atomic snapshot of the header without taking the lock. */
void __sheap_fetch(uint64_t ea, sheap_header *line);
/* Write a whole header line unconditionally (Initialize). */
void __sheap_publish(uint64_t ea, sheap_header *line);

/* Zero `size` bytes (a multiple of 128) at a 128-aligned ea, then wait. */
void __sheap_dma_zero(uint64_t ea, uint64_t size, unsigned tag);
void __sheap_dma_wait(unsigned tag);

/* ---- node-state tree cache (linecache.c) -------------------------- */

/* Start a locked operation on the tree at ea_tree: forget every cached
 * line (another processor may have changed them since the last lock). */
void __sheap_cache_begin(uint64_t ea_tree, unsigned tag);
/* Write every modified line back and wait; call before unlocking. */
void __sheap_cache_flush(void);
extern const sheap_tree_access __sheap_cache_access;

/* ---- key table (keytable.c) --------------------------------------- */

/* Each works on the entry for `key` of the keyed heap whose header
 * snapshot is *h, with a line reservation on the entry's 128-byte line. */
uint64_t __sheap_key_new_object(const sheap_header *h, CellSheapKey key);
int __sheap_key_finalize_new(const sheap_header *h, CellSheapKey key,
                             uint64_t object);
int __sheap_key_delete_object(const sheap_header *h, CellSheapKey key,
                              uint64_t *object);
int __sheap_key_finalize_delete(const sheap_header *h, CellSheapKey key);

#endif /* PS3DK_SHEAP_SPU_H */
