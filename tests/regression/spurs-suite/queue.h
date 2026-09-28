/* spurs-suite queue: task kinds, result slots and the shared queue block. */
#ifndef SPURS_SUITE_QUEUE_H
#define SPURS_SUITE_QUEUE_H

#include "common.h"

/* task kinds (argTask u32[3]); each reports in the slot of the same number */
#define Q_API               0   /* SPU-initialized queue: every call and error path */
#define Q_PERM              1   /* direction checks on the PPU-facing queues */
#define Q_SPU2SPU_CONSUMER  2
#define Q_SPU2SPU_PRODUCER  3
#define Q_SPU2PPU_PRODUCER  4
#define Q_PPU2SPU_CONSUMER  5
#define Q_SLOTS             6

/* queue block (argTask u32[2]): four 128-byte queues, then their rings */
#define Q_SPU2PPU    0          /* depth 4, PPU pops */
#define Q_PPU2SPU    1          /* depth 4, PPU pushes */
#define Q_SPU2SPU    2          /* depth 2, two tasks */
#define Q_SPU_INIT   3          /* depth 2, initialized by the Q_API task */
#define Q_COUNT      4
#define Q_ENTRY      16
#define Q_RING_BYTES (8 * Q_ENTRY)
#define Q_QUEUE_EA(base, i) ((base) + (i) * 128u)
#define Q_RING_EA(base, i)  ((base) + Q_COUNT * 128u + (i) * Q_RING_BYTES)

#define Q_ROUNDS     12         /* entries per streaming direction */
#define Q_SEQ_MAGIC  0x51000000u

typedef struct q_entry {
    unsigned int seq, magic, producer, pad;
} __attribute__((aligned(16))) q_entry;

#endif
