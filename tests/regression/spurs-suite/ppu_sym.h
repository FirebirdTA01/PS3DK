/* spurs-ppu-sym row: values shared by the PPU and the SPU task. */
#ifndef SUITE_PPU_SYM_H
#define SUITE_PPU_SYM_H
#include <stdint.h>

#define PS_MAGIC_A 0x50505541u
#define PS_MAGIC_B 0x50505542u

/* the PPU globals the task names; aligned for a 16-byte DMA */
typedef struct ps_target {
    uint32_t magic;
    uint32_t seen;        /* SPU -> PPU: set by the task through the reference */
    uint32_t pad[2];
} __attribute__((aligned(16))) ps_target;

/* argTask u64[0] = box EA */
typedef struct ps_box {
    uint32_t eaA, eaB;    /* SPU -> PPU: CELL_SPURS_PPU_SYM(ps_target_a / _b) */
    uint32_t magicA, magicB;  /* what the task read at those addresses */
    uint32_t state;       /* 2 when done */
    uint32_t pad[3];
} __attribute__((aligned(16))) ps_box;

#endif
