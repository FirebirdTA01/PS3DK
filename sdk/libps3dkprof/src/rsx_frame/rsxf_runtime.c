/* RSX frame capture runtime (rsxf.h). Dormant unless the executable defines ps3dk_prof_automatic_rsx_frame_options:
 * then every hook returns after one load of `S` (NULL) and nothing is allocated, decoded or written.
 *
 * Slot word (rsxcap.h layout, whole-word CAS by every actor):
 *   IDLE(e) -requester-> CLAIMING(e+1) -requester-> REQUESTED(e+1) -owner boundary-> ARMING -> ARMED
 *   ARMED -owner boundary-> CLOSING -drainer, after END-> IDLE(e+1)
 *   REQUESTED -owner, no capture window after 3 boundaries-> IDLE (abandoned, nothing written)
 *   any state -drainer at seal-> IDLE (END ABANDONED when a BEGIN was written)
 * Staging blocks: allocated by the requester before REQUESTED, owned block by block by the owner (FILLING) and the
 * drainer (SEALED), freed by the drainer after END (or by the owner when a request is abandoned before arming). */
#include "rsxf.h"
#include "../core/prof_internal.h"
#include "../rsx/rsxcap.h"
#include "../rsx/gpu_port_gcm.h"
#include "../auto_rsx/auto_ring_commands.h"
#include <ps3dk/prof_auto.h>
#include <string.h>

extern const ps3dk_prof_rsx_frame_options ps3dk_prof_automatic_rsx_frame_options __attribute__((weak));

enum { BIND_SLOTS = 64, PROGRAMS = 512, LABELS = 256, SCOPES = 8, BLOCKS_MAX = 16, UNIT_MAX = 512 };
enum { B_FREE = 0, B_FILLING = 1, B_SEALED = 2 };

typedef struct bind { uint32_t offset, bytes; const void *ucode; } bind;
typedef struct program { uint64_t hash, key; uint32_t index, profile; } program;
typedef struct label { uint64_t hash; uint32_t index, n; char text[RSXF_TEXT_MAX]; } label;
typedef struct block {
    uint8_t *base;
    uint32_t state;              /* atomic: B_* */
    uint32_t used, records, seq; /* owner while FILLING, drainer while SEALED */
    uint32_t draws, programs, labels;
} block;

typedef struct rsxf_state {
    rsxf_decoder dec;
    void *ctx;
    uintptr_t cursor;
    int ringed;
    uint32_t block_bytes, nblocks;
    /* owner: labels */
    const char *label, *scope[SCOPES];
    uint32_t depth, depth_excess;
    /* owner: bound programs by offset */
    bind binds[BIND_SLOTS];
    /* owner: the covered draw between begin and end */
    struct { uint32_t active, claimed, kind, mode, first, count, offset, index_type, location, clear_mask, tag;
             uint64_t caller, op_id; uint32_t column; } hook;
    uint32_t last_column;
    /* requester -> owner (written before REQUESTED) */
    uint32_t req_source, req_id;
    uint32_t name_len;
    char name[RSXF_NAME_MAX];
    block blocks[BLOCKS_MAX];
    /* owner while ARMED */
    uint32_t armed, window_waits, window_asked, cur, next_seq;
    uint64_t frame, words_at_arm;
    uint32_t draws_seen, hooked, decoded_only, mismatches, untimed, truncated_at, labels_refused, programs_refused;
    uint32_t nprograms, nlabels;
    uint64_t bytes_streamed, bytes_dropped;
    /* the records of sealed blocks: what END counts (an open block at seal is not written) */
    uint32_t sealed_draws, sealed_programs, sealed_labels;
    uint64_t sealed_bytes;
    program programs[PROGRAMS];
    label labels[LABELS];
    uint64_t vp_key;             /* (start, generation) of the cached VP index */
    uint32_t vp_index, fp_addr, fp_index, fp_cached, vp_cached;
    /* owner -> drainer */
    uint8_t begin_buf[RSXF_BEGIN_FIXED + 64];
    uint32_t begin_len, begin_ready;  /* begin_ready atomic */
    uint8_t end_buf[RSXF_END_BYTES];
    uint32_t end_ready;               /* atomic */
    /* drainer */
    uint32_t begin_emitted, drain_seq, blocks_emitted;
    /* status for the live link (single-word atomics) */
    uint32_t st_id, st_request, st_state, st_reason, st_draws;
    uint64_t st_frame, st_bytes;
} rsxf_state;

/* Published once at attach (release); NULL: dormant or not attached. auto_runtime.c tests it with one load. */
void *ps3dk_prof__rsxf_active;
#define S (*(rsxf_state **)(void *)&ps3dk_prof__rsxf_active)
static uint64_t g_slot;          /* RSXCAP_WORD layout */
static uint32_t g_selected;      /* 0 unknown, 1 no, 2 yes */
static uint32_t g_config_emitted;
static ps3dk_prof_desc g_window_desc = {"rsx-frame", "", "<ps3dk-rsxf>", 1u, 1u, 0u, 0u};

