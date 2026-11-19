/* RSX frame capture: chunk and record encoders and the canonical program hash (format section 12). Pure functions:
 * no allocation, no globals; every byte of every output is written (reserved fields and padding zero). */
#include "rsxf.h"
#include <string.h>

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }
static void le64(uint8_t *p, uint64_t v) { le32(p, (uint32_t)v); le32(p + 4, (uint32_t)(v >> 32)); }
static size_t pad8(size_t n) { return (n + 7u) & ~(size_t)7u; }

size_t ps3dk_prof__rsxf_put_config(uint8_t *o, size_t cap, const rsxf_wire_config *c)
{
    if (!o || !c || cap < RSXF_CONFIG_BYTES || c->availability > RSXF_NO_DECODER || c->block_bytes < 4096u ||
        c->block_bytes > 1048576u || (c->block_bytes & 7u) || !c->blocks || c->blocks > 16u || !c->programs ||
        c->programs > 65535u || !c->labels || c->labels > 65535u || !c->label_bytes || c->label_bytes > 255u)
        return 0;
    memset(o, 0, RSXF_CONFIG_BYTES);
    le16(o, 1); le32(o + 4, 1); le32(o + 8, c->availability); le32(o + 12, c->block_bytes); le32(o + 16, c->blocks);
    le32(o + 20, c->programs); le32(o + 24, c->labels); le32(o + 28, c->label_bytes);
    return RSXF_CONFIG_BYTES;
}

size_t ps3dk_prof__rsxf_put_begin(uint8_t *o, size_t cap, const rsxf_wire_begin *b)
{
    size_t n;
    if (!o || !b || !b->id || b->name_len > RSXF_NAME_MAX || (b->name_len && !b->name) || b->source < 1 ||
        b->source > 2 || (b->flags & ~3u) || (b->known & ~3u) || ((b->source == 1) != (b->request != 0)))
        return 0;
    n = RSXF_BEGIN_FIXED + pad8(b->name_len);
    if (cap < n)
        return 0;
    memset(o, 0, n);
    le32(o, b->id); le16(o + 4, b->source); le16(o + 6, b->flags); le32(o + 8, b->request); le32(o + 12, b->owner);
    le64(o + 16, b->frame); le32(o + 24, b->window); le64(o + 32, b->tb);
    le32(o + 40, (b->known & 1u) ? b->vp_start : 0u); le32(o + 44, (b->known & 2u) ? b->fp_address : 0u);
    le32(o + 48, b->known); le16(o + 52, b->name_len);
    if (b->name_len)
        memcpy(o + 56, b->name, b->name_len);
    return n;
}

size_t ps3dk_prof__rsxf_put_draw(uint8_t *o, size_t cap, const rsxf_wire_draw *d)
{
    if (!o || !d || cap < RSXF_DRAW_BYTES)
        return 0;
    memset(o, 0, RSXF_DRAW_BYTES);
    le16(o, RSXF_REC_DRAW); le16(o + 2, RSXF_DRAW_BYTES); le32(o + 4, d->ordinal); le16(o + 8, d->kind);
    le16(o + 10, d->flags); le32(o + 12, d->untimed); le64(o + 16, d->op_id); le32(o + 24, d->mode);
    le32(o + 28, d->first); le32(o + 32, d->count); le32(o + 36, d->index_offset); o[40] = d->index_type;
    o[41] = d->index_location; le32(o + 44, d->clear_mask); le32(o + 48, d->tag); le32(o + 52, d->label);
    le32(o + 56, d->vp); le32(o + 60, d->fp); le32(o + 64, d->fp_address); le32(o + 68, d->vp_start);
    le64(o + 72, d->caller); le32(o + 80, d->words); le64(o + 88, d->tb);
    return RSXF_DRAW_BYTES;
}

size_t ps3dk_prof__rsxf_put_program(uint8_t *o, size_t cap, const rsxf_wire_program *p)
{
    if (!o || !p || cap < RSXF_PROGRAM_BYTES || !p->index)
        return 0;
    memset(o, 0, RSXF_PROGRAM_BYTES);
    le16(o, RSXF_REC_PROGRAM); le16(o + 2, RSXF_PROGRAM_BYTES); le32(o + 4, p->index); o[8] = p->profile;
    o[9] = p->source; le16(o + 10, p->flags); le32(o + 12, p->instructions); le64(o + 16, p->hash);
    le32(o + 24, p->ucode_bytes); le32(o + 28, p->const_blocks); le32(o + 32, p->address); le32(o + 36, p->first_draw);
    return RSXF_PROGRAM_BYTES;
}

size_t ps3dk_prof__rsxf_text_bytes(uint32_t n) { return 16u + pad8(n); }

