/*! \file cell/sync/barrier.h
 \brief Forwarder for the CellSyncBarrier surface.

  The reference SDK splits CellSync* primitives across per-type
  headers (cell/sync/barrier.h, cell/sync/mutex.h, cell/sync/queue.h,
  …) and re-exports the lot through <cell/sync.h>.  Our SDK ships
  the surface in a single <cell/sync.h>; this header bridges the
  per-type include path so source code that includes the dedicated
  barrier header continues to compile.
*/

#ifndef PS3TC_CELL_SYNC_BARRIER_H
#define PS3TC_CELL_SYNC_BARRIER_H

#include <cell/sync.h>

#endif  /* PS3TC_CELL_SYNC_BARRIER_H */
