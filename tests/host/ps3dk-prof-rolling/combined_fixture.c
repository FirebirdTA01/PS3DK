/* Test-only, fixed mixed source and accepted-output model. Not a runtime
 * mirror. Checkpoints derive solely from this C model's accepted chunks;
 * original source bytes and actual physical commits are independent of Rust. */
#include "roll_store.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SOURCE_CAP = 65536, CHUNK_CAP = 8192, CHECKPOINT_CAP = 32768,
       FILE_CAP = 262144, META_CAP = 128, JOURNAL_CAP = 32, STACK_CAP = 4,
       CUTS = 13, SEGMENTS = 12, PATH_CAP = 1024 };
typedef struct { unsigned char data[SOURCE_CAP]; size_t n; } buffer;
typedef struct { unsigned char bytes[32]; } record;
typedef struct { record begin, parent; uint64_t frame; } ppu_stack;
typedef struct {
    unsigned present, flags, depth;
    uint64_t written, next, max_span, last_tb, next_frame, frame;
    uint64_t last_frame_seq, last_frame_tb, record_chunks;
    ppu_stack stack[STACK_CAP];
} ppu;
typedef struct { uint64_t seq; uint32_t epoch, raw, desc, depth; } event;
typedef struct {
    unsigned kind, ordinal, depth;
    uint64_t first, last, first_seq, events, chunks;
    event stack[STACK_CAP];
} journal;
typedef struct { size_t offset, bytes; uint64_t index; unsigned type; } metadata;
typedef struct {
    uint64_t next_chunk, prefix_bytes, spu_next, order_count;
    unsigned epoch, spu_depth, brackets, terminal, window, domain_final, order_final;
    ppu lanes[2]; event spu_stack[STACK_CAP];
    metadata meta[META_CAP]; unsigned meta_count;
    journal entries[JOURNAL_CAP]; unsigned entries_count;
} accepted;
typedef struct { accepted state; size_t offset; buffer checkpoint; } cut;
static buffer source;
static accepted live;
static cut cuts[CUTS];
static size_t segment_end[SEGMENTS];

static void require(int ok, const char *why)
{
    if (!ok) { fprintf(stderr, "combined_fixture: %s\n", why); exit(1); }
}
static void raw(buffer *b, const void *p, size_t n)
{
    require(n <= sizeof b->data - b->n, "bounded buffer exhausted");
    memcpy(b->data + b->n, p, n); b->n += n;
}
static void u8(buffer *b, unsigned n) { unsigned char c = (unsigned char)n; raw(b, &c, 1); }
static void u16(buffer *b, unsigned n) { u8(b, n); u8(b, n >> 8); }
static void u32(buffer *b, uint32_t n) { unsigned i; for (i = 0; i < 4; ++i) u8(b, n >> (8 * i)); }
static void u64(buffer *b, uint64_t n) { unsigned i; for (i = 0; i < 8; ++i) u8(b, (unsigned)(n >> (8 * i))); }
static uint32_t r32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t r64(const unsigned char *p) { return r32(p) | (uint64_t)r32(p + 4) << 32; }
static void string(buffer *b, const char *s) { size_t n = strlen(s); require(n <= 65535, "string length"); u16(b, (unsigned)n); raw(b, s, n); }
static uint32_t crc(const unsigned char *p, size_t n)
{
    uint32_t c = UINT32_MAX; size_t i; unsigned bit;
    for (i = 0; i < n; ++i) {
        c ^= p[i]; for (bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return c ^ UINT32_MAX;
}
static uint64_t fnv(const buffer *b)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325); size_t i;
    for (i = 0; i < b->n; ++i) h = (h ^ b->data[i]) * UINT64_C(0x100000001b3);
    return h;
}
static int is_spu(unsigned ty) { return (ty >= 8 && ty <= 13) || ty == 16 || ty == 18; }
static journal *entry(accepted *s, unsigned kind)
{
    journal *j; require(s->entries_count < JOURNAL_CAP, "journal cap");
    j = &s->entries[s->entries_count++]; memset(j, 0, sizeof *j); j->kind = kind; return j;
}
/* Only these independently emitted source shapes are in the fixture subset.
 * State is copied before admission and installed after the exact memory write. */
