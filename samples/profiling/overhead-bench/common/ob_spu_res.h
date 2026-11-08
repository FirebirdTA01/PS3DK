/* overhead-bench SPU reply block (shared by the PPU binding and the SPU program).  The PPU passes the EA of one
 * 128-byte aligned ob_spu_res as arg3; for each reply the SPU PUTs words w[0..15] and then, with a fenced PUT on the
 * SAME tag (OB_BENCH_TAG), the reply number in seq: the fence orders the seq PUT after the words PUT (same-tag
 * putf, the PRIMITIVES r3 rule), so a PPU that reads seq == n and then (after its own lwsync) the words reads reply
 * n's words.  The PPU polls seq outside every timed block.  OB_BENCH_TAG differs from the writer's tag
 * (OB_WRITER_TAG): ob_owner_check verifies that before the first command.
 * Reply words: w[0..7] the block's (d0, d1, rc, sseq, tail_lost, staged, published, lost), or at startup (attach rc,
 * dec_write rc), or at exit (fini rc); w[8] the command word this reply answers (identity; 0 at startup and exit);
 * w[9] 1 when a decrementer write that returned success happened before this reply's block, else 0. */
#ifndef OB_SPU_RES_H
#define OB_SPU_RES_H

#include <stdint.h>

#define OB_WRITER_TAG 3u
#define OB_BENCH_TAG 5u

typedef struct ob_spu_res {
    uint32_t w[16];
    uint32_t pad[15];
    uint32_t seq;
} __attribute__((aligned(128))) ob_spu_res;

#if defined(__cplusplus)
static_assert(sizeof(ob_spu_res) == 128, "ob_spu_res size");
#else
_Static_assert(sizeof(ob_spu_res) == 128, "ob_spu_res size");
#endif

#define OB_SPU_EXIT 0u
/* (seq << 16) | (case << 8) | tu; seq 1..65535, so a command word is never OB_SPU_EXIT */
#define OB_SPU_CMD(seq, c, tu) ((((uint32_t)(seq)&0xFFFFu) << 16) | (((uint32_t)(c)&0xFFu) << 8) | ((uint32_t)(tu)&0xFFu))

#endif
