/* spu-celldma: layout and byte patterns shared by the PPU and SPU sides. */
#ifndef SPU_CELLDMA_VALUES_H
#define SPU_CELLDMA_VALUES_H

#define SRC_BYTES   49152u   /* PPU source, filled with src_byte(i)        */
#define SLOT_BYTES  2048u    /* one unaligned-put case per slot            */
#define SLOTS       20u
#define BIG_BYTES   40960u   /* large-put destination                      */
#define FILL_DST    0xa5u    /* PPU fill of every destination before a run */
#define ATOMIC_ADDS 100u

/* The control block the SPU reads first (128 bytes, big-endian EAs). */
typedef struct celldma_control {
    unsigned long long src, dst, big, small, scalars, line;
    unsigned long long pad[10];
} __attribute__((aligned(128))) celldma_control;

static inline unsigned char src_byte(unsigned i) { return (unsigned char)(i * 37u + 11u); }
static inline unsigned char put_byte(unsigned i) { return (unsigned char)(i * 13u + 5u); }
static inline unsigned char big_byte(unsigned i) { return (unsigned char)(i * 7u + 1u); }

/* Unaligned-put cases: slot k writes put_byte(j) for j in [off, off+size). */
static const unsigned put_off[SLOTS]  = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 1, 3, 8, 15 };
static const unsigned put_size[SLOTS] = { 1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 21, 31, 33, 100, 1000, 1500, 1, 29, 8, 1024 };

/* Scalars written by cellDmaPutUintN, at these offsets of `scalars`. */
#define SCALAR64 0x0123456789abcdefull
#define SCALAR32 0x89abcdefu
#define SCALAR16 0xbeefu
#define SCALAR8  0x5au
#define SCALAR64_AT 0
#define SCALAR32_AT 8
#define SCALAR16_AT 12
#define SCALAR8_AT  15

/* Lock-line words the atomic commands leave. */
#define LINE_LLUC  0xc0ffee01u   /* word 1, written by cellDmaPutlluc  */
#define LINE_QLLUC 0xc0ffee02u   /* word 2, written by cellDmaPutqlluc */

#endif
