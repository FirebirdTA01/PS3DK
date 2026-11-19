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
    }
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
    rsxf_wire_config c = {RSXF_AVAILABLE, 262144u, 2u, 512u, 256u, 63u};
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
    r += ps3dk_prof__rsxf_put_draw(rec + r, sizeof rec - r, &w);
    assert(r == RSXF_PROGRAM_BYTES + 32 + RSXF_DRAW_BYTES);
    n = ps3dk_prof__rsxf_put_draws_header(buf, sizeof buf, 1, 0, 3, (uint32_t)r);
    memcpy(buf + n, rec, r);
    n += r;
    fwrite("D", 1, 1, out); fwrite(&n, 4, 1, out); fwrite(buf, 1, n, out);
    memset(&e, 0, sizeof e);
    e.id = 1; e.outcome = RSXF_OUT_DONE; e.blocks = 1; e.draws_seen = 1; e.draws_recorded = 1; e.decoded_only = 1;
    e.truncated_at = RSXF_NO_ORDINAL; e.programs = 1; e.labels = 1; e.untimed = 1; e.bytes_streamed = r;
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
    encoders(out, owner);
    assert(fclose(out) == 0);
    puts("rsxf units PASS");
    return 0;
}
