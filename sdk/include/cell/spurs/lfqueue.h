/* cell/spurs/lfqueue.h - CellSpursLFQueue type.
 *
 * Lock-free queue within a SPURS context.  Opaque 128-byte aligned
 * structure matching the CellSyncLFQueue pattern.
 */
#ifndef __PS3DK_CELL_SPURS_LFQUEUE_H__
#define __PS3DK_CELL_SPURS_LFQUEUE_H__

#include <stdint.h>

#define CELL_SPURS_LFQUEUE_ALIGN 128
#define CELL_SPURS_LFQUEUE_SIZE  128

typedef struct CellSpursLFQueue {
    unsigned char skip[CELL_SPURS_LFQUEUE_SIZE];
} __attribute__((aligned(CELL_SPURS_LFQUEUE_ALIGN))) CellSpursLFQueue;

#endif /* __PS3DK_CELL_SPURS_LFQUEUE_H__ */
