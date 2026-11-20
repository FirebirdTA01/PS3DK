/* RSX frame capture units on the host: the command-stream decoder (rsxf_decode.c) and the chunk/record encoders and
 * canonical hash (rsxf_wire.c). Writes unit-sample.bin, the RSXF chunk payloads of one capture, for rsxf-test.py to
 * wrap into a capture and hand to the reader. Every check is an assert (the driver requires exit 0). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsxf.h"
#include "auto_ring_commands.h"

#define M(sub, m, n) (((uint32_t)(n) << 18) | ((uint32_t)(sub) << 13) | (m))
#define NI 0x40000000u

static rsxf_decoder d;
static int sunk;
static void count_event(void *user, const rsxf_draw_event *e)
{
    assert(e->kind == RSXF_K_CLEAR && e->clear_mask == 0xf0);
    (*(int *)user)++;
}

static void decode(const uint32_t *w, size_t n, int ok)
{
    assert(ps3dk_prof__rsxf_decode(&d, w, n) == ok);
}

static void vp_upload_and_start(void)
{
    /* upload 3 instructions at slot 10 (both FROM_ID and START with one packet), the last with word 3 bit 0 */
    const uint32_t w[] = {M(0, 0x1e9c, 2), 10, 10, M(0, 0x0b80, 12), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x13};
    ps3dk_prof__rsxf_decode_init(&d);
    decode(w, sizeof w / 4, 1);
    assert(d.known & 1u);
    assert(d.vp_start == 10 && d.vp_upload == 13 && d.vp_word == 0);
    assert(d.vp_known[10] && d.vp_known[11] && d.vp_known[12] && !d.vp_known[13]);
    assert(d.vp[12][3] == 0x13 && d.vp[10][0] == 1);
    {
        rsxf_hash h;
        ps3dk_prof__rsxf_vp_hash(&d.vp[10][0], RSXF_VP_SLOTS - 10, &h);
        assert(h.instructions == 3 && !h.unterminated && h.bytes == 48);
    }
}

static void draws_and_clears(void)
{
    const uint32_t w[] = {
        M(0, 0x08e4, 1), 0x0e120941u,                    /* FP_ADDRESS */
        M(0, 0x1808, 1), 5,                              /* BEGIN triangles */
        M(0, 0x1814, 2) | NI, (255u << 24) | 0, (35u << 24) | 256, /* two batches: 256 + 36 vertices from 0 */
        M(0, 0x1808, 1), 0,                              /* END */
        M(0, 0x181c, 2), 0x4000, (1u << 4) | 1u,         /* index offset, dma: u16, main */
        M(0, 0x1808, 1), 4,
        M(0, 0x1824, 1) | NI, (5u << 24) | 3,            /* 6 indices from 3 */
        M(0, 0x1808, 1), 0,
        M(0, 0x1d94, 1), 0xf3,                           /* clear */
        M(0, 0x1808, 1), 6,
        M(0, 0x1818, 3) | NI, 1, 2, 3,                   /* inline vertex data */
        M(0, 0x1808, 1), 0,
        M(0, 0x1808, 1), 1,
        M(0, 0x180c, 2) | NI, 0x00010000, 0x00030002,    /* inline u16 indices: 4 */
        M(0, 0x1808, 1), 0,
        M(0, 0x1808, 1), 2,
        M(0, 0x1880, 4), 0, 0, 0, 0,                     /* immediate attribute data */
        M(0, 0x1808, 1), 0,
        M(3, 0x1808, 1), 5,                              /* another subchannel: skipped */
    };
    ps3dk_prof__rsxf_decode_init(&d);
    decode(w, sizeof w / 4, 1);
    assert((d.known & 2u) && d.fp_address == 0x0e120941u);
    assert(d.n_events == 6 && !d.lost_events);
    assert(d.events[0].kind == RSXF_K_ARRAY && d.events[0].mode == 5 && d.events[0].first == 0 &&
           d.events[0].count == 292);
    assert(d.events[1].kind == RSXF_K_INDEX && d.events[1].count == 6 && d.events[1].first == 3 &&
           d.events[1].index_offset == 0x4000 && d.events[1].index_type == 1 && d.events[1].index_location == 1);
    assert(d.events[2].kind == RSXF_K_CLEAR && d.events[2].clear_mask == 0xf3);
    assert(d.events[3].kind == RSXF_K_INLINE_ARRAY && d.events[3].words == 3);
    assert(d.events[4].kind == RSXF_K_INLINE_INDEX && d.events[4].count == 4 && d.events[4].index_type == 1);
    assert(d.events[5].kind == RSXF_K_IMMEDIATE);
}

