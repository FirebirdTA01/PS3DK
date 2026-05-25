/* cell/sync2.h -- SPU-side sync2 declarations.
 *
 * The SPU sync2 API surface is split across sub-headers under
 * cell/sync2/ (cond.h, mutex.h, queue.h, semaphore.h, thread.h).
 * These sub-headers are not yet shipped.
 *
 * SPU code that needs sync2 entry points should include the
 * individual sub-headers directly or provide its own prototypes
 * until the full surface is available.  The libsync2.a archive
 * resolves -lsync2 at link time but currently contains no
 * implementations.
 */
#ifndef __CELL_SYNC2_H_SPU__
#define __CELL_SYNC2_H_SPU__

/* Sub-headers (not yet shipped):
 *   cell/sync2/cond.h
 *   cell/sync2/mutex.h
 *   cell/sync2/queue.h
 *   cell/sync2/semaphore.h
 *   cell/sync2/thread.h
 */

#endif /* __CELL_SYNC2_H_SPU__ */
