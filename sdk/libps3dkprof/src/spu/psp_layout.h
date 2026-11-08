/* SPU lane transport: the layout shared by the SPU producer and the PPU drainer (design p3.2 r3, section B).
 * Portable C: no SPU or PPU intrinsics.  All control fields and slots are big-endian, the native order of both the
 * PPU and the SPU; the helpers below encode them explicitly so the host model stores the same bytes. */
#ifndef PSP_LAYOUT_H
#define PSP_LAYOUT_H

#include <stdint.h>

/* ---- slots: 8 bytes, byte 6 is the discriminator ---- */
#define PSP_SLOT 8u
#define PSP_K_BEGIN 1u
#define PSP_K_END 2u
#define PSP_K_MARK 3u
#define PSP_K_LOSS 0x80u  /* bytes 0-3 count 1..0xFFFFFFFF */
#define PSP_K_EPOCH 0x81u /* bytes 0-3 the decrementer value written */
#define PSP_K_PAD 0x82u
#define PSP_K_CAL 0x83u   /* bytes 0-3 decrementer value, bytes 4-5 request id 1..65535 */
#define PSP_LOSS_MAX 0xFFFFFFFFu
#define PSP_MAX_DEPTH 64u /* depths 0..63 are recorded; a BEGIN at depth 64 is refused */

/* ---- control block: 256 bytes, 128-aligned; line A at 0 (PPU writes), line B at 128 (SPU writes) ---- */
#define PSP_CTL_BYTES 256u
#define PSP_A_READ_IDX 0u        /* u64, even, monotonic */
#define PSP_A_CAL_REQ 8u         /* u16, 0 none; strictly increasing per lane */
#define PSP_A_CAL_CANCEL 10u     /* u16, the last cancelled id */
#define PSP_B 128u
#define PSP_B_GEN 128u           /* u64, odd; +2 per publication */
#define PSP_B_WRITE_IDX 136u     /* u64, even, monotonic */
#define PSP_B_NEXT_SEQ 144u      /* u64, <= PSP_SEQ_MAX */
#define PSP_B_OLDER_LOST 152u    /* u64, the unpublished suffix; a final range only when state = CLOSED */
#define PSP_B_REFUSED 160u       /* u64 */
#define PSP_B_REFUSED_CALLS 168u /* u64 */
#define PSP_B_STATE 176u         /* u32 */
#define PSP_B_FLAGS 180u         /* u32 */
#define PSP_B_CAL_ANSWERED 184u  /* u16 */
#define PSP_B_EPOCHS_WRITTEN 188u /* u32, completed decrementer interventions */
#define PSP_B_GEN_END 248u       /* u64, equal to gen in a consistent version */

#define PSP_ST_NONE 0u
#define PSP_ST_STARTED 1u
#define PSP_ST_CLOSED 2u

#define PSP_F_OVERFLOWED 1u
#define PSP_F_OWN_DEC 2u
#define PSP_F_FAULT 4u
#define PSP_F_GEN_EXHAUSTED 8u
#define PSP_F_KNOWN 0xFu

/* ---- checked boundaries (r3 B.3) ---- */
#define PSP_SEQ_MAX (1ull << 62)
#define PSP_GEN_MAX ((1ull << 63) - 1u) /* the last generation; it is reserved for CLOSED */
#define PSP_W_MAX (1ull << 63)
#define PSP_REQ_MAX 65535u

static inline void psp_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void psp_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void psp_be64(uint8_t *p, uint64_t v) { psp_be32(p, (uint32_t)(v >> 32)); psp_be32(p + 4, (uint32_t)v); }
static inline uint16_t psp_rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t psp_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t psp_rd64(const uint8_t *p) { return ((uint64_t)psp_rd32(p) << 32) | psp_rd32(p + 4); }

static inline void psp_slot(uint8_t *s, uint32_t w0, uint16_t w1, uint8_t kind, uint8_t b7)
{
    psp_be32(s, w0);
    psp_be16(s + 4, w1);
    s[6] = kind;
    s[7] = b7;
}

#endif