static void rejections_forget_and_recover(void)
{
    const uint32_t set[] = {M(0, 0x08e4, 1), 0x40u, M(0, 0x1ea0, 1), 7};
    const uint32_t jump[] = {0x20001000u};
    const uint32_t call[] = {0x00001002u};
    const uint32_t reserved[] = {0x80000000u | M(0, 0x100, 1), 0};
    const uint32_t truncated[] = {M(0, 0x08e4, 4), 1};
    const uint32_t range[] = {M(0, 0x1ffc, 2), 1, 2};
    ps3dk_prof__rsxf_decode_init(&d);
    decode(set, 4, 1);
    assert((d.known & 3u) == 3u);
    decode(jump, 1, 0);
    assert(d.known == 0 && d.bad_reason == AUTO_RING_REJECT_JUMP && d.bad_header == 0x20001000u && d.resets == 1);
    decode(set, 2, 1);
    assert(d.known == 2u); /* FP_ADDRESS written again: known again; the VP start is still unknown */
    decode(call, 1, 0);
    decode(reserved, 2, 0);
    decode(truncated, 2, 0);
    decode(range, 3, 0);
    /* each rejection resets; the words after a rejected header are read as headers again (the range packet's two
     * payload words are themselves a JUMP and a CALL), and the first rejection is kept */
    assert(d.bad_reason == AUTO_RING_REJECT_JUMP && d.bad_header == 0x20001000u && d.resets == 7);
    {
        /* a group open across a rejection is dropped (its draw cannot be trusted) */
        const uint32_t open[] = {M(0, 0x1808, 1), 5, M(0, 0x1814, 1), 0};
        const uint32_t close[] = {M(0, 0x1808, 1), 0};
        d.n_events = 0;
        decode(open, 4, 1);
        decode(jump, 1, 0);
        decode(close, 2, 1);
        assert(d.n_events == 0);
    }
    {
        /* more events than the buffer holds between two scans are counted, not overwritten */
        uint32_t clears[2 * 20];
        for (int i = 0; i < 20; i++) {
            clears[2 * i] = M(0, 0x1d94, 1);
            clears[2 * i + 1] = 0xf0;
        }
        d.n_events = d.lost_events = 0;
        decode(clears, 40, 1);
        assert(d.n_events == 16 && d.lost_events == 4);
        /* with a sink (the runtime's) every event goes out at once: nothing is buffered or lost */
        d.n_events = d.lost_events = 0;
        d.sink = count_event;
        d.sink_user = &sunk;
        decode(clears, 40, 1);
        assert(sunk == 20 && d.n_events == 0 && d.lost_events == 0);
        d.sink = NULL;
    }
}

/* The 3D register shadow: plain methods stored with KNOWN and DIRTY bits, data ports and triggers not; a rejection
 * clears every KNOWN bit and counts the reset. */