static uint32_t opt_kb(void)
{
    uint32_t kb = ps3dk_prof_automatic_rsx_frame_options.staging_kb;
    return kb ? kb : 256u;
}
static uint32_t opt_blocks(void)
{
    uint32_t b = ps3dk_prof_automatic_rsx_frame_options.blocks;
    return b ? b : 2u;
}

int rsxf_selected(void)
{
    uint32_t s = LD_RLX(&g_selected);
    if (s)
        return s == 2;
    s = 1;
    if (&ps3dk_prof_automatic_rsx_frame_options) {
        const ps3dk_prof_rsx_frame_options *o = &ps3dk_prof_automatic_rsx_frame_options;
        uint32_t kb = opt_kb(), b = opt_blocks();
        if (o->size == sizeof *o && !o->reserved && kb >= 4u && kb <= 1024u && b >= 2u && b <= BLOCKS_MAX)
            s = 2;
    }
    ST_RLX(&g_selected, s);
    return s == 2;
}

static void status(uint32_t state, uint32_t reason)
{
    ST_RLX(&S->st_reason, reason);
    ST_REL(&S->st_state, state);
}

void rsxf_attach(void *ctx, int ringed)
{
    rsxf_state *s;
    if (!rsxf_selected())
        return;
    if ((s = LD_ACQ(&S)) != NULL) {
        /* an override attach (ps3dk_prof_auto_attach after cellGcmInit) replaces the options, the ring with them;
         * it runs on the owner before any covered operation, so nothing is armed yet */
        s->ctx = ctx;
        s->ringed = ringed;
        s->cursor = ctx ? (uintptr_t)(void *)((gpp_ctx *)ctx)->current : 0;
        return;
    }
    s = pal_alloc(sizeof *s);
    if (!s)
        return; /* CONFIG then records the decoder as unavailable */
    memset(s, 0, sizeof *s);
    ps3dk_prof__rsxf_decode_init(&s->dec);
    s->ctx = ctx;
    s->ringed = ringed;
    /* decoding starts at the context's current word: everything the owner writes from attach on is seen */
    s->cursor = ctx ? (uintptr_t)(void *)((gpp_ctx *)ctx)->current : 0;
    s->block_bytes = opt_kb() * 1024u;
    s->nblocks = opt_blocks();
    ST_REL(&S, s);
}

/* ---- staging (owner) ---------------------------------------------------------------------------------------------- */

static void seal_current(rsxf_state *s)
{
    block *b = &s->blocks[s->cur];
    if (LD_RLX(&b->state) != B_FILLING || !b->records)
        return;
    b->seq = s->next_seq++;
    s->sealed_draws += b->draws;
    s->sealed_programs += b->programs;
    s->sealed_labels += b->labels;
    s->sealed_bytes += b->used - RSXF_DRAWS_HEADER;
    ST_REL(&b->state, B_SEALED);
}

static int take_block(rsxf_state *s)
{
    for (uint32_t k = 1; k <= s->nblocks; k++) {
        uint32_t i = (s->cur + k) % s->nblocks;
        if (LD_ACQ(&s->blocks[i].state) == B_FREE) {
            block *b = &s->blocks[i];
            b->used = RSXF_DRAWS_HEADER;
            b->records = b->draws = b->programs = b->labels = 0;
            ST_RLX(&b->state, B_FILLING);
            s->cur = i;
            return 1;
        }
    }
    return 0;
}

/* One unit (definitions then the draw) into the current block, or a new one; 0 when no block is free. */
static int append(rsxf_state *s, const uint8_t *unit, uint32_t len, uint32_t records, uint32_t draws,
                  uint32_t programs, uint32_t labels)
{
    block *b = &s->blocks[s->cur];
    if (s->truncated_at != RSXF_NO_ORDINAL)
        return 0;
    if (LD_RLX(&b->state) != B_FILLING || b->used + len > s->block_bytes) {
        seal_current(s);
        if (!take_block(s))
            return 0;
        b = &s->blocks[s->cur];
    }
    memcpy(b->base + b->used, unit, len);
    b->used += len;
    b->records += records;
    b->draws += draws;
    b->programs += programs;
    b->labels += labels;
    s->bytes_streamed += len;
    return 1;
}

/* ---- identities (owner, armed) ---------------------------------------------------------------------------------- */

static program *find_program(rsxf_state *s, uint32_t profile, uint64_t hash, uint64_t key)
{
    for (uint32_t i = 0; i < s->nprograms; i++) {
        program *p = &s->programs[i];
        if (p->profile == profile && (hash ? p->hash == hash : (!p->hash && p->key == key)))
            return p;
    }
    return NULL;
}

