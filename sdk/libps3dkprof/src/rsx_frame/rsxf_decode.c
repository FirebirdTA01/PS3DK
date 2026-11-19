/* RSX frame capture: the owner command-stream decoder, P1 subset (VP upload and start, FP_ADDRESS, index registers,
 * draw triggers, clears). Subchannel 0 (the 3D object the firmware binds) only; other subchannels are skipped by
 * count. Header rules are those of the AUTO ring decoder (auto_ring_commands.c): a JUMP, CALL or RETURN, a reserved
 * bit, an empty or truncated packet or a method range past 0x1ffc is not followed; the decoder then forgets every
 * known register (the unseen words may have changed them) and continues with the next word it is given. A register
 * becomes known again when it is next written. No allocation, no reads beyond the n words. */
#include "rsxf.h"
#include "../auto_rsx/auto_ring_commands.h"
#include <string.h>

enum {
    M_FP_ADDRESS = 0x08e4, M_VP_INST_FIRST = 0x0b80, M_VP_INST_END = 0x0c00, M_BEGIN_END = 0x1808,
    M_ELEMENT_U16 = 0x180c, M_ELEMENT_U32 = 0x1810, M_VERTEX_BATCH = 0x1814, M_VERTEX_DATA = 0x1818,
    M_INDEX_OFFSET = 0x181c, M_INDEX_DMA = 0x1820, M_INDEX_DRAW = 0x1824, M_ATTR_DATA_FIRST = 0x1880,
    M_ATTR_DATA_END = 0x1d00, /* VTX_ATTR_2F/2I/4UB/4I (0x1880..) through VTX_ATTR_4F (..0x1cfc) */ M_CLEAR = 0x1d94, M_VP_UPLOAD_FROM = 0x1e9c, M_VP_START_FROM = 0x1ea0
};

void ps3dk_prof__rsxf_decode_init(rsxf_decoder *d)
{
    memset(d, 0, sizeof *d);
}

int ps3dk_prof__rsxf_shadowed(uint32_t m)
{
    if (m < 0x0100u || m > 0x1ffcu || (m & 3u))
        return 0; /* the object bind and the channel words below NOP */
    if (m >= M_VP_INST_FIRST && m < M_VP_INST_END)
        return 0; /* VP instruction upload port */
    if (m >= 0x1f00u && m <= 0x1f7cu)
        return 0; /* VP constant upload port */
    switch (m) {
    case M_BEGIN_END: case M_ELEMENT_U16: case M_ELEMENT_U32: case M_VERTEX_BATCH: case M_VERTEX_DATA:
    case M_INDEX_DRAW: case M_CLEAR: case 0x0110u: /* WAIT_FOR_IDLE */
        return 0; /* draw triggers, inline data and pipeline waits */
    default:
        return 1;
    }
}

void ps3dk_prof__rsxf_decode_unknown(rsxf_decoder *d, uint32_t header, uint32_t reason)
{
    if (reason && !d->bad_reason) {
        d->bad_header = header;
        d->bad_reason = reason;
    }
    d->known = 0;
    memset(d->reg_known, 0, sizeof d->reg_known);
    d->regs_cleared++;
    memset(d->vpc_known, 0, sizeof d->vpc_known);
    /* unseen words may have held inline transfers: no remembered word can be trusted */
    memset(d->imap_addr, 0, sizeof d->imap_addr);
    d->imap_used = 0;
    d->imap_cleared++;
    memset(d->vp_known, 0, sizeof d->vp_known);
    d->vp_generation++;
    d->in_group = 0;
    d->resets++;
}

static void event(rsxf_decoder *d, const rsxf_draw_event *e)
{
    if (d->sink)
        d->sink(d->sink_user, e); /* at once: the registers are this draw's */
    else if (d->n_events < sizeof d->events / sizeof d->events[0])
        d->events[d->n_events++] = *e;
    else
        d->lost_events++;
}

static void group_kind(rsxf_decoder *d, uint32_t kind)
{
    if (!d->group.kind)
        d->group.kind = kind;
    else if (d->group.kind != kind)
        d->group.kind = RSXF_K_IMMEDIATE; /* mixed triggers in one group */
}