static void register_shadow(void)
{
    const uint32_t w[] = {M(0, 0x310, 2), 1, 0x03020302,       /* BLEND_ENABLE, BLEND_FUNC_SRC (incrementing) */
                          M(0, 0x1d94, 1) | NI, 0xf3,           /* CLEAR_BUFFERS: a trigger, not a register */
                          M(0, 0x0b80, 4), 1, 2, 3, 4,          /* VP instruction port */
                          M(0, 0x1f00, 1), 7,                   /* VP constant port */
                          M(0, 0x1a00, 2) | NI, 0x100, 0x200,   /* TEX_OFFSET(0) twice (non-incrementing) */
                          M(2, 0x0320, 1), 9};                  /* another subchannel: not this object */
    uint32_t cleared;
    ps3dk_prof__rsxf_decode_init(&d);
    decode(w, sizeof w / 4, 1);
#define KNOWN(m) ((d.reg_known[((m) >> 2) >> 5] >> (((m) >> 2) & 31u)) & 1u)
#define DIRTY(m) ((d.reg_dirty[((m) >> 2) >> 5] >> (((m) >> 2) & 31u)) & 1u)
    assert(KNOWN(0x310) && d.reg[0x310 >> 2] == 1 && KNOWN(0x314) && d.reg[0x314 >> 2] == 0x03020302);
    assert(DIRTY(0x310) && DIRTY(0x314));
    assert(KNOWN(0x1a00) && d.reg[0x1a00 >> 2] == 0x200); /* the last value written */
    assert(!KNOWN(0x1d94) && !KNOWN(0x0b80) && !KNOWN(0x0b8c) && !KNOWN(0x1f00) && !KNOWN(0x320));
    assert(!ps3dk_prof__rsxf_shadowed(0x1808) && !ps3dk_prof__rsxf_shadowed(0x1814) &&
           !ps3dk_prof__rsxf_shadowed(0x0000) && ps3dk_prof__rsxf_shadowed(0x181c) && ps3dk_prof__rsxf_shadowed(0x1fc0));
    /* one-shot actions are not state: NOP, NOTIFY, cache invalidates, report writes, semaphore releases */
    assert(!ps3dk_prof__rsxf_shadowed(0x0100) && !ps3dk_prof__rsxf_shadowed(0x0104) &&
           !ps3dk_prof__rsxf_shadowed(0x1710) && !ps3dk_prof__rsxf_shadowed(0x1714) &&
           !ps3dk_prof__rsxf_shadowed(0x1fd8) && !ps3dk_prof__rsxf_shadowed(0x1800) &&
           !ps3dk_prof__rsxf_shadowed(0x1d70) && !ps3dk_prof__rsxf_shadowed(0x1d74));
    /* interpreted, not render state: the VP constant and instruction upload pointers, AUTO's semaphore offset */
    assert(!ps3dk_prof__rsxf_shadowed(0x1efc) && !ps3dk_prof__rsxf_shadowed(0x1e9c) && !ps3dk_prof__rsxf_shadowed(0x1d6c));
    cleared = d.regs_cleared;
    ps3dk_prof__rsxf_decode_unknown(&d, 0, AUTO_RING_REJECT_JUMP);
    assert(!KNOWN(0x310) && !KNOWN(0x1a00) && d.regs_cleared == cleared + 1);
    assert(d.reg[0x310 >> 2] == 1); /* the value stays; it is just not known */
#undef KNOWN
#undef DIRTY
    ps3dk_prof__rsxf_decode_init(&d);
}

/* VP constants through VP_UPLOAD_CONST_ID + the data port (the id then x, y, z, w per slot, one packet), and inline
 * transfers into local memory (subchannel 3 destination, subchannel 5 pixels from the start pixel, padding not
 * stored); a rejection forgets both. */
