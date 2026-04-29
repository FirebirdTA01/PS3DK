/*! \file cell/sync/barrier.h (SPU variant)
 \brief Forwarder to the SPU-side cellSync* surface.

  Reference SDK splits CellSync* primitives across per-type headers;
  this bridge file lets SPU sources include the dedicated barrier
  header and find the cellSyncBarrier* prototypes that live in our
  combined SPU <cell/sync.h>.
*/

#ifndef PS3TC_SPU_CELL_SYNC_BARRIER_H
#define PS3TC_SPU_CELL_SYNC_BARRIER_H

#include <cell/sync.h>

#endif  /* PS3TC_SPU_CELL_SYNC_BARRIER_H */
