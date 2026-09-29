/*! \file cell/sync2.h
 \brief cellSync2 thread-pluggable sync primitives (PPU umbrella).

  The PPU functions are the libsync2 module's (link -lsync2_stub); the
  headers follow the reference layout under cell/sync2/, which sources may
  include directly.

  cellSync2 vs cellSync: sync2 takes a CellSync2ThreadConfig* on every
  blocking call so it can plug in custom self() / waitSignal() /
  sendSignal() callbacks for PPU threads, PPU fibers, SPURS tasks, or
  user-defined blockable threads. Each primitive is a fixed 128-byte
  opaque struct plus a separately allocated waiting-queue buffer the
  caller supplies via cellSync2*EstimateBufferSize().
*/
#ifndef __PSL1GHT_CELL_SYNC2_H__
#define __PSL1GHT_CELL_SYNC2_H__

#include <cell/sync2/types.h>
#include <cell/sync2/version.h>
#include <cell/sync2/error.h>
#include <cell/sync2/thread_types.h>
#include <cell/sync2/thread.h>
#include <cell/sync2/mutex.h>
#include <cell/sync2/cond.h>
#include <cell/sync2/queue.h>
#include <cell/sync2/semaphore.h>

#endif /* __PSL1GHT_CELL_SYNC2_H__ */