/* The program index for (profile, hash or key); a new PROGRAM record is appended to `unit` when it is new. */
static uint32_t program_index(rsxf_state *s, uint8_t *unit, uint32_t *len, uint32_t *defs, uint32_t profile,
                              const rsxf_hash *h, uint32_t source, uint32_t flags, uint32_t address, uint64_t key,
                              uint32_t ordinal)
{
    uint64_t hash = (flags & RSXF_PF_UNREADABLE) ? 0 : h->hash;
    program *p = find_program(s, profile, hash, key);
    rsxf_wire_program w;
    if (p)
        return p->index;
    if (s->nprograms >= PROGRAMS) {
        s->programs_refused++;
        return 0;
    }
    memset(&w, 0, sizeof w);
    w.index = s->nprograms + 1u;
    w.profile = (uint8_t)profile;
    w.source = (uint8_t)source;
    w.flags = (uint16_t)(flags | (h && h->unterminated && !(flags & RSXF_PF_UNREADABLE) ? RSXF_PF_UNTERMINATED : 0));
    if (!(flags & RSXF_PF_UNREADABLE)) {
        w.instructions = h->instructions;
        w.hash = h->hash;
        w.ucode_bytes = h->bytes;
        w.const_blocks = profile == RSXF_PROFILE_FP ? h->const_blocks : 0;
    }
    w.address = address;
    w.first_draw = ordinal;
    *len += (uint32_t)ps3dk_prof__rsxf_put_program(unit + *len, UNIT_MAX - *len, &w);
    (*defs)++;
    p = &s->programs[s->nprograms++];
    p->hash = hash;
    p->key = key;
    p->index = w.index;
    p->profile = profile;
    return w.index;
}

static uint32_t vp_identity(rsxf_state *s, uint8_t *unit, uint32_t *len, uint32_t *defs, uint32_t ordinal,
                            uint32_t *unknown)
{
    rsxf_decoder *d = &s->dec;
    uint32_t start, n;
    uint64_t key;
    rsxf_hash h;
    if (!(d->known & 1u) || d->vp_start >= RSXF_VP_SLOTS) {
        *unknown = 1;
        return 0;
    }
    start = d->vp_start;
    key = (uint64_t)start << 48 | (d->vp_generation & 0xffffffffffffull);
    if (s->vp_cached && s->vp_key == key)
        return s->vp_index;
    /* the slots from start through the last-instruction bit must all be known (the hash reads them in place) */
    for (n = 0; start + n < RSXF_VP_SLOTS; n++) {
        if (!d->vp_known[start + n]) {
            *unknown = 1;
            return 0;
        }
        if (d->vp[start + n][3] & 1u) {
            n++;
            break;
        }
    }
    ps3dk_prof__rsxf_vp_hash(&d->vp[start][0], n, &h);
    s->vp_index = program_index(s, unit, len, defs, RSXF_PROFILE_VP, &h, RSXF_SRC_STREAM, 0, start, key, ordinal);
    s->vp_key = key;
    s->vp_cached = s->vp_index != 0;
    return s->vp_index;
}

static uint32_t fp_offset(uint32_t v) { return v & 0x1ffffffcu; }

static uint32_t fp_identity(rsxf_state *s, uint8_t *unit, uint32_t *len, uint32_t *defs, uint32_t ordinal,
                            uint32_t *unknown)
{
    rsxf_decoder *d = &s->dec;
    uint32_t off;
    const bind *b;
    rsxf_hash h;
    if (!(d->known & 2u)) {
        *unknown = 1;
        return 0;
    }
    if (s->fp_cached && s->fp_addr == d->fp_address)
        return s->fp_index;
    off = fp_offset(d->fp_address);
    b = &s->binds[(off >> 6) % BIND_SLOTS];
    if (b->ucode && fp_offset(b->offset) == off) {
        ps3dk_prof__rsxf_fp_hash(b->ucode, b->bytes, &h);
        s->fp_index = program_index(s, unit, len, defs, RSXF_PROFILE_FP, &h, RSXF_SRC_CONTAINER, 0, d->fp_address,
                                    off, ordinal);
    } else {
        memset(&h, 0, sizeof h);
        s->fp_index = program_index(s, unit, len, defs, RSXF_PROFILE_FP, &h, RSXF_SRC_MEMORY, RSXF_PF_UNREADABLE,
                                    d->fp_address, off, ordinal);
    }
    s->fp_addr = d->fp_address;
    s->fp_cached = s->fp_index != 0;
    return s->fp_index;
}

static uint32_t label_index(rsxf_state *s, uint8_t *unit, uint32_t *len, uint32_t *defs)
{
    char path[RSXF_TEXT_MAX];
    uint32_t n = 0;
    uint64_t h;
    const char *parts[SCOPES + 1];
    uint32_t np = 0;
    for (uint32_t i = 0; i < s->depth && i < SCOPES; i++)
        if (s->scope[i] && s->scope[i][0])
            parts[np++] = s->scope[i];
    if (s->label && s->label[0])
        parts[np++] = s->label;
    if (!np)
        return 0;
    for (uint32_t i = 0; i < np && n < RSXF_TEXT_MAX; i++) {
        if (i && n < RSXF_TEXT_MAX)
            path[n++] = '/';
        for (const char *c = parts[i]; *c && n < RSXF_TEXT_MAX; c++)
            path[n++] = *c;
    }
    h = ps3dk_prof__rsxf_fnv(RSXF_FNV_OFFSET, path, n);
    for (uint32_t i = 0; i < s->nlabels; i++)
        if (s->labels[i].hash == h && s->labels[i].n == n && !memcmp(s->labels[i].text, path, n))
            return s->labels[i].index;
    if (s->nlabels >= LABELS) {
        s->labels_refused++;
        return 0;
    }
    {
        label *l = &s->labels[s->nlabels];
        size_t w = ps3dk_prof__rsxf_put_text(unit + *len, UNIT_MAX - *len, RSXF_REC_LABEL, s->nlabels + 1u, path, n);
        if (!w) {
            s->labels_refused++;
            return 0;
        }
        *len += (uint32_t)w;
        (*defs)++;
        l->hash = h;
        l->n = n;
        memcpy(l->text, path, n);
        l->index = ++s->nlabels;
        return l->index;
    }
}