static void method(rsxf_decoder *d, uint32_t m, uint32_t v)
{
    if (ps3dk_prof__rsxf_shadowed(m)) {
        uint32_t i = m >> 2, bit = 1u << (i & 31u);
        d->reg[i] = v;
        d->reg_known[i >> 5] |= bit;
        d->reg_dirty[i >> 5] |= bit;
    }
    if (m >= M_VP_INST_FIRST && m < M_VP_INST_END) {
        uint32_t slot = d->vp_upload;
        if (slot < RSXF_VP_SLOTS) {
            d->vp[slot][d->vp_word] = v;
            if (d->vp_word == 3)
                d->vp_known[slot] = 1;
        }
        if (++d->vp_word == 4) {
            d->vp_word = 0;
            d->vp_upload++;
            d->vp_generation++;
        }
        return;
    }
    if (m >= 0x1f00u && m <= 0x1f7cu) {
        /* VP constant data port: slot = the uploaded id + the vec4 the method address selects */
        uint32_t slot = d->vpc_id + ((m - 0x1f00u) >> 4), comp = (m >> 2) & 3u;
        if (slot < RSXF_VP_CONSTS) {
            d->vpc[slot][comp] = v;
            d->vpc_known[slot] |= (uint8_t)(1u << comp);
            d->vpc_dirty[slot >> 5] |= 1u << (slot & 31u);
        }
        return;
    }
    switch (m) {
    case 0x1efc: d->vpc_id = v; return; /* VP_UPLOAD_CONST_ID */
    case M_VP_UPLOAD_FROM: d->vp_upload = v; d->vp_word = 0; return;
    case M_VP_START_FROM: d->vp_start = v; d->known |= 1u; return;
    case M_FP_ADDRESS: d->fp_address = v; d->known |= 2u; return;
    case M_INDEX_OFFSET: d->index_offset = v; d->known |= 4u; return;
    case M_INDEX_DMA: d->index_dma = v; return;
    case M_CLEAR: {
        rsxf_draw_event e;
        memset(&e, 0, sizeof e);
        e.kind = RSXF_K_CLEAR;
        e.clear_mask = v;
        event(d, &e);
        return;
    }
    case M_BEGIN_END:
        if (v) {
            memset(&d->group, 0, sizeof d->group);
            d->group.mode = v;
            d->group_batches = 0;
            d->in_group = 1;
        } else if (d->in_group) {
            d->in_group = 0;
            if (!d->group.kind)
                d->group.kind = RSXF_K_IMMEDIATE;
            event(d, &d->group);
        }
        return;
    default: break;
    }
    if (!d->in_group)
        return;
    d->group.words++;
    switch (m) {
    case M_VERTEX_BATCH:
    case M_INDEX_DRAW:
        group_kind(d, m == M_VERTEX_BATCH ? RSXF_K_ARRAY : RSXF_K_INDEX);
        if (!d->group_batches++)
            d->group.first = v & 0xffffffu;
        d->group.count += (v >> 24) + 1u;
        if (m == M_INDEX_DRAW) {
            d->group.index_offset = d->index_offset;
            d->group.index_location = d->index_dma & 1u;
            d->group.index_type = (d->index_dma >> 4) & 1u;
        }
        return;
    case M_ELEMENT_U16:
        group_kind(d, RSXF_K_INLINE_INDEX);
        d->group.index_type = 1;
        d->group.count += 2;
        return;
    case M_ELEMENT_U32:
        group_kind(d, RSXF_K_INLINE_INDEX);
        d->group.count += 1;
        return;
    case M_VERTEX_DATA:
        group_kind(d, RSXF_K_INLINE_ARRAY);
        d->group.count += 1; /* inline arrays count their data words */
        return;
    default:
        if (m >= M_ATTR_DATA_FIRST && m < M_ATTR_DATA_END)
            group_kind(d, RSXF_K_IMMEDIATE);
        return;
    }
}

static uint32_t imap_slot(uint32_t addr) { return ((addr >> 2) * 2654435761u) >> (32 - 12); }