static void constants_and_inline_writes(void)
{
    const uint32_t w[] = {M(0, 0x1efc, 5), 7, 1, 2, 3, 4,
                          M(0, 0x1efc, 9), 20, 11, 12, 13, 14, 21, 22, 23, 24,
                          M(3, 0x188, 1), 0xfeed0000u, M(3, 0x30c, 1), 0x1000,
                          M(5, 0x304, 3), 2, (1u << 16) | 3u, (1u << 16) | 3u, M(5, 0x400, 4), 0xa, 0xb, 0xc, 0,
                          M(3, 0x188, 1), 0xfeed0001u, M(5, 0x400, 2), 0x77, 0x78}; /* main memory: not mapped */
    uint32_t v = 0;
    ps3dk_prof__rsxf_decode_init(&d);
    decode(w, sizeof w / 4, 1);
    assert(d.vpc[7][0] == 1 && d.vpc[7][3] == 4 && d.vpc_known[7] == 15);
    assert(d.vpc[20][2] == 13 && d.vpc[21][0] == 21 && d.vpc_known[21] == 15 && !d.vpc_known[8]);
    assert((d.vpc_dirty[0] >> 7 & 1u) && (d.vpc_dirty[0] >> 20 & 1u) && (d.vpc_dirty[0] >> 21 & 1u));
    assert(ps3dk_prof__rsxf_inline_word(&d, 0x1008, &v) == RSXF_FPC_INLINE && v == 0xa);
    assert(ps3dk_prof__rsxf_inline_word(&d, 0x1010, &v) == RSXF_FPC_INLINE && v == 0xc);
    assert(ps3dk_prof__rsxf_inline_word(&d, 0x1014, &v) == RSXF_FPC_CONTAINER); /* the padding word: not written */
    assert(ps3dk_prof__rsxf_inline_word(&d, 0x1004, &v) == RSXF_FPC_CONTAINER);
    assert(d.imap_used == 3 && !d.imap_overflow && d.main_written);
    ps3dk_prof__rsxf_decode_unknown(&d, 0, AUTO_RING_REJECT_JUMP);
    assert(!d.vpc_known[7] && ps3dk_prof__rsxf_inline_word(&d, 0x1008, &v) == RSXF_FPC_UNKNOWN && d.imap_cleared == 1);
    ps3dk_prof__rsxf_decode_init(&d);
}

/* The inline-write map's limits: a whole COLOR packet past 32 words is remembered; an M2MF copy into local memory makes
 * its range UNKNOWN (an inline write after it is remembered again); a different program placed over remembered words
 * makes them UNKNOWN; the VP constant id is masked to 9 bits. */
static void inline_map_limits(void)
{
    uint32_t w[8 + 1 + 40], n = 0, v = 0;
    ps3dk_prof__rsxf_decode_init(&d);
    w[n++] = M(3, 0x188, 1); w[n++] = 0xfeed0000u; w[n++] = M(3, 0x30c, 1); w[n++] = 0x2000;
    w[n++] = M(5, 0x304, 2); w[n++] = 0; w[n++] = (1u << 16) | 40u;
    w[n++] = M(5, 0x400, 40);
    for (uint32_t i = 0; i < 40; i++)
        w[n++] = 0x100u + i;
    decode(w, n, 1);
    assert(ps3dk_prof__rsxf_inline_word(&d, 0x2000 + 4u * 39u, &v) == RSXF_FPC_INLINE && v == 0x100u + 39u);
    assert(d.imap_used == 40 && !d.imap_overflow);
    {
        /* M2MF: 16 bytes into local 0x2010 (words 4..7), then an inline write to word 5 */
        const uint32_t c[] = {M(1, 0x188, 1), 0xfeed0000u, M(1, 0x310, 1), 0x2010, M(1, 0x318, 4), 0, 16, 1, 0x101,
                              M(1, 0x328, 1), 0,
                              M(3, 0x30c, 1), 0x2014, M(5, 0x304, 2), 0, (1u << 16) | 1u, M(5, 0x400, 1), 0x55};
        decode(c, sizeof c / 4, 1);
        assert(d.m2mf_copies == 1 && d.ntaint == 1);
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x2010, &v) == RSXF_FPC_UNKNOWN);     /* remembered, then copied over */
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x2014, &v) == RSXF_FPC_INLINE && v == 0x55); /* written after the copy */
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x201c, &v) == RSXF_FPC_UNKNOWN);
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x2020, &v) == RSXF_FPC_INLINE && v == 0x108);  /* past the copy */
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x3000, &v) == RSXF_FPC_CONTAINER);
    }
    /* a different program over words 8..9: their inline values are no longer trusted */
    ps3dk_prof__rsxf_inline_forget(&d, 0x2020, 0x2028);
    assert(ps3dk_prof__rsxf_inline_word(&d, 0x2020, &v) == RSXF_FPC_UNKNOWN);
    assert(ps3dk_prof__rsxf_inline_word(&d, 0x2028, &v) == RSXF_FPC_INLINE && v == 0x10a);
    {
        /* VP_UPLOAD_CONST_ID keeps 9 bits: 0x200 + 3 selects slot 3, never a slot past the table */
        const uint32_t c[] = {M(0, 0x1efc, 5), 0x203u, 1, 2, 3, 4};
        decode(c, sizeof c / 4, 1);
        assert(d.vpc_id == 3 && d.vpc_known[3] == 15 && d.vpc[3][0] == 1);
    }
    ps3dk_prof__rsxf_decode_init(&d);
}