/* ---- draw records (owner, armed) ---------------------------------------------------------------------------------- */

static uint32_t untimed_of(uint32_t column)
{
    if ((column >= 2 && column <= 11) || column == RSXF_UNTIMED_OUTSIDE_RANGE || column == RSXF_UNTIMED_RING_FULL ||
        column == RSXF_UNTIMED_RING_STATE)
        return column;
    return RSXF_UNTIMED_NO_WINDOW;
}

static void record(rsxf_state *s, const rsxf_draw_event *e, int hooked)
{
    uint8_t unit[UNIT_MAX];
    uint32_t len = 0, defs_p = 0, defs_l = 0, unknown = 0, programs_before = s->nprograms, labels_before = s->nlabels;
    rsxf_wire_draw w;
    uint32_t ordinal = s->draws_seen++;
    memset(&w, 0, sizeof w);
    w.ordinal = ordinal;
    if (hooked) {
        w.kind = (uint16_t)s->hook.kind;
        w.mode = s->hook.kind == RSXF_K_CLEAR ? 0 : s->hook.mode;
        w.first = s->hook.first;
        w.count = s->hook.kind == RSXF_K_CLEAR ? 0 : s->hook.count;
        if (s->hook.kind == RSXF_K_INDEX) {
            w.index_offset = s->hook.offset;
            w.index_type = (uint8_t)(s->hook.index_type & 1u);
            w.index_location = (uint8_t)(s->hook.location & 1u);
        }
        w.clear_mask = s->hook.kind == RSXF_K_CLEAR ? s->hook.clear_mask : 0;
        w.flags = RSXF_DF_HOOKED | (e ? RSXF_DF_DECODED : 0);
        if (e && (e->kind != s->hook.kind || (e->kind != RSXF_K_CLEAR && e->count != s->hook.count))) {
            w.flags |= RSXF_DF_MISMATCH;
            s->mismatches++;
        }
        w.caller = s->hook.caller;
        w.tag = s->hook.tag;
        w.op_id = s->hook.op_id;
        w.untimed = w.op_id ? 0 : untimed_of(s->hook.column);
        s->hooked++;
    } else {
        w.kind = (uint16_t)e->kind;
        w.mode = e->kind == RSXF_K_CLEAR ? 0 : (e->mode > 10 ? 0 : e->mode);
        w.first = e->kind == RSXF_K_ARRAY || e->kind == RSXF_K_INDEX ? e->first : 0;
        w.count = e->kind == RSXF_K_INLINE_ARRAY || e->kind == RSXF_K_IMMEDIATE || e->kind == RSXF_K_CLEAR ? 0 : e->count;
        if (e->kind == RSXF_K_INDEX) {
            w.index_offset = e->index_offset;
            w.index_location = (uint8_t)e->index_location;
        }
        if (e->kind == RSXF_K_INDEX || e->kind == RSXF_K_INLINE_INDEX)
            w.index_type = (uint8_t)e->index_type;
        w.clear_mask = e->kind == RSXF_K_CLEAR ? e->clear_mask : 0;
        w.flags = RSXF_DF_DECODED;
        w.untimed = RSXF_UNTIMED_NOT_HOOKED;
        s->decoded_only++;
        if (e->kind != RSXF_K_CLEAR && !w.mode) /* a group without a valid mode: keep the record readable */
            w.mode = 1;
    }
    if (w.untimed)
        s->untimed++;
    w.words = e ? e->words : 0;
    w.tb = p_tb();
    if (w.kind != RSXF_K_CLEAR) {
        w.vp = vp_identity(s, unit, &len, &defs_p, ordinal, &unknown);
        w.fp = fp_identity(s, unit, &len, &defs_p, ordinal, &unknown);
    }
    w.vp_start = (s->dec.known & 1u) ? s->dec.vp_start : 0;
    w.fp_address = (s->dec.known & 2u) ? s->dec.fp_address : 0;
    w.label = label_index(s, unit, &len, &defs_l);
    if (unknown)
        w.flags |= RSXF_DF_STATE_UNKNOWN;
    len += (uint32_t)ps3dk_prof__rsxf_put_draw(unit + len, UNIT_MAX - len, &w);
    if (!append(s, unit, len, defs_p + defs_l + 1u, 1, defs_p, defs_l)) {
        /* no block: this draw and every later one are counted only; definitions made for it are undone */
        if (s->truncated_at == RSXF_NO_ORDINAL)
            s->truncated_at = ordinal;
        s->bytes_dropped += len;
        s->nprograms = programs_before;
        s->nlabels = labels_before;
        s->vp_cached = s->fp_cached = 0;
    }
    ST_RLX(&s->st_draws, s->draws_seen);
    ST_RLX(&s->st_bytes, s->bytes_streamed);
}