static void accept(accepted *s, unsigned ty, const buffer *b)
{
    const unsigned char *p = b->data; unsigned i, count;
    require(!s->terminal && s->next_chunk < META_CAP, "terminal/chunk count");
    if (ty == 4) {
        unsigned inc; require(b->n >= 22 && r32(p) == 1, "THREADS subset"); inc = r32(p + 4);
        require(inc >= 1 && inc <= 2 && !s->lanes[inc - 1].present, "THREADS identity");
        s->lanes[inc - 1].present = 1;
    } else if (ty == 5) {
        ppu *l; unsigned inc; require(b->n >= 8, "RECORDS header");
        inc = r32(p); count = r32(p + 4);
        require(inc >= 1 && inc <= 2 && s->lanes[inc - 1].present, "RECORDS owner");
        require(count && count <= 32 && b->n == 8 + (size_t)count * 32, "RECORDS shape");
        l = &s->lanes[inc - 1];
        for (i = 0; i < count; ++i) {
            const unsigned char *r = p + 8 + 32 * i; unsigned kind = r[0], depth = r[1];
            uint64_t tb = r64(r + 16), id = r64(r + 24);
            require(r[2] == 0 && r[3] == 0 && r64(r + 8) == l->next, "PPU flags/sequence");
            if (kind == 1) {
                ppu_stack *st;
                require(depth == l->depth && depth < STACK_CAP && id > l->max_span, "PPU BEGIN");
                require(i + 1 < count && r[32] == 2, "BEGIN_PARENT complete chunk");
                st = &l->stack[l->depth++]; memcpy(st->begin.bytes, r, 32); st->frame = l->frame;
                l->max_span = id;
            } else if (kind == 2) {
                ppu_stack *st;
                require(l->depth && depth == l->depth - 1, "PPU PARENT depth");
                st = &l->stack[l->depth - 1];
                require(tb == (depth ? r64(l->stack[depth - 1].begin.bytes + 24) : 0), "PPU parent identity");
                require(r64(r + 8) == r64(st->begin.bytes + 8) + 1, "PPU parent adjacent");
                memcpy(st->parent.bytes, r, 32);
            } else if (kind == 3) {
                require(l->depth && depth == l->depth - 1, "PPU END depth");
                require(id == r64(l->stack[depth].begin.bytes + 24)
                    && r32(r + 4) == r32(l->stack[depth].begin.bytes + 4), "PPU END identity");
                --l->depth;
            } else if (kind == 4) {
                require(!l->depth && id == l->next_frame, "PPU FRAME");
                l->frame = id; l->next_frame = id + 1; l->last_frame_seq = l->next;
                l->last_frame_tb = tb; l->flags |= 2 | 64;
            } else require(0, "unsupported PPU kind");
            if (kind != 2) { require(!(l->flags & 1) || tb >= l->last_tb, "PPU TB"); l->last_tb = tb; l->flags |= 1; }
            ++l->next; ++l->written;
        }
        ++l->record_chunks;
    } else if (ty == 9) {
        require(b->n == 32 && r32(p + 8) == 1 && !s->epoch, "EPOCH subset"); s->epoch = 1;
    } else if (ty == 10) {
        require(b->n == 44 && r32(p + 4) == s->epoch && r64(p + 8) == s->brackets, "CAL sequence"); ++s->brackets;
    } else if (ty == 13) {
        journal *j;
        require(b->n >= 20 && r32(p) == 1 && r32(p + 4) == s->epoch && s->epoch == 1
            && r64(p + 8) == s->spu_next && !s->domain_final, "SPU source/sequence");
        count = r32(p + 16); require(count && count <= 32 && b->n == 20 + (size_t)count * 8, "SPU shape");
        if (!s->entries_count || s->entries[s->entries_count - 1].kind != 2) {
            j = entry(s, 2); j->first = s->next_chunk; j->first_seq = s->spu_next;
        } else j = &s->entries[s->entries_count - 1];
        for (i = 0; i < count; ++i) {
            const unsigned char *e = p + 20 + i * 8;
            unsigned desc = e[4] | (unsigned)e[5] << 8, kind = e[6], depth = e[7];
            require(desc < 3, "SPU desc");
            if (kind == 1) {
                event *v; require(depth == s->spu_depth && depth < STACK_CAP, "SPU BEGIN");
                v = &s->spu_stack[s->spu_depth++]; v->seq = s->spu_next;
                v->epoch = s->epoch; v->raw = r32(e); v->desc = desc; v->depth = depth;
            } else {
                require(kind == 2 && s->spu_depth && depth == s->spu_depth - 1
                    && s->spu_stack[depth].desc == desc, "SPU END"); --s->spu_depth;
            }
            ++s->spu_next;
        }
        j->last = s->next_chunk; j->events += count; ++j->chunks; j->depth = s->spu_depth;
        memcpy(j->stack, s->spu_stack, sizeof j->stack);
    } else if (ty == 18) {
        require(b->n >= 8 && r32(p + 4) == 0, "ORDER header"); count = r32(p);
        require(count <= 2 && b->n == 8 + 24 * (size_t)count, "ORDER shape");
        for (i = 0; i < count; ++i) {
            const unsigned char *r = p + 8 + i * 24; unsigned kind = r32(r);
            require(r32(r + 4) == 1 && !s->order_final, "ORDER lane/final");
            if (kind == 5) {
                require(r64(r + 8) == s->order_count && r64(r + 16) == 0, "ORDER final census"); s->order_final = 1;
            } else { require(kind >= 1 && kind <= 4, "ORDER kind"); ++s->order_count; }
        }
    } else if (ty == 16) {
        require(b->n == 40 && r64(p + 12) == s->spu_next && r64(p + 20) == s->spu_next
            && r64(p + 28) == 0 && r32(p + 36) == s->spu_depth && s->order_final, "DOMAIN_FINAL census");
        s->domain_final = 1;
    } else if (ty == 41) { require(!s->window, "WINDOW_BEGIN"); s->window = 1;
    } else if (ty == 42) { require(s->window == 1 && r64(p + 12) == s->lanes[0].frame, "WINDOW_END"); s->window = 2;
    } else if (ty == 6) {
        require(s->domain_final && s->order_final && s->window == 2 && s->lanes[0].written == 12
            && !s->lanes[0].depth && !s->lanes[1].written && s->spu_next == 6 && !s->spu_depth,
            "terminal fixture equations"); s->terminal = 1;
    } else require(ty == 1 || ty == 2 || ty == 3 || ty == 8 || ty == 11 || ty == 12 || ty == 17,
                   "unsupported metadata type");
    if (ty != 5 && ty != 13 && ty != 6) {
        metadata *m; unsigned ordinal = s->meta_count;
        require(ordinal < META_CAP, "metadata count"); m = &s->meta[s->meta_count++];
        m->offset = source.n; m->bytes = b->n + 20; m->index = s->next_chunk; m->type = ty;
        if (is_spu(ty)) { journal *j = entry(s, 1); j->first = j->last = s->next_chunk; j->ordinal = ordinal; }
    }
    require(s->prefix_bytes <= SOURCE_CAP - b->n - 20, "prefix bytes cap");
    ++s->next_chunk; s->prefix_bytes += b->n + 20;
}
static void emit(unsigned ty, buffer *payload)
{
    buffer framed = {{0}, 0}; accepted next = live;
    require(payload->n <= CHUNK_CAP - 20, "chunk cap"); accept(&next, ty, payload);
    u32(&framed, ty); u32(&framed, (uint32_t)payload->n); u64(&framed, live.next_chunk);
    raw(&framed, payload->data, payload->n); u32(&framed, crc(framed.data, framed.n));
    raw(&source, framed.data, framed.n); live = next; payload->n = 0;
}
static void patch64(buffer *b, size_t at, uint64_t value)
{
    unsigned i; require(at <= b->n && 8 <= b->n - at, "checkpoint patch bounds");
    for (i = 0; i < 8; ++i) b->data[at + i] = (unsigned char)(value >> (8 * i));
}
static void section(buffer *out, unsigned kind, unsigned count, const buffer *body)
{
    require(body->n <= CHECKPOINT_CAP && out->n <= CHECKPOINT_CAP - 16
        && body->n <= CHECKPOINT_CAP - out->n - 16, "checkpoint section cap");
    u16(out, kind); u16(out, 1); u32(out, count); u32(out, (uint32_t)body->n); u32(out, 0);
    raw(out, body->data, body->n);
}
/* PFRMIX01 schema1. No host-struct serialization, synthesized original
 * events, standalone-family envelopes, or future declaration imports. */