/* Unknown flow (an unseen CALL may have moved any destination cursor): data words after it stay unknown until their
 * destination is written again, then become known at the new destination. VP constants: id 7, a reset, four data
 * words (the GPU may write c20: not c7), then id 20 and data (c20 known). Inline transfer: a destination, a reset, a
 * COLOR packet (dropped, not remembered at the old offset), then a complete destination and data (remembered). VP
 * instructions: a slot, a reset, an instruction (dropped), then a slot and an instruction (known). M2MF: a copy whose
 * destination was not seen since the reset makes every remembered word UNKNOWN. */
static void cursors_after_unknown_flow(void)
{
    uint32_t v = 0, dropped;
    ps3dk_prof__rsxf_decode_init(&d);
    {
        const uint32_t a[] = {M(0, 0x1efc, 1), 7};
        const uint32_t b[] = {M(0, 0x1f00, 4), 1, 2, 3, 4};
        const uint32_t c[] = {M(0, 0x1efc, 5), 20, 5, 6, 7, 8};
        decode(a, sizeof a / 4, 1);
        ps3dk_prof__rsxf_decode_unknown(&d, 2, AUTO_RING_REJECT_CALL);
        dropped = d.port_dropped;
        decode(b, sizeof b / 4, 1);
        assert(!d.vpc_known[7] && !d.vpc_known[20] && d.port_dropped == dropped + 4);
        decode(c, sizeof c / 4, 1);
        assert(d.vpc_known[20] == 15 && d.vpc[20][0] == 5 && !d.vpc_known[7]);
    }
    {
        const uint32_t a[] = {M(3, 0x188, 1), 0xfeed0000u, M(3, 0x30c, 1), 0x4000, M(5, 0x304, 2), 0, (1u << 16) | 2u,
                              M(5, 0x400, 2), 0x11, 0x12};
        const uint32_t b[] = {M(5, 0x400, 2), 0x21, 0x22};
        const uint32_t c[] = {M(3, 0x188, 1), 0xfeed0000u, M(3, 0x30c, 1), 0x5000, M(5, 0x304, 2), 0, (1u << 16) | 2u,
                              M(5, 0x400, 2), 0x31, 0x32};
        decode(a, sizeof a / 4, 1);
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x4000, &v) == RSXF_FPC_INLINE && v == 0x11);
        ps3dk_prof__rsxf_decode_unknown(&d, 0x20000000u, AUTO_RING_REJECT_JUMP);
        dropped = d.port_dropped;
        decode(b, sizeof b / 4, 1);
        assert(d.port_dropped == dropped + 2 && d.imap_used == 0);
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x4000, &v) == RSXF_FPC_UNKNOWN); /* never 0x21 at the old offset */
        /* a partial destination is not enough: the offset alone leaves the rest unknown */
        {
            const uint32_t p[] = {M(3, 0x30c, 1), 0x5000, M(5, 0x400, 2), 0x41, 0x42};
            decode(p, sizeof p / 4, 1);
            assert(d.imap_used == 0 && d.port_dropped == dropped + 4);
        }
        decode(c, sizeof c / 4, 1);
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x5000, &v) == RSXF_FPC_INLINE && v == 0x31);
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x5004, &v) == RSXF_FPC_INLINE && v == 0x32);
    }
    {
        const uint32_t a[] = {M(0, 0x1e9c, 1), 10};
        const uint32_t b[] = {M(0, 0x0b80, 4), 1, 2, 3, 4};
        const uint32_t c[] = {M(0, 0x1e9c, 1), 12, M(0, 0x0b80, 4), 5, 6, 7, 8};
        decode(a, sizeof a / 4, 1);
        ps3dk_prof__rsxf_decode_unknown(&d, 2, AUTO_RING_REJECT_CALL);
        decode(b, sizeof b / 4, 1);
        assert(!d.vp_known[10] && !d.vp_known[0]);
        decode(c, sizeof c / 4, 1);
        assert(d.vp_known[12] && d.vp[12][0] == 5 && !d.vp_known[10]);
    }
    {
        /* an M2MF launch after a reset before its destination is written again: everything remembered is UNKNOWN */
        const uint32_t b[] = {M(1, 0x328, 1), 0};
        ps3dk_prof__rsxf_decode_unknown(&d, 2, AUTO_RING_REJECT_CALL);
        {
            /* re-establish the inline destination only, remember one word, then the blind copy */
            const uint32_t c[] = {M(3, 0x188, 1), 0xfeed0000u, M(3, 0x30c, 1), 0x6000, M(5, 0x304, 2), 0,
                                  (1u << 16) | 1u, M(5, 0x400, 1), 0x77};
            decode(c, sizeof c / 4, 1);
        }
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x6000, &v) == RSXF_FPC_INLINE && v == 0x77);
        decode(b, sizeof b / 4, 1);
        assert(ps3dk_prof__rsxf_inline_word(&d, 0x6000, &v) == RSXF_FPC_UNKNOWN && d.taint_all);
    }
    ps3dk_prof__rsxf_decode_init(&d);
}