static void events(rsxf_state *s)
{
    rsxf_decoder *d = &s->dec;
    for (uint32_t i = 0; i < d->n_events; i++) {
        const rsxf_draw_event *e = &d->events[i];
        if (!s->armed)
            continue;
        if (s->hook.active && !s->hook.claimed) {
            s->hook.claimed = 1;
            record(s, e, 1);
        } else {
            record(s, e, 0);
        }
    }
    d->n_events = 0;
}

/* ---- owner hooks ---------------------------------------------------------------------------------------------- */

static rsxf_state *owner_state(void *ctx)
{
    rsxf_state *s = LD_ACQ(&S);
    return s && (!ctx || ctx == s->ctx) ? s : NULL;
}

void rsxf_scan(void *ctx)
{
    rsxf_state *s = owner_state(ctx);
    gpp_ctx *c = ctx;
    uintptr_t begin, cur;
    if (!s || !c)
        return;
    begin = (uintptr_t)(void *)c->begin;
    cur = (uintptr_t)(void *)c->current;
    if (!s->cursor || s->cursor < begin || s->cursor > cur || ((cur - s->cursor) & 3u)) {
        if (s->cursor)
            ps3dk_prof__rsxf_decode_unknown(&s->dec, 0, AUTO_RING_REJECT_CURSOR);
        s->cursor = cur;
        return;
    }
    (void)ps3dk_prof__rsxf_decode(&s->dec, (const uint32_t *)(void *)s->cursor, (cur - s->cursor) / 4u);
    s->cursor = cur;
    events(s);
}

void rsxf_wrap(void *ctx)
{
    rsxf_state *s = owner_state(ctx);
    if (!s)
        return;
    rsxf_scan(ctx);
    s->cursor = (uintptr_t)(void *)((gpp_ctx *)ctx)->begin; /* the wrap resumes at begin; its JUMP is skipped */
}

void rsxf_reset(void *ctx)
{
    rsxf_state *s = owner_state(ctx);
    if (!s)
        return;
    ps3dk_prof__rsxf_decode_unknown(&s->dec, 0, AUTO_RING_REJECT_CURSOR);
    s->cursor = 0;
}

void rsxf_hook_begin(void *ctx, uint32_t kind, uint32_t mode, uint32_t first, uint32_t count, uint32_t offset,
                     uint32_t index_type, uint32_t location, uint32_t clear_mask, uint64_t caller, uint32_t tag)
{
    rsxf_state *s = owner_state(ctx);
    if (!s)
        return;
    rsxf_scan(ctx);
    s->last_column = 0;
    if (!s->armed)
        return;
    s->hook.active = 1;
    s->hook.claimed = 0;
    s->hook.kind = kind;
    s->hook.mode = mode;
    s->hook.first = first;
    s->hook.count = count;
    s->hook.offset = offset;
    s->hook.index_type = index_type;
    s->hook.location = location;
    s->hook.clear_mask = clear_mask;
    s->hook.caller = caller;
    s->hook.tag = tag;
    s->hook.op_id = 0;
    s->hook.column = 0;
}

void rsxf_note_column(uint32_t c)
{
    rsxf_state *s = LD_ACQ(&S);
    if (s && c != 1u)
        s->last_column = c;
}

void rsxf_hook_admitted(uint64_t op_id)
{
    rsxf_state *s = LD_ACQ(&S);
    if (!s || !s->hook.active)
        return;
    s->hook.op_id = op_id;
    s->hook.column = op_id ? 0 : s->last_column;
}

void rsxf_hook_end(void *ctx)
{
    rsxf_state *s = owner_state(ctx);
    if (!s)
        return;
    rsxf_scan(ctx);
    if (s->hook.active && !s->hook.claimed && s->armed)
        record(s, NULL, 1);
    s->hook.active = 0;
}

int rsxf_forced(void)
{
    rsxf_state *s = LD_ACQ(&S);
    return s && s->armed;
}

void ps3dk_prof__rsxf_bind(void *ctx, const void *ucode, uint32_t bytes, uint32_t offset);
void ps3dk_prof__rsxf_bind(void *ctx, const void *ucode, uint32_t bytes, uint32_t offset)
{
    rsxf_state *s = owner_state(ctx);
    bind *b;
    if (!s || !ucode || !bytes)
        return;
    b = &s->binds[(fp_offset(offset) >> 6) % BIND_SLOTS];
    b->offset = offset;
    b->bytes = bytes;
    b->ucode = ucode;
    if (s->fp_cached && fp_offset(s->fp_addr) == fp_offset(offset))
        s->fp_cached = 0; /* the same address may now hold another program */
}

/* ---- labels (owner thread; the AUTO owner check is ps3dk_prof__auto_owner_self) --------------------------------- */