static void imap_store(rsxf_decoder *d, uint32_t addr, uint32_t v)
{
    uint32_t i = imap_slot(addr);
    for (uint32_t k = 0; k < 16; k++, i = (i + 1u) & (RSXF_INLINE_MAP - 1u)) {
        if (d->imap_addr[i] == addr + 1u || !d->imap_addr[i]) {
            if (!d->imap_addr[i])
                d->imap_used++;
            d->imap_addr[i] = addr + 1u;
            d->imap_val[i] = v;
            return;
        }
    }
    d->imap_overflow++; /* not remembered: a later lookup of it reports UNKNOWN, never an older value */
}

int ps3dk_prof__rsxf_inline_word(const rsxf_decoder *d, uint32_t addr, uint32_t *v)
{
    uint32_t i = imap_slot(addr);
    for (uint32_t k = 0; k < 16; k++, i = (i + 1u) & (RSXF_INLINE_MAP - 1u)) {
        if (d->imap_addr[i] == addr + 1u) {
            *v = d->imap_val[i];
            return 1;
        }
        if (!d->imap_addr[i])
            return 0;
    }
    return 0;
}

/* Subchannels 3 (surfaces 2D) and 5 (image from CPU), as this SDK's inline transfer binds and writes them: the
 * destination DMA and offset, the start pixel and width, then 32-bit pixels landing at offset + 4 x (x + i). */
static void method_2d(rsxf_decoder *d, uint32_t sub, uint32_t m, uint32_t v)
{
    if (sub == 3) {
        if (m == 0x188u)
            d->s3_dma = v;
        else if (m == 0x30cu)
            d->s3_offset = v;
        return;
    }
    if (m == 0x304u)
        d->s5_x = v & 0xffffu;
    else if (m == 0x308u)
        d->s5_w = v & 0xffffu;
    else if (m >= 0x400u && m < 0x480u) {
        uint32_t i = (m - 0x400u) >> 2;
        if (i < d->s5_w && d->s3_dma == 0xfeed0000u)
            imap_store(d, d->s3_offset + 4u * (d->s5_x + i), v);
    }
}

int ps3dk_prof__rsxf_decode(rsxf_decoder *d, const uint32_t *p, size_t n)
{
    const uint32_t allowed = 0x40000000u | (2047u << 18) | (7u << 13) | 0x1ffcu;
    size_t at = 0;
    int ok = 1;
    if (!p)
        return n == 0;
    d->words += n;
    while (at < n) {
        uint32_t h = p[at++], count = (h >> 18) & 2047u, m = h & 0x1ffcu, sub = (h >> 13) & 7u;
        uint32_t why = AUTO_RING_REJECT_NONE;
        if (h & ~allowed) {
            if ((h & 0xe0000003u) == 0x20000000u || (h & 0xe0000003u) == 1u) why = AUTO_RING_REJECT_JUMP;
            else if ((h & 3u) == 2u) why = AUTO_RING_REJECT_CALL;
            else if ((h & 0xffff0003u) == 0x00020000u) why = AUTO_RING_REJECT_RETURN;
            else why = AUTO_RING_REJECT_RESERVED;
        } else if (!count && h) why = AUTO_RING_REJECT_EMPTY;
        else if (count > n - at) why = AUTO_RING_REJECT_TRUNCATED;
        else if (!(h & 0x40000000u) && count && m + 4u * (count - 1u) > 0x1ffcu) why = AUTO_RING_REJECT_RANGE;
        if (why) {
            ps3dk_prof__rsxf_decode_unknown(d, h, why);
            ok = 0;
            if (why == AUTO_RING_REJECT_TRUNCATED)
                return 0; /* the rest of the words are this packet's payload */
            continue;
        }
        if (sub == 0)
            for (uint32_t i = 0; i < count; i++)
                method(d, (h & 0x40000000u) ? m : m + 4u * i, p[at + i]);
        else if (sub == 3 || sub == 5)
            for (uint32_t i = 0; i < count; i++)
                method_2d(d, sub, (h & 0x40000000u) ? m : m + 4u * i, p[at + i]);
        at += count;
    }
    return ok;
}