static void register_records(void)
{
    uint8_t o[64];
    const uint16_t idx[2] = {0x310 >> 2, 0xa74 >> 2}, bad[2] = {0xa74 >> 2, 0x310 >> 2}, far[1] = {2048};
    const uint32_t val[2] = {1, 1};
    assert(ps3dk_prof__rsxf_put_regs(o, sizeof o, RSXF_REC_BASELINE, 0, 0, idx, val, 2) == 32);
    assert(o[0] == RSXF_REC_BASELINE && o[2] == 32 && o[8] == 2 && o[16] == (0x310 >> 2) && o[20] == 1);
    assert(!ps3dk_prof__rsxf_put_regs(o, sizeof o, RSXF_REC_BASELINE, 0, 0, bad, val, 2));   /* not increasing */
    assert(!ps3dk_prof__rsxf_put_regs(o, sizeof o, RSXF_REC_BASELINE, 0, 0, far, val, 1));   /* past the file */
    assert(!ps3dk_prof__rsxf_put_regs(o, sizeof o, RSXF_REC_BASELINE, 3, 0, idx, val, 2));   /* an ordinal */
    assert(!ps3dk_prof__rsxf_put_regs(o, sizeof o, RSXF_REC_STATE, 3, 0, idx, val, 0));      /* empty, no reset */
    assert(ps3dk_prof__rsxf_put_regs(o, sizeof o, RSXF_REC_STATE, 3, RSXF_STATE_RESET, idx, val, 0) == 16);
    assert(!ps3dk_prof__rsxf_put_regs(o, 31, RSXF_REC_STATE, 3, 0, idx, val, 2));            /* capacity */
}

static void utf8(void)
{
    char o[16];
    uint32_t n;
    /* valid text is copied as is, a NUL ends it */
    n = ps3dk_prof__rsxf_utf8_append(o, 0, sizeof o, "rock\0x", 6);
    assert(n == 4 && !memcmp(o, "rock", 4));
    /* invalid bytes (a lone continuation, an overlong form, a surrogate, past U+10FFFF, a cut sequence) become '?' */
    n = ps3dk_prof__rsxf_utf8_append(o, 0, sizeof o, "\x80" "a\xc0\xaf" "b\xed\xa0\x80\xf4\x90\x80\x80\xe2\x82", 15);
    assert(n == 14 && !memcmp(o, "?a??b?????????", 14));
    /* a multi-byte character that does not fit whole is left out: the text ends on a character boundary */
    memcpy(o, "abcdefghijklm", 13);
    n = ps3dk_prof__rsxf_utf8_append(o, 13, 15, "\xe2\x82\xac", 3);
    assert(n == 13);
    n = ps3dk_prof__rsxf_utf8_append(o, 12, 15, "\xe2\x82\xac", 3);
    assert(n == 15 && !memcmp(o + 12, "\xe2\x82\xac", 3));
    /* a 2-byte and a 4-byte character */
    n = ps3dk_prof__rsxf_utf8_append(o, 0, sizeof o, "\xc3\xa9\xf0\x9f\x8e\xae", 6);
    assert(n == 6 && !memcmp(o, "\xc3\xa9\xf0\x9f\x8e\xae", 6));
}