int ps3dk_prof__auto_owner_self(void);

void ps3dk_prof_draw_label(const char *l)
{
    rsxf_state *s = LD_ACQ(&S);
    if (s && ps3dk_prof__auto_owner_self())
        s->label = l;
}
void ps3dk_prof_draw_scope_begin(const char *n)
{
    rsxf_state *s = LD_ACQ(&S);
    if (!s || !ps3dk_prof__auto_owner_self())
        return;
    if (s->depth < SCOPES)
        s->scope[s->depth++] = n;
    else
        s->depth_excess++;
}
void ps3dk_prof_draw_scope_end(void)
{
    rsxf_state *s = LD_ACQ(&S);
    if (!s || !ps3dk_prof__auto_owner_self())
        return;
    if (s->depth_excess)
        s->depth_excess--;
    else if (s->depth) {
        s->depth--;
        s->label = NULL; /* a label belongs to the scope it was set in */
    }
}

/* ---- requests (any thread) ---------------------------------------------------------------------------------------- */

static void free_staging(rsxf_state *s)
{
    for (uint32_t i = 0; i < BLOCKS_MAX; i++) {
        if (s->blocks[i].base)
            pal_free(s->blocks[i].base);
        s->blocks[i].base = NULL;
        ST_RLX(&s->blocks[i].state, B_FREE);
    }
}