size_t ps3dk_prof__rsxf_put_text(uint8_t *o, size_t cap, uint16_t type, uint32_t index, const char *s, uint32_t n)
{
    size_t len = ps3dk_prof__rsxf_text_bytes(n);
    if (!o || !s || !n || n > RSXF_TEXT_MAX || !index || (type != RSXF_REC_LABEL && type != RSXF_REC_NAME) ||
        cap < len || memchr(s, 0, n))
        return 0;
    memset(o, 0, len);
    le16(o, type); le16(o + 2, (uint32_t)len); le32(o + 4, index); le16(o + 8, n);
    memcpy(o + 16, s, n);
    return len;
}

size_t ps3dk_prof__rsxf_put_draws_header(uint8_t *o, size_t cap, uint32_t id, uint32_t seq, uint32_t records,
                                         uint32_t record_bytes)
{
    if (!o || cap < RSXF_DRAWS_HEADER || !records)
        return 0;
    le32(o, id); le32(o + 4, seq); le32(o + 8, records); le32(o + 12, record_bytes);
    return RSXF_DRAWS_HEADER;
}

size_t ps3dk_prof__rsxf_put_end(uint8_t *o, size_t cap, const rsxf_wire_end *e)
{
    if (!o || !e || cap < RSXF_END_BYTES || e->outcome < 1 || e->outcome > 3 || e->reason > RSXF_R_CANCELLED ||
        ((e->truncated_at != RSXF_NO_ORDINAL) != (e->outcome == RSXF_OUT_TRUNCATED)) ||
        (!e->reject_reason && e->reject_header))
        return 0;
    memset(o, 0, RSXF_END_BYTES);
    le32(o, e->id); le16(o + 4, e->outcome); le16(o + 6, e->reason); le32(o + 8, e->blocks);
    le32(o + 12, e->blocks_dropped); le32(o + 16, e->draws_seen); le32(o + 20, e->draws_recorded);
    le32(o + 24, e->hooked); le32(o + 28, e->decoded_only); le32(o + 32, e->mismatches); le32(o + 36, e->truncated_at);
    le32(o + 40, e->programs); le32(o + 44, e->labels); le32(o + 48, e->labels_refused);
    le32(o + 52, e->programs_refused); le64(o + 56, e->bytes_streamed); le64(o + 64, e->bytes_dropped);
    le64(o + 72, e->tb_end); le64(o + 80, e->decoder_words); le32(o + 88, e->reject_header);
    le32(o + 92, e->reject_reason); le32(o + 96, e->fp_reads); le64(o + 104, e->fp_read_ticks);
    le32(o + 112, e->untimed);
    return RSXF_END_BYTES;
}

/* ---- canonical hash ----------------------------------------------------------------------------------------------- */

uint64_t ps3dk_prof__rsxf_fnv(uint64_t h, const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static uint32_t swap16(uint32_t w) { return w << 16 | w >> 16; }

void ps3dk_prof__rsxf_fp_hash(const uint8_t *u, size_t bytes, rsxf_hash *out)
{
    static const char domain[] = "ps3dk.rsxf.fp1";
    uint64_t h = ps3dk_prof__rsxf_fnv(RSXF_FNV_OFFSET, domain, sizeof domain - 1u);
    size_t pos = 0;
    uint32_t n = 0, consts = 0, open = 1;
    while (u && pos + 16u <= bytes && n < 65536u) {
        const uint8_t *in = u + pos;
        int constant = 0, end = (swap16(be32(in)) & 1u) != 0;
        h = ps3dk_prof__rsxf_fnv(h, in, 16);
        n++;
        pos += 16;
        for (int k = 1; k < 4; k++)
            if ((swap16(be32(in + 4 * k)) & 3u) == 2u)
                constant = 1;
        if (constant) {
            consts++;
            pos = pos + 16u <= bytes ? pos + 16u : bytes;
        }
        if (end) {
            open = 0;
            break;
        }
    }
    out->hash = h; out->instructions = n; out->bytes = (uint32_t)pos; out->const_blocks = consts;
    out->unterminated = open;
}

void ps3dk_prof__rsxf_vp_hash(const uint32_t *w, uint32_t instructions, rsxf_hash *out)
{
    static const char domain[] = "ps3dk.rsxf.vp1";
    uint64_t h = ps3dk_prof__rsxf_fnv(RSXF_FNV_OFFSET, domain, sizeof domain - 1u);
    uint32_t n = 0, open = 1;
    while (w && n < instructions && n < RSXF_VP_SLOTS) {
        uint8_t b[16];
        for (int k = 0; k < 4; k++) {
            uint32_t v = w[4 * n + k];
            b[4 * k] = (uint8_t)(v >> 24); b[4 * k + 1] = (uint8_t)(v >> 16);
            b[4 * k + 2] = (uint8_t)(v >> 8); b[4 * k + 3] = (uint8_t)v;
        }
        h = ps3dk_prof__rsxf_fnv(h, b, 16);
        n++;
        if (w[4 * (n - 1) + 3] & 1u) {
            open = 0;
            break;
        }
    }
    out->hash = h; out->instructions = n; out->bytes = n * 16u; out->const_blocks = 0; out->unterminated = open;
}