static void hashes(void)
{
    /* FP: inst 0 reads a constant (src0 type 2), its 16-byte block, inst 1 ends; stored halfword-swapped */
    const uint32_t logical[8] = {0x3e80, 0xc802, 0xc800, 0xc800, 0x3e81, 0xc800, 0xc800, 0xc800};
    uint8_t a[48], b[48];
    rsxf_hash ha, hb;
    size_t n = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t w = logical[i] << 16 | logical[i] >> 16;
        a[n] = (uint8_t)(w >> 24); a[n + 1] = (uint8_t)(w >> 16); a[n + 2] = (uint8_t)(w >> 8); a[n + 3] = (uint8_t)w;
        n += 4;
        if (i == 3) {
            memset(a + n, 0x11, 16);
            n += 16;
        }
    }
    memcpy(b, a, sizeof b);
    memset(b + 16, 0x77, 16); /* other constant values */
    ps3dk_prof__rsxf_fp_hash(a, sizeof a, &ha);
    ps3dk_prof__rsxf_fp_hash(b, sizeof b, &hb);
    assert(ha.hash == hb.hash && ha.instructions == 2 && ha.const_blocks == 1 && ha.bytes == 48 && !ha.unterminated);
    b[3] ^= 1; /* an instruction word */
    ps3dk_prof__rsxf_fp_hash(b, sizeof b, &hb);
    assert(ha.hash != hb.hash);
    ps3dk_prof__rsxf_fp_hash(a, 32, &hb);
    assert(hb.unterminated);
    {
        /* the reader's independent value of the same program (tools/ps3dk-prof tests: fp_expected_hash) */
        uint8_t v[14 + 32];
        memcpy(v, "ps3dk.rsxf.fp1", 14);
        memcpy(v + 14, a, 16);
        memcpy(v + 30, a + 32, 16);
        assert(ha.hash == ps3dk_prof__rsxf_fnv(RSXF_FNV_OFFSET, v, sizeof v));
    }
}

