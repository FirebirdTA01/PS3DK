/*! \file cell/sync.h (SPU variant)
 \brief SPU-side declarations for the cellSync user-space primitives.

  The PPU surface in <cell/sync.h> takes pointers to LS-resident
  primitives because the PPU has direct memory access. The SPU side
  takes a 64-bit effective address: the primitive lives in main
  memory and SPU operations DMA it in/out atomically with
  getllar/putllc.

  The function bodies are emitted by an SPU companion to the PPU
  libsync_stub. Header here just declares the externs; consumers
  link with `-lsync_spu` (when that stub ships) — see the SPU
  libsync TODO in docs/known-issues.md.
*/

#ifndef PS3TC_SPU_CELL_SYNC_H
#define PS3TC_SPU_CELL_SYNC_H

#include <stdint.h>
#include <cell/error.h>
#include <cell/sync/lfqueue.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes are spelled identically on PPU and SPU.
 * AGAIN..BUSY are also in cell/sync/error.h; guard for idempotency. */
#ifndef CELL_SYNC_ERROR_AGAIN
#define CELL_SYNC_ERROR_AGAIN         0x80410101
#define CELL_SYNC_ERROR_INVAL         0x80410102
#define CELL_SYNC_ERROR_NOSYS         0x80410103
#define CELL_SYNC_ERROR_NOMEM         0x80410104
#define CELL_SYNC_ERROR_SRCH          0x80410105
#define CELL_SYNC_ERROR_NOENT         0x80410106
#define CELL_SYNC_ERROR_NOEXEC        0x80410107
#define CELL_SYNC_ERROR_DEADLK        0x80410108
#define CELL_SYNC_ERROR_PERM          0x80410109
#define CELL_SYNC_ERROR_BUSY          0x8041010a
#endif
#define CELL_SYNC_ERROR_ABORT         0x8041010c
#define CELL_SYNC_ERROR_FAULT         0x8041010d
#define CELL_SYNC_ERROR_CHILD         0x8041010e
#define CELL_SYNC_ERROR_STAT          0x8041010f
#define CELL_SYNC_ERROR_ALIGN         0x80410110
#define CELL_SYNC_ERROR_NULL_POINTER  0x80410111
#define CELL_SYNC_ERROR_SHOTAGE       0x80410112
#define CELL_SYNC_ERROR_UNKNOWNKEY    0x80410113

#define CELL_SYNC_LFQUEUE_TERMINATED  2

/* ---- Mutex (ticket-fair) -------------------------------------------- */

int cellSyncMutexInitialize(uint64_t ea_mutex, unsigned int tag);
int cellSyncMutexLock      (uint64_t ea_mutex);
int cellSyncMutexTryLock   (uint64_t ea_mutex);
int cellSyncMutexUnlock    (uint64_t ea_mutex);

/* ---- Barrier (N-way rendezvous) ------------------------------------- */

int cellSyncBarrierInitialize(uint64_t ea_barrier, uint16_t count,
                              unsigned int tag);
int cellSyncBarrierNotify    (uint64_t ea_barrier);
int cellSyncBarrierTryNotify (uint64_t ea_barrier);
int cellSyncBarrierWait      (uint64_t ea_barrier);
int cellSyncBarrierTryWait   (uint64_t ea_barrier);

/* ---- Rwm (read/write lock with attached buffer) --------------------- */

int cellSyncRwmInitialize(uint64_t ea_rwm, uint64_t buffer_ea,
                          uint32_t buffer_size, unsigned int tag);
int cellSyncRwmReadBegin   (uint64_t ea_rwm, void *ls_buffer,
                            unsigned int tag);
int cellSyncRwmTryReadBegin(uint64_t ea_rwm, void *ls_buffer,
                            unsigned int tag);
int cellSyncRwmReadEnd     (uint64_t ea_rwm, unsigned int tag);
int cellSyncRwmWrite       (uint64_t ea_rwm, void *ls_buffer,
                            unsigned int tag);
int cellSyncRwmTryWrite    (uint64_t ea_rwm, void *ls_buffer,
                            unsigned int tag);

/* ---- Queue (fixed-size FIFO) ---------------------------------------- */

int          cellSyncQueueInitialize(uint64_t ea_queue, uint64_t ptr_buffer,
                                     uint32_t buffer_size, unsigned int depth,
                                     unsigned int tag);
int          cellSyncQueuePush   (uint64_t ea_queue, const void *ls_buffer);
int          cellSyncQueueTryPush(uint64_t ea_queue, const void *ls_buffer);
int          cellSyncQueuePop    (uint64_t ea_queue, void *ls_buffer);
int          cellSyncQueueTryPop (uint64_t ea_queue, void *ls_buffer);
int          cellSyncQueuePeek   (uint64_t ea_queue, void *ls_buffer);
int          cellSyncQueueTryPeek(uint64_t ea_queue, void *ls_buffer);
int          cellSyncQueueClear  (uint64_t ea_queue);
unsigned int cellSyncQueueSize   (uint64_t ea_queue);

/* ---- Lock-free queue (PPU<->SPU) ------------------------------------
 * Types and prototypes are in <cell/sync/lfqueue.h> which is included
 * at the top of this umbrella.
 */

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SPU_CELL_SYNC_H */
