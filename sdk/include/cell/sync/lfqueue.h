/*! \file cell/sync/lfqueue.h
 \brief Forwarder for the cellSync lock-free queue.

  The reference SDK splits CellSync* primitives across per-type headers;
  our SDK carries the CellSyncLFQueue type and the cellSyncLFQueue*
  functions in <cell/sync.h>.  This header bridges the include path.
*/

#ifndef PS3TC_CELL_SYNC_LFQUEUE_H
#define PS3TC_CELL_SYNC_LFQUEUE_H

/* An SPU compile finds the SPU tree's header of this name next on the
 * include path (the in-tree SPU libraries list this PPU tree first). */
#ifdef __SPU__
#include_next <cell/sync/lfqueue.h>
#else
#include <cell/sync.h>
#endif

#endif  /* PS3TC_CELL_SYNC_LFQUEUE_H */