static void checkpoint(buffer *out, const accepted *s)
{
    buffer body = {{0}, 0}; unsigned i, k, lane_count = 0;
    require(!s->terminal && out->n == 0 && source.n == 16 + s->prefix_bytes, "checkpoint cut frontier");
    raw(out, "PFRMIX01", 8); u16(out, 1); u16(out, 80); u32(out, 4);
    raw(out, source.data, 16); u64(out, s->next_chunk); u64(out, s->prefix_bytes); u64(out, 0);
    u32(out, 3); u32(out, 0); for (i = 0; i < 4; ++i) u16(out, 1); u64(out, 0);

    /* Exact accepted metadata occurs once, in original order. SPU support
     * ordinals below refer to this whole table, including PPU-only rows. */
    for (i = 0; i < s->meta_count; ++i) {
        const metadata *m = &s->meta[i];
        require(m->index < s->next_chunk && (i == 0 || s->meta[i - 1].index < m->index)
            && m->offset <= source.n && m->bytes <= source.n - m->offset, "metadata cut bounds/order");
        require(body.n <= 16384 - 4 && m->bytes <= 16384 - body.n - 4, "metadata byte cap");
        u32(&body, (uint32_t)m->bytes); raw(&body, source.data + m->offset, m->bytes);
    }
    section(out, 1, s->meta_count, &body); body.n = 0;
    for (i = 0; i < 2; ++i) {
        const ppu *l = &s->lanes[i]; unsigned have_frame = (l->flags & 64) != 0;
        if (!l->present) continue;
        ++lane_count;
        u32(&body, 128 + 80 * l->depth + 16 * have_frame); u32(&body, i + 1);
        u64(&body, l->written); u64(&body, 0); u64(&body, 0); u64(&body, 0);
        u64(&body, l->next); u64(&body, l->max_span); u64(&body, l->last_tb);
        u64(&body, l->next_frame); u64(&body, l->frame); u64(&body, l->frame);
        u64(&body, l->last_frame_seq); u64(&body, l->last_frame_tb);
        u32(&body, l->flags); u32(&body, l->depth); u32(&body, have_frame); u32(&body, 0);
        u64(&body, l->record_chunks);
        for (k = 0; k < l->depth; ++k) {
            const ppu_stack *st = &l->stack[k];
            raw(&body, st->begin.bytes, 32); raw(&body, st->parent.bytes, 32);
            u32(&body, have_frame ? 1 : 0); u32(&body, 0); u64(&body, st->frame);
        }
        /* This fixed source admits consecutive frames starting at zero. */
        if (have_frame) { u64(&body, 0); u64(&body, l->frame); }
    }
    section(out, 2, lane_count, &body); body.n = 0;
    /* No explicit frame tokens in this fixture. Ambient frame context must
     * not manufacture unresolved token membership. */
    section(out, 3, 0, &body);
    for (i = 0; i < s->entries_count; ++i) {
        const journal *j = &s->entries[i];
        require(j->first <= j->last && j->last < s->next_chunk, "SPU journal frontier");
        if (j->kind == 1) {
            require(j->ordinal < s->meta_count && is_spu(s->meta[j->ordinal].type)
                && s->meta[j->ordinal].index == j->first && j->first == j->last, "SPU support ordinal");
            u32(&body, 1); u32(&body, 32); u64(&body, j->first); u64(&body, j->last);
            u32(&body, j->ordinal); u32(&body, 0);
        } else {
            require(j->kind == 2 && j->depth <= STACK_CAP && j->chunks && j->events >= j->chunks,
                "SPU run census");
            require(i == 0 || s->entries[i - 1].kind != 2, "canonical merged SPU event blocks");
            u32(&body, 2); u32(&body, 96 + 24 * j->depth); u64(&body, j->first); u64(&body, j->last);
            u32(&body, 1); u32(&body, 0); /* one lane run */
            u32(&body, 1); u32(&body, 1); /* lane1, epoch1 */
            u64(&body, j->first_seq); u64(&body, j->events); u64(&body, j->chunks);
            u32(&body, 1); u32(&body, j->depth); /* known stack, no protocol error */
            for (k = 0; k < 6; ++k) u32(&body, 0);
            for (k = 0; k < j->depth; ++k) {
                const event *e = &j->stack[k];
                u64(&body, e->seq); u32(&body, e->epoch); u32(&body, e->raw); u32(&body, e->desc); u32(&body, e->depth);
            }
        }
    }
    section(out, 4, s->entries_count, &body);
    patch64(out, 48, out->n - 80);
    require(out->n <= CHECKPOINT_CAP - 4, "checkpoint CRC cap"); u32(out, crc(out->data, out->n));
}
static void snapshot(unsigned index)
{
    require(index < CUTS && !live.terminal, "checkpoint after terminal");
    cuts[index].state = live; cuts[index].offset = source.n;
    checkpoint(&cuts[index].checkpoint, &cuts[index].state);
}
static void records(buffer *b, unsigned n) { u32(b, 1); u32(b, n); }
static void rec(buffer *b, unsigned kind, unsigned depth, unsigned desc, uint64_t seq, uint64_t v, uint64_t id)
{ u8(b, kind); u8(b, depth); u16(b, 0); u32(b, desc); u64(b, seq); u64(b, v); u64(b, id); }
static void thread(buffer *b, unsigned inc)
{ u32(b, 1); u32(b, inc); u32(b, inc); u64(b, 1000 + inc); string(b, inc == 1 ? "t1" : "t2"); emit(4, b); }
static void order(buffer *b, unsigned kind, uint64_t a, uint64_t v)
{ u32(b, 1); u32(b, 0); u32(b, kind); u32(b, 1); u64(b, a); u64(b, v); emit(18, b); }
static void bracket(buffer *b, unsigned raw_value, uint64_t t0, uint64_t t1)
{
    unsigned ordinal = live.brackets;
    u32(b, 2); u32(b, 1); u64(b, ordinal); u32(b, 1); u64(b, raw_value); u64(b, t0); u64(b, t1); emit(10, b);
    order(b, 1, UINT64_C(1) | (uint64_t)ordinal << 32, live.spu_next);
}
static void drained(buffer *b, uint64_t tb)
{
    u32(b, 2); u32(b, 0); u32(b, 2); u32(b, 1); u32(b, 1); u32(b, 0); u64(b, live.spu_next);
    u32(b, 3); u32(b, 1); u64(b, tb); u64(b, live.spu_next); emit(18, b);
}
static void events(buffer *b, unsigned n) { u32(b, 1); u32(b, 1); u64(b, live.spu_next); u32(b, n); }
static void ev(buffer *b, unsigned raw_value, unsigned desc, unsigned kind, unsigned depth)
{ u32(b, raw_value); u16(b, desc); u8(b, kind); u8(b, depth); }
static void field(buffer *b, unsigned id, unsigned status, const buffer *value)
{ u16(b, id); u8(b, status); u8(b, (unsigned)value->n); raw(b, value->data, value->n); }
static void profile(buffer *b)
{
    buffer v = {{0}, 0}; unsigned i;
    u16(b, 1); u16(b, 0); u8(b, 8); raw(b, "SPU-TEST", 8); u32(b, 1);
    for (i = 0; i < 32; ++i) u8(b, 9);
    u32(b, 1 | 1 << 3 | 1 << 4 | 1 << 7 | 1 << 9 | 1 << 10); u32(b, 2); u16(b, 7);
    u32(&v, 0); field(b, 1, 3, &v); field(b, 2, 3, &v); v.n = 0;
    u64(&v, 79800000); field(b, 3, 1, &v); v.n = 0;
    u32(&v, 0); field(b, 4, 3, &v); v.n = 0;
    u8(&v, 1); field(b, 5, 2, &v); v.n = 0; u8(&v, 0); field(b, 6, 2, &v); v.n = 0;
    u32(&v, 40); u32(&v, 4); u32(&v, 256); u32(&v, 65536); u64(&v, 16 << 20);
    for (i = 0; i < 4; ++i) u32(&v, 0);
    field(b, 7, 1, &v); emit(17, b);
}
static void final_ppu(buffer *b, unsigned inc, uint64_t written)
{
    unsigned i; u32(b, inc); u8(b, 2); u8(b, 0); u16(b, 0); u64(b, written); u64(b, written);
    for (i = 0; i < 4; ++i) u64(b, 0);
}
static void generate(void)
{
    buffer b = {{0}, 0}; unsigned seg, i;
    raw(&source, "PS3DKPRF", 8); u16(&source, 1); u16(&source, 0); u32(&source, 7 | 512);
    for (seg = 0; seg < SEGMENTS; ++seg) {
        snapshot(seg);
        switch (seg) {
        case 0:
            string(&b, "c-combined"); string(&b, "test-build-0001"); string(&b, "0.26.0");
            string(&b, "gcc 16.1.0"); string(&b, "-O2"); string(&b, "fixture");
            u8(&b, 1); u8(&b, 1); u16(&b, 0); u64(&b, 7); u32(&b, 4096); u32(&b, 8); u32(&b, 16); emit(1, &b); break;
        case 1: u32(&b, 1); u32(&b, 1); u32(&b, 0); u64(&b, 79800000); u64(&b, 1000); emit(2, &b); break;
        case 2: profile(&b); break;
        case 3:
            u32(&b, 3);
            for (i = 0; i < 3; ++i) {
                buffer key = {{0}, 0}; const char *name = i == 0 ? "A" : i == 1 ? "B" : "C";
                raw(&key, "test-build-0001", 15); u8(&key, 0); raw(&key, "main.c", 6); u8(&key, 0);
                u32(&key, 10 + i); raw(&key, name, 1);
                u32(&b, i); u64(&b, fnv(&key)); u8(&b, 1); u8(&b, 0); u32(&b, 10 + i);
                string(&b, name); string(&b, ""); string(&b, "main.c");
            }
            emit(3, &b); break;
        case 4: thread(&b, 1); break;
        case 5:
            u32(&b, 1); u32(&b, 0); string(&b, "spu-build"); u32(&b, 3);
            for (i = 0; i < 3; ++i) {
                u32(&b, i); u32(&b, 0x100 + i); u32(&b, 10 + i);
                string(&b, i == 0 ? "spu.work" : i == 1 ? "spu.ping" : "spu.other"); string(&b, "spu/main.c");
            }
            emit(11, &b);
            u32(&b, 1); u32(&b, 2); u8(&b, 1); u8(&b, 32); u8(&b, 1); u8(&b, 0); u32(&b, 1); u32(&b, 1);
            for (i = 0; i < 4; ++i) u64(&b, 0);
            string(&b, "spu-dec"); emit(8, &b);
            u32(&b, 1); u32(&b, 1); u32(&b, 2); u32(&b, 0); u32(&b, 1); u64(&b, 1); string(&b, "worker"); emit(12, &b);
            order(&b, 4, 1000, 0); break;
        case 6:
            u32(&b, 1); u32(&b, 2); u32(&b, 1); u8(&b, 1); u8(&b, 0); u16(&b, 0); u64(&b, 0); u64(&b, 1001); emit(9, &b);
            bracket(&b, 10000, 1100, 1110); break;
        case 7:
            u16(&b, 1); u16(&b, 0); u32(&b, 1); u32(&b, 0); u64(&b, 0); u8(&b, 1); u8(&b, 1);
            u16(&b, 0); u32(&b, 0); u32(&b, 0); emit(41, &b);
            records(&b, 1); rec(&b, 4, 0, 0, 0, 1000, 0); emit(5, &b); break;
        case 8:
            records(&b, 2); rec(&b, 1, 0, 0, 1, 1010, 1); rec(&b, 2, 0, 0, 2, 0, 0); emit(5, &b);
            events(&b, 1); ev(&b, 9990, 0, 1, 0); emit(13, &b); break;
        case 9:
            thread(&b, 2); records(&b, 4); rec(&b, 1, 1, 1, 3, 1020, 2); rec(&b, 2, 1, 0, 4, 1, 0);
            rec(&b, 3, 1, 1, 5, 1040, 2); rec(&b, 3, 0, 0, 6, 1050, 1); emit(5, &b);
            events(&b, 3); ev(&b, 9980, 1, 1, 1); ev(&b, 9970, 1, 2, 1); ev(&b, 9960, 0, 2, 0); emit(13, &b);
            bracket(&b, 9950, 1200, 1210); drained(&b, 1250); break;
        case 10:
            records(&b, 4); rec(&b, 4, 0, 0, 7, 1100, 1); rec(&b, 1, 0, 2, 8, 1110, 3);
            rec(&b, 2, 0, 0, 9, 0, 0); rec(&b, 3, 0, 2, 10, 1140, 3); emit(5, &b);
            events(&b, 2); ev(&b, 9940, 2, 1, 0); ev(&b, 9930, 2, 2, 0); emit(13, &b);
            bracket(&b, 9920, 1300, 1310); drained(&b, 1350); break;
        case 11:
            records(&b, 1); rec(&b, 4, 0, 0, 11, 1200, 2); emit(5, &b);
            order(&b, 5, live.order_count, 0);
            u32(&b, 1); u32(&b, 1); u8(&b, 1); u8(&b, 1); u16(&b, 0);
            u64(&b, live.spu_next); u64(&b, live.spu_next); u64(&b, 0); u32(&b, 0); emit(16, &b);
            u16(&b, 1); u16(&b, 0); u32(&b, 1); u8(&b, 1); u8(&b, 0); u16(&b, 0); u64(&b, 2);
            for (i = 0; i < 3; ++i) u32(&b, 0);
            emit(42, &b); snapshot(12);
            u32(&b, 0); u32(&b, 2); final_ppu(&b, 1, 12); final_ppu(&b, 2, 0);
            u64(&b, 0); u32(&b, 0); u32(&b, 0); u64(&b, 0); u64(&b, 3); emit(6, &b); break;
        default: require(0, "segment index");
        }
        segment_end[seg] = source.n;
    }
    require(live.terminal && live.order_count == 8 && live.prefix_bytes == source.n - 16, "final frontier");
}

