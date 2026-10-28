/*! \file cell/sync/error.h
 \brief Forwarder for the cellSync error codes.

  The platform ABI ships the cellSync error constants under a per-type
  header (cell/sync/error.h, cell/sync/mutex.h, ...).  Our SDK carries the
  whole surface — types, functions and CELL_SYNC_ERROR_* codes — in
  <cell/sync.h>; this header bridges the include path.
*/

#ifndef PS3TC_CELL_SYNC_ERROR_H
#define PS3TC_CELL_SYNC_ERROR_H

/* An SPU compile finds the SPU tree's header of this name next on the
 * include path (the in-tree SPU libraries list this PPU tree first). */
#ifdef __SPU__
#include_next <cell/sync/error.h>
#else
#include <cell/sync.h>
#endif

#endif  /* PS3TC_CELL_SYNC_ERROR_H */
