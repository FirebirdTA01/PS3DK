/* spurs-suite lock-free queue: task kinds, result slots and the queue block. */
#ifndef SPURS_SUITE_LFQUEUE_H
#define SPURS_SUITE_LFQUEUE_H

#include "common.h"

/* task kinds (argTask u32[3]); each reports in the slot of the same number */
#define LQ_API               0  /* SPU-initialized queue: calls and error paths */
#define LQ_SPU2SPU_CONSUMER  1
#define LQ_SPU2SPU_PRODUCER  2
#define LQ_SPU2PPU_PRODUCER  3
#define LQ_PPU2SPU_CONSUMER  4
#define LQ_SLOTS             5

/* queue block (argTask u32[2]): four 128-byte queues, then their rings */
#define LQ_SPU2PPU    0         /* depth 4, PPU pops */
#define LQ_PPU2SPU    1         /* depth 4, PPU pushes */
#define LQ_SPU2SPU    2         /* depth 2, two tasks */
#define LQ_SPU_INIT   3         /* depth 2, initialized by the LQ_API task */
#define LQ_COUNT      4
#define LQ_ENTRY      16
#define LQ_RING_BYTES (8 * LQ_ENTRY)
#define LQ_QUEUE_EA(base, i) ((base) + (i) * 128u)
#define LQ_RING_EA(base, i)  ((base) + LQ_COUNT * 128u + (i) * LQ_RING_BYTES)

#define LQ_ROUNDS     12        /* entries per streaming direction */
#define LQ_SEQ_MAGIC  0x4c000000u

typedef struct lq_entry {
    unsigned int seq, magic, producer, pad;
} __attribute__((aligned(16))) lq_entry;

#endif