static void encoders(FILE *out, uint32_t owner)
{
    uint8_t buf[4096], rec[1024];
    size_t n, r = 0;
    rsxf_wire_config c = {RSXF_AVAILABLE, 262144u, 2u, 512u, 256u, 63u, 0u};
    rsxf_wire_begin b;
    rsxf_wire_program p;
    rsxf_wire_draw w;
    rsxf_wire_end e;
    /* refusals write nothing */
    c.block_bytes = 100;
    assert(!ps3dk_prof__rsxf_put_config(buf, sizeof buf, &c));
    c.block_bytes = 262144u;
    n = ps3dk_prof__rsxf_put_config(buf, sizeof buf, &c);
    assert(n == RSXF_CONFIG_BYTES);
    fwrite("C", 1, 1, out); fwrite(&n, 4, 1, out); fwrite(buf, 1, n, out);
    memset(&b, 0, sizeof b);
    b.id = 1; b.source = 2; b.owner = owner; b.frame = 5; b.name = "unit"; b.name_len = 4; b.known = 2; b.fp_address = 0x40;
    assert(!ps3dk_prof__rsxf_put_begin(buf, 60, &b));
    n = ps3dk_prof__rsxf_put_begin(buf, sizeof buf, &b);
    assert(n == RSXF_BEGIN_FIXED + 8);
    fwrite("B", 1, 1, out); fwrite(&n, 4, 1, out); fwrite(buf, 1, n, out);
    memset(&p, 0, sizeof p);
    p.index = 1; p.profile = RSXF_PROFILE_FP; p.source = RSXF_SRC_CONTAINER; p.instructions = 2; p.hash = 0x1234;
    p.ucode_bytes = 48; p.const_blocks = 1; p.address = 0x40;
    r += ps3dk_prof__rsxf_put_program(rec + r, sizeof rec - r, &p);
    assert(!ps3dk_prof__rsxf_put_text(rec + r, sizeof rec - r, RSXF_REC_LABEL, 1, "a\0b", 3));
    r += ps3dk_prof__rsxf_put_text(rec + r, sizeof rec - r, RSXF_REC_LABEL, 1, "terrain/rock", 12);
    memset(&w, 0, sizeof w);
    w.ordinal = 0; w.kind = RSXF_K_ARRAY; w.flags = RSXF_DF_DECODED; w.untimed = RSXF_UNTIMED_NOT_HOOKED;
    w.mode = 5; w.count = 36; w.label = 1; w.fp = 1; w.fp_address = 0x40;
    {
        /* the register file at arming, then what the draw changed */
        const uint16_t bi[2] = {0x310 >> 2, 0xa74 >> 2}, si[1] = {0xa6c >> 2};
        const uint32_t bv[2] = {1, 1}, sv[1] = {0x203};
        uint8_t tmp[1024];
        size_t pre = ps3dk_prof__rsxf_put_regs(tmp, sizeof tmp, RSXF_REC_BASELINE, 0, 0, bi, bv, 2);
        assert(pre == 32);
        pre += ps3dk_prof__rsxf_put_regs(tmp + pre, sizeof tmp - pre, RSXF_REC_STATE, 0, 0, si, sv, 1);
        assert(pre == 56);
        /* BASELINE, then the draw's STATE, then its definitions and the DRAW */
        memmove(rec + pre, rec, r);
        memcpy(rec, tmp, pre);
        r += pre;
    }
    r += ps3dk_prof__rsxf_put_draw(rec + r, sizeof rec - r, &w);
    assert(r == 56 + RSXF_PROGRAM_BYTES + 32 + RSXF_DRAW_BYTES);
    n = ps3dk_prof__rsxf_put_draws_header(buf, sizeof buf, 1, 0, 5, (uint32_t)r);
    memcpy(buf + n, rec, r);
    n += r;
    fwrite("D", 1, 1, out); fwrite(&n, 4, 1, out); fwrite(buf, 1, n, out);
    memset(&e, 0, sizeof e);
    e.id = 1; e.outcome = RSXF_OUT_DONE; e.blocks = 1; e.draws_seen = 1; e.draws_recorded = 1; e.decoded_only = 1;
    e.truncated_at = RSXF_NO_ORDINAL; e.programs = 1; e.labels = 1; e.untimed = 1; e.bytes_streamed = r;
    e.lost_events = 2;
    e.baseline_registers = 2;
    e.state_records = 1;
    e.outcome = RSXF_OUT_TRUNCATED;
    assert(!ps3dk_prof__rsxf_put_end(buf, sizeof buf, &e)); /* TRUNCATED needs an ordinal */
    e.outcome = RSXF_OUT_DONE;
    n = ps3dk_prof__rsxf_put_end(buf, sizeof buf, &e);
    assert(n == RSXF_END_BYTES);
    fwrite("E", 1, 1, out); fwrite(&n, 4, 1, out); fwrite(buf, 1, n, out);
}

int main(int argc, char **argv)
{
    uint32_t owner = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 1u;
    FILE *out = fopen("unit-sample.bin", "wb");
    assert(out);
    vp_upload_and_start();
    draws_and_clears();
    rejections_forget_and_recover();
    hashes();
    register_shadow();
    constants_and_inline_writes();
    inline_map_limits();
    cursors_after_unknown_flow();
    register_records();
    utf8();
    encoders(out, owner);
    assert(fclose(out) == 0);
    puts("rsxf units PASS");
    return 0;
}