typedef struct { unsigned char *bytes; size_t n, cap; } memory;
static int64_t read_at(void *ctx, uint64_t at, void *p, size_t n)
{ memory *m = ctx; if (at > m->n || n > m->n - (size_t)at) return -1; memcpy(p, m->bytes + (size_t)at, n); return (int64_t)n; }
static int64_t write_at(void *ctx, uint64_t at, const void *p, size_t n)
{ memory *m = ctx; if (at > m->n || n > m->n - (size_t)at) return -1; memcpy(m->bytes + (size_t)at, p, n); return (int64_t)n; }
static int resize_memory(void *ctx, uint64_t n)
{ memory *m = ctx; if (n > m->cap) return -1; m->n = (size_t)n; memset(m->bytes, 0, m->n); return 0; }
static int size_memory(void *ctx, uint64_t *n) { *n = ((memory *)ctx)->n; return 0; }
static int sync_memory(void *ctx) { (void)ctx; return 0; }
static void pack(memory *m)
{
    pfr_config cfg; pfr_layout layout; pfr_view view; pfr_transition transition;
    pfr_io io = {m, read_at, write_at, resize_memory, size_memory, sync_memory};
    pfr_store *store; void *scratch; unsigned i;
    memset(&cfg, 0, sizeof cfg); cfg.max_bytes = FILE_CAP; cfg.retained_slots = 3;
    cfg.checkpoint_bytes = CHECKPOINT_CAP; cfg.payload_bytes = CHUNK_CAP; memset(cfg.session, 0x73, sizeof cfg.session);
    require(pfr_plan(&cfg, &layout) == PFR_OK && layout.file_bytes <= FILE_CAP, "physical layout");
    store = malloc(pfr_state_bytes()); scratch = malloc(layout.scratch_bytes); m->cap = (size_t)layout.file_bytes; m->bytes = malloc(m->cap);
    require(store && scratch && m->bytes, "physical allocation");
    require(pfr_format_v2(store, &io, &cfg, scratch, layout.scratch_bytes) == PFR_OK, "format v2");
    for (i = 0; i < SEGMENTS; ++i) {
        require(segment_end[i] - cuts[i].offset <= CHUNK_CAP, "segment payload cap");
        require(pfr_commit(store, cuts[i].checkpoint.data, cuts[i].checkpoint.n,
            source.data + cuts[i].offset, segment_end[i] - cuts[i].offset) == PFR_OK, "physical commit");
    }
    require(pfr_get_view(store, &view) == PFR_OK && view.first_segment == 9 && view.next_segment == 12
        && view.next_chunk == live.next_chunk && view.bound_payload_bytes == live.prefix_bytes, "physical committed frontier");
    transition.expected_revision = view.generation; transition.next_chunk = live.next_chunk;
    transition.payload_bytes = live.prefix_bytes; transition.state = PFR_SEALED; transition.reason = PFR_REASON_NONE;
    require(pfr_publish_state(store, &transition) == PFR_OK, "physical seal");
    free(scratch); free(store);
}
static char created[16][PATH_CAP];
static unsigned created_count;
static int output(const char *dir, const char *name, const void *p, size_t n)
{
    char path[PATH_CAP]; FILE *f; int len = snprintf(path, sizeof path, "%s/%s", dir, name); int ok;
    if (len < 0 || (size_t)len >= sizeof path || created_count == 16) return 0;
    f = fopen(path, "wbx"); if (!f) { fprintf(stderr, "combined_fixture: cannot exclusively create %s\n", path); return 0; }
    memcpy(created[created_count++], path, (size_t)len + 1);
    ok = fwrite(p, 1, n, f) == n; if (fclose(f)) ok = 0;
    if (!ok) fprintf(stderr, "combined_fixture: short write/close %s\n", path);
    return ok;
}
int main(int argc, char **argv)
{
    memory m = {NULL, 0, 0}; buffer manifest = {{0}, 0}; unsigned i; int ok = 0;
    if (argc != 2) { fputs("usage: rolling-combined-fixture FRESH_OUTPUT_DIRECTORY\n", stderr); return 2; }
    generate(); pack(&m);
    raw(&manifest, "{\"schema\":1,\"cuts\":[", strlen("{\"schema\":1,\"cuts\":["));
    for (i = 0; i < CUTS; ++i) {
        char text[768], name[32]; const accepted *s = &cuts[i].state; int n;
        (void)snprintf(name, sizeof name, "cut-%03u.ckp", i);
        n = snprintf(text, sizeof text,
            "%s{\"file\":\"%s\",\"kind\":\"%s\",\"next_chunk\":%" PRIu64 ",\"prefix_bytes\":%" PRIu64
            ",\"source_offset\":%zu,\"ppu_written\":%" PRIu64 ",\"ppu_record_chunks\":%" PRIu64
            ",\"ppu_stack\":%u,\"spu_events\":%" PRIu64 ",\"spu_stack\":%u,\"metadata\":%u,\"journal\":%u}",
            i ? "," : "", name, i == 12 ? "pre_trailer_probe" : "segment_start", s->next_chunk,
            s->prefix_bytes, cuts[i].offset, s->lanes[0].written, s->lanes[0].record_chunks,
            s->lanes[0].depth, s->spu_next, s->spu_depth, s->meta_count, s->entries_count);
        require(n >= 0 && (size_t)n < sizeof text, "manifest line cap"); raw(&manifest, text, (size_t)n);
    }
    raw(&manifest, "]}\n", 3);
    if (!output(argv[1], "source.prof", source.data, source.n) || !output(argv[1], "rolling.profr", m.bytes, m.n)) goto done;
    for (i = 0; i < CUTS; ++i) {
        char name[32]; (void)snprintf(name, sizeof name, "cut-%03u.ckp", i);
        if (!output(argv[1], name, cuts[i].checkpoint.data, cuts[i].checkpoint.n)) goto done;
    }
    if (!output(argv[1], "cuts.json", manifest.data, manifest.n)) goto done;
    printf("C fixture: %" PRIu64 " original chunks, %zu bytes, 12 physical segments; 13 independent PFRMIX01 checkpoints\n",
           live.next_chunk, source.n); ok = 1;
done:
    free(m.bytes);
    if (!ok) { while (created_count) (void)remove(created[--created_count]); }
    return ok ? 0 : 1;
}
