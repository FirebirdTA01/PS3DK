/* spurs-suite: definitions shared by every row's PPU and SPU sides. */
#ifndef SPURS_SUITE_COMMON_H
#define SPURS_SUITE_COMMON_H

#define RESULT_MAGIC 0xc0de0000u

/* One 16-byte result slot per SPU program instance: magic | kind, status,
 * value, extra.  A program writes it with one DMA put when it finishes a
 * step, so the PPU can tell how far it got. */
typedef struct result_slot {
    unsigned int magic, status, value, extra;
} __attribute__((aligned(16))) result_slot;

#endif
