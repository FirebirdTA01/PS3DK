/* Owner FIFO accounting. Bounded, allocation-free, single owner. This is
 * command storage/publication evidence, never GPU execution or utilization.
 * All owner-buffer words count (including CALL and profiler commands); external
 * CALL targets do not. Unknown owner cursor/PUT flow is sticky until a new
 * explicitly established baseline. No modular subtraction infers a lap. */
#ifndef PS3DK_PROF_FIFO_ACCOUNT_H
#define PS3DK_PROF_FIFO_ACCOUNT_H
#include <stdint.h>
#include <string.h>

enum {
    FIFO_ACCOUNT_FLOW = 1, FIFO_ACCOUNT_BOUNDARY = 2,
    FIFO_ACCOUNT_TOKEN = 4, FIFO_ACCOUNT_WINDOW = 8,
    FIFO_ACCOUNT_SATURATED = 16, FIFO_ACCOUNT_OFFSET = 32
};
typedef struct fifo_account_row {
    uint64_t frame, window, prepared, published, wraps, seq;
    uint32_t owner, reasons;
} fifo_account_row;
typedef struct fifo_account {
    uint64_t prepared, published, wraps, lap_base;
    uint64_t start_prepared, start_published, start_wraps, frame, window;
    uint32_t begin, end, current, tail, phase, reasons, active, owner;
} fifo_account;

static inline void fifo_account_invalidate(fifo_account *s, uint32_t reason)
{
    s->reasons |= reason;
}
static inline int fifo_account_offset(const fifo_account *s, uint32_t off)
{
    return !(off & 3u) && off >= s->begin && off <= s->end;
}
static inline void fifo_account_add(fifo_account *s, uint64_t *v, uint64_t n)
{
    if (n > UINT64_MAX - *v) {
        *v = UINT64_MAX;
        fifo_account_invalidate(s, FIFO_ACCOUNT_SATURATED);
    } else *v += n;
}
/* Baseline must be one known linear lap with published <= prepared. */
static inline void fifo_account_init(fifo_account *s, uint32_t begin, uint32_t end,
                                      uint32_t current, uint32_t published)
{
    memset(s, 0, sizeof *s);
    s->begin = begin; s->end = end; s->current = current;
    if ((begin | end) & 3u || begin >= end || end >= 0x20000000u ||
        !fifo_account_offset(s, current) || !fifo_account_offset(s, published) || published > current) {
        fifo_account_invalidate(s, FIFO_ACCOUNT_OFFSET);
        return;
    }
    s->prepared = current - begin;
    s->published = published - begin;
}
static inline void fifo_account_prepare(fifo_account *s, uint32_t current)
{
    if (s->reasons) return;
    if (s->phase || !fifo_account_offset(s, current) || current < s->current) {
        fifo_account_invalidate(s, FIFO_ACCOUNT_FLOW);
        return;
    }
    fifo_account_add(s, &s->prepared, current - s->current);
    s->current = current;
}
static inline void fifo_account_publish(fifo_account *s, uint32_t published)
{
    uint64_t next;
    if (s->reasons) return;
    if (s->phase || !fifo_account_offset(s, published) || published > s->current) {
        fifo_account_invalidate(s, FIFO_ACCOUNT_FLOW);
        return;
    }
    next = s->lap_base;
    fifo_account_add(s, &next, published - s->begin);
    if (next < s->published || next > s->prepared) {
        fifo_account_invalidate(s, FIFO_ACCOUNT_FLOW);
        return;
    }
    s->published = next;
}
/* Native phases 1..6 only, all required. The actual JUMP store precedes
 * PHASE1_BEGIN(2), PUT=tail excludes it, PUT=begin at PHASE2_BEGIN(4)
 * includes it. Tail slack is never charged. A successful EXIT counts a wrap. */
static inline void fifo_account_wrap(fifo_account *s, unsigned kind, uint32_t begin, uint32_t tail)
{
    if (s->reasons) return;
    if (kind != s->phase + 1u || kind > 6 || begin != s->begin ||
        !fifo_account_offset(s, tail) || (kind != 1 && tail != s->tail)) {
        fifo_account_invalidate(s, FIFO_ACCOUNT_BOUNDARY);
        return;
    }
    if (kind == 1) {
        fifo_account_prepare(s, tail);
        s->tail = tail;
    } else if (kind == 2) {
        uint64_t before = s->prepared;
        fifo_account_add(s, &s->prepared, 4);
        s->published = before;
    } else if (kind == 4) {
        s->published = s->prepared;
    } else if (kind == 6) {
        s->lap_base = s->prepared;
        s->current = s->begin;
        fifo_account_add(s, &s->wraps, 1);
    }
    s->phase = kind == 6 ? 0 : kind;
}
static inline void fifo_account_begin(fifo_account *s, uint32_t owner, uint64_t frame,
                                      uint64_t window, uint32_t current)
{
    if (s->active) fifo_account_invalidate(s, FIFO_ACCOUNT_BOUNDARY);
    fifo_account_prepare(s, current);
    s->active = 1; s->owner = owner; s->frame = frame; s->window = window;
    s->start_prepared = s->prepared; s->start_published = s->published; s->start_wraps = s->wraps;
    if (!owner) fifo_account_invalidate(s, FIFO_ACCOUNT_TOKEN);
}
static inline fifo_account_row fifo_account_end(fifo_account *s, uint32_t owner, uint64_t frame,
                                                uint64_t window, uint32_t current)
{
    fifo_account_row row;
    uint32_t reasons = 0;
    memset(&row, 0, sizeof row);
    if (!s->active) reasons |= FIFO_ACCOUNT_BOUNDARY;
    if (s->active && (owner != s->owner || frame != s->frame)) reasons |= FIFO_ACCOUNT_TOKEN;
    if (s->active && window != s->window) reasons |= FIFO_ACCOUNT_WINDOW;
    fifo_account_prepare(s, current);
    row.owner = s->active ? s->owner : owner;
    row.frame = s->active ? s->frame : frame;
    row.window = s->active ? s->window : window;
    row.reasons = reasons | s->reasons;
    if (!row.reasons) {
        row.prepared = s->prepared - s->start_prepared;
        row.published = s->published - s->start_published;
        row.wraps = s->wraps - s->start_wraps;
    }
    s->active = 0;
    return row;
}
static inline void fifo_account_le(uint8_t *b, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) b[i] = (uint8_t)(v >> (8u * i));
}
/* Type43, schema1, 80 bytes. Flags: COMPLETE=1, INCLUDES_PROFILER=2.
 * Unavailable rows carry nonzero reasons and zero numerical counters. */
static inline void fifo_account_encode(uint8_t b[80], const fifo_account_row *r,
                                       uint32_t instance, uint32_t generation, uint32_t domain)
{
    memset(b, 0, 80);
    fifo_account_le(b, 1, 2); fifo_account_le(b + 2, r->reasons ? 2 : 3, 2);
    fifo_account_le(b + 4, instance, 4); fifo_account_le(b + 8, generation, 4);
    fifo_account_le(b + 12, domain, 4); fifo_account_le(b + 16, r->owner, 4);
    fifo_account_le(b + 20, r->reasons, 4); fifo_account_le(b + 24, r->frame, 8);
    fifo_account_le(b + 32, r->window, 8); fifo_account_le(b + 40, r->prepared, 8);
    fifo_account_le(b + 48, r->published, 8); fifo_account_le(b + 56, r->wraps, 8);
    fifo_account_le(b + 64, r->seq, 8);
}
#endif