static int request(uint32_t source, uint32_t request_id, uint32_t flags, const char *name, uint32_t name_len,
                   uint32_t *id_out)
{
    rsxf_state *s = LD_ACQ(&S);
    uint64_t w;
    uint32_t e;
    if (!rsxf_selected() || !s || !s->ringed)
        return PS3DK_PROF_ENOTSUP;
    if (flags || name_len > RSXF_NAME_MAX || (name_len && (!name || memchr(name, 0, name_len))))
        return PS3DK_PROF_EINVAL;
    if (!p_ready())
        return PS3DK_PROF_ENOTINIT;
    if (LD_ACQ(&g_prof->stop_latch))
        return PS3DK_PROF_ESTOPPED;
    w = LD_ACQ(&g_slot);
    if (RSXCAP_STATE(w) != RSXCAP_IDLE || (w & RSXCAP_F_STOP))
        return (w & RSXCAP_F_STOP) ? PS3DK_PROF_ESTOPPED : PS3DK_PROF_EBUSY;
    e = RSXCAP_EPOCH(w) + 1u;
    if (!__atomic_compare_exchange_n(&g_slot, &w, RSXCAP_WORD(RSXCAP_CLAIMING, 0, e, 0, 0), 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return PS3DK_PROF_EBUSY;
    for (uint32_t i = 0; i < s->nblocks; i++) {
        s->blocks[i].base = pal_alloc(s->block_bytes);
        if (!s->blocks[i].base) {
            free_staging(s);
            ST_REL(&g_slot, RSXCAP_WORD(RSXCAP_IDLE, 0, e, 0, 0));
            return PS3DK_PROF_ENOMEM;
        }
        ST_RLX(&s->blocks[i].state, B_FREE);
    }
    s->req_source = source;
    s->req_id = request_id;
    s->name_len = name_len;
    if (name_len)
        memcpy(s->name, name, name_len);
    ST_RLX(&s->st_id, e);
    ST_RLX(&s->st_request, request_id);
    ST_RLX(&s->st_draws, 0u);
    ST_RLX(&s->st_frame, 0ull);
    ST_RLX(&s->st_bytes, 0ull);
    status(5u, 0u);
    ST_REL(&g_slot, RSXCAP_WORD(RSXCAP_REQUESTED, 0, e, 0, 0));
    *id_out = e;
    return PS3DK_PROF_OK;
}

int ps3dk_prof_rsx_frame_capture(const char *name, uint32_t flags)
{
    uint32_t id = 0, n = 0;
    int rc;
    if (name)
        while (name[n] && n <= RSXF_NAME_MAX)
            n++;
    rc = request(2u, 0u, flags, name, n, &id);
    return rc == PS3DK_PROF_OK ? (int)id : rc;
}

/* ---- frame boundary (owner) ---------------------------------------------------------------------------------------- */

static void arm(rsxf_state *s, uint64_t w, uint64_t frame, uint64_t tb, uint32_t owner, uint32_t window)
{
    uint32_t e = RSXCAP_EPOCH(w);
    rsxf_wire_begin b;
    uint64_t want = w;
    if (!__atomic_compare_exchange_n(&g_slot, &want, RSXCAP_WORD(RSXCAP_ARMING, 0, e, 0, 0), 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;
    s->frame = frame;
    s->draws_seen = s->hooked = s->decoded_only = s->mismatches = s->untimed = 0;
    s->labels_refused = s->programs_refused = s->nprograms = s->nlabels = 0;
    s->truncated_at = RSXF_NO_ORDINAL;
    s->bytes_streamed = s->bytes_dropped = s->sealed_bytes = 0;
    s->sealed_draws = s->sealed_programs = s->sealed_labels = 0;
    s->vp_cached = s->fp_cached = 0;
    s->next_seq = 0;
    s->cur = s->nblocks - 1u;
    s->words_at_arm = s->dec.words;
    s->hook.active = 0;
    (void)take_block(s);
    memset(&b, 0, sizeof b);
    b.id = e;
    b.source = (uint16_t)s->req_source;
    b.request = s->req_id;
    b.owner = owner;
    b.frame = frame;
    b.window = window;
    b.tb = tb;
    b.known = s->dec.known & 3u;
    b.vp_start = s->dec.vp_start;
    b.fp_address = s->dec.fp_address;
    b.name = s->name;
    b.name_len = s->name_len;
    s->begin_len = (uint32_t)ps3dk_prof__rsxf_put_begin(s->begin_buf, sizeof s->begin_buf, &b);
    ST_RLX(&s->end_ready, 0u);
    ST_RLX(&s->begin_emitted, 0u);
    ST_REL(&s->begin_ready, 1u);
    ST_RLX(&s->st_frame, frame);
    s->armed = 1;
    status(1u, 0u);
    ST_REL(&g_slot, RSXCAP_WORD(RSXCAP_ARMED, 0, e, 0, 0));
}

/* END from the sealed blocks only: every count matches the records the drainer writes. */
static void build_end(rsxf_state *s, uint32_t id, uint16_t outcome, uint16_t reason)
{
    rsxf_wire_end e;
    memset(&e, 0, sizeof e);
    e.id = id;
    e.outcome = outcome;
    e.reason = reason;
    e.blocks = s->next_seq;
    e.draws_seen = s->draws_seen;
    e.draws_recorded = s->sealed_draws;
    e.hooked = s->hooked;
    e.decoded_only = s->decoded_only;
    e.mismatches = s->mismatches;
    e.truncated_at = outcome == RSXF_OUT_TRUNCATED ? s->truncated_at : RSXF_NO_ORDINAL;
    e.programs = s->sealed_programs;
    e.labels = s->sealed_labels;
    e.labels_refused = s->labels_refused;
    e.programs_refused = s->programs_refused;
    e.bytes_streamed = s->sealed_bytes;
    e.bytes_dropped = s->bytes_dropped + (s->bytes_streamed - s->sealed_bytes);
    e.tb_end = p_tb();
    e.decoder_words = s->dec.words - s->words_at_arm;
    e.reject_reason = s->dec.bad_reason;
    e.reject_header = s->dec.bad_reason ? s->dec.bad_header : 0;
    e.untimed = s->untimed;
    (void)ps3dk_prof__rsxf_put_end(s->end_buf, sizeof s->end_buf, &e);
}

static void close_capture(rsxf_state *s, uint64_t w)
{
    uint32_t e = RSXCAP_EPOCH(w);
    uint64_t want = w;
    int truncated = s->truncated_at != RSXF_NO_ORDINAL;
    seal_current(s);
    if (LD_RLX(&s->blocks[s->cur].state) == B_FILLING)
        ST_REL(&s->blocks[s->cur].state, B_FREE); /* an empty open block */
    build_end(s, e, truncated ? RSXF_OUT_TRUNCATED : RSXF_OUT_DONE, truncated ? RSXF_R_STAGING : RSXF_R_NONE);
    s->armed = 0;
    s->hook.active = 0;
    ST_REL(&s->end_ready, 1u);
    status(2u, truncated ? RSXF_R_STAGING : 0u);
    (void)__atomic_compare_exchange_n(&g_slot, &want, RSXCAP_WORD(RSXCAP_CLOSING, 0, e, 0, 0), 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE);
}

void rsxf_frame(uint64_t frame, uint64_t tb, uint32_t owner, int window_open, uint32_t window_id)
{
    rsxf_state *s = LD_ACQ(&S);
    uint64_t w;
    if (!s)
        return;
    s->label = NULL;
    s->depth = s->depth_excess = 0;
    w = LD_ACQ(&g_slot);
    if (RSXCAP_STATE(w) == RSXCAP_ARMED) {
        close_capture(s, w);
        return;
    }
    if (RSXCAP_STATE(w) != RSXCAP_REQUESTED)
        return;
    if (window_open) {
        s->window_waits = s->window_asked = 0;
        arm(s, w, frame, tb, owner, window_id);
        return;
    }
    if (!s->window_asked) {
        /* a capture window from the next frame mark; EBUSY (one is opening) is waited for the same way */
        (void)ps3dk_prof_capture_begin(&g_window_desc, PS3DK_PROF_WINDOW_FRAMES, 2u);
        s->window_asked = 1;
        return;
    }
    if (++s->window_waits >= 3u) {
        uint64_t want = w;
        s->window_waits = s->window_asked = 0;
        if (__atomic_compare_exchange_n(&g_slot, &want, RSXCAP_WORD(RSXCAP_IDLE, 0, RSXCAP_EPOCH(w), 0, 0), 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            free_staging(s);
            status(4u, RSXF_R_CANCELLED);
        }
    }
}

/* ---- drainer -------------------------------------------------------------------------------------------------------- */

static int emit_blocks(rsxf_state *s, rsxf_emit_fn emit, void *user, uint32_t id)
{
    for (;;) {
        block *b = NULL;
        for (uint32_t i = 0; i < s->nblocks; i++)
            if (LD_ACQ(&s->blocks[i].state) == B_SEALED && s->blocks[i].seq == s->drain_seq)
                b = &s->blocks[i];
        if (!b)
            return 1;
        (void)ps3dk_prof__rsxf_put_draws_header(b->base, RSXF_DRAWS_HEADER, id, b->seq, b->records,
                                                b->used - RSXF_DRAWS_HEADER);
        if (emit(user, RSXF_CHUNK_DRAWS, b->base, b->used) != 0)
            return 0;
        s->drain_seq++;
        s->blocks_emitted++;
        ST_REL(&b->state, B_FREE);
    }
}

void rsxf_drain(rsxf_emit_fn emit, void *user, int auto_available, int seal)
{
    rsxf_state *s = LD_ACQ(&S);
    uint64_t w;
    uint32_t st, id;
    if (!rsxf_selected())
        return;
    if (!LD_RLX(&g_config_emitted)) {
        uint8_t b[RSXF_CONFIG_BYTES];
        rsxf_wire_config c;
        c.availability = !auto_available ? RSXF_NO_AUTO : !s ? RSXF_NO_DECODER : !s->ringed ? RSXF_NO_RING
                         : RSXF_AVAILABLE;
        c.block_bytes = opt_kb() * 1024u;
        c.blocks = opt_blocks();
        c.programs = PROGRAMS;
        c.labels = LABELS;
        c.label_bytes = RSXF_TEXT_MAX;
        if (!ps3dk_prof__rsxf_put_config(b, sizeof b, &c) || emit(user, RSXF_CHUNK_CONFIG, b, sizeof b) != 0)
            return;
        ST_RLX(&g_config_emitted, 1u);
    }
    if (!s)
        return;
    w = LD_ACQ(&g_slot);
    st = RSXCAP_STATE(w);
    id = RSXCAP_EPOCH(w);
    if (LD_ACQ(&s->begin_ready) && !s->begin_emitted) {
        if (emit(user, RSXF_CHUNK_BEGIN, s->begin_buf, s->begin_len) != 0)
            return;
        s->begin_emitted = 1;
        s->drain_seq = 0;
        s->blocks_emitted = 0;
    }
    if (s->begin_emitted && !emit_blocks(s, emit, user, id))
        return;
    if (st == RSXCAP_CLOSING && LD_ACQ(&s->end_ready) && s->drain_seq == s->next_seq) {
        if (emit(user, RSXF_CHUNK_END, s->end_buf, RSXF_END_BYTES) != 0)
            return;
        free_staging(s);
        ST_RLX(&s->begin_ready, 0u);
        s->begin_emitted = 0;
        status(3u, LD_RLX(&s->st_reason));
        ST_REL(&g_slot, RSXCAP_WORD(RSXCAP_IDLE, 0, id, 0, 0));
        return;
    }
    if (seal && st != RSXCAP_IDLE) {
        /* the owner has quiesced: an armed capture ends here with the blocks already sealed (the open block's
         * records are dropped, so every count matches the emitted records), a request not yet armed is withdrawn */
        if (s->begin_emitted && (st == RSXCAP_ARMED || st == RSXCAP_ARMING)) {
            build_end(s, id, RSXF_OUT_ABANDONED, RSXF_R_SHUTDOWN);
            if (emit(user, RSXF_CHUNK_END, s->end_buf, RSXF_END_BYTES) != 0)
                return;
        }
        s->armed = 0;
        free_staging(s);
        status(4u, RSXF_R_SHUTDOWN);
        ST_REL(&g_slot, RSXCAP_WORD(RSXCAP_IDLE, RSXCAP_F_STOP, id, 0, 0));
    }
}

/* ---- live link ------------------------------------------------------------------------------------------------- */

static int live_request(uint32_t request_id, uint32_t flags, const char *name, uint32_t name_len, uint32_t *id)
{
    if (!request_id)
        return PS3DK_PROF_EINVAL;
    return request(1u, request_id, flags, name, name_len, id);
}
static void live_status(rsxf_status *o)
{
    rsxf_state *s = LD_ACQ(&S);
    memset(o, 0, sizeof *o);
    if (!s)
        return;
    o->state = LD_ACQ(&s->st_state);
    o->id = LD_RLX(&s->st_id);
    o->request = LD_RLX(&s->st_request);
    o->reason = LD_RLX(&s->st_reason);
    o->draws = LD_RLX(&s->st_draws);
    o->frame = LD_RLX(&s->st_frame);
    o->bytes = LD_RLX(&s->st_bytes);
}
static int live_available(void)
{
    rsxf_state *s = LD_ACQ(&S);
    return rsxf_selected() && s && s->ringed;
}
static const rsxf_live_ops g_live_ops = {live_request, live_status, live_available};
const rsxf_live_ops *ps3dk_prof__rsxf_live_ops(void) { return rsxf_selected() ? &g_live_ops : NULL; }
