/* RSX frame capture runtime (rsxf.h). Dormant unless the executable defines ps3dk_prof_automatic_rsx_frame_options
 * and AUTO attached with the report ring: until then every hook returns after one load of `S` (NULL) and nothing is
 * allocated, decoded or written.
 *
 * Slot word (rsxcap.h layout, whole-word CAS by every actor; no store ever overwrites a word it did not observe):
 *   IDLE(e) -requester-> CLAIMING(e+1) -requester-> REQUESTED(e+1) -owner boundary-> ARMING -> ARMED
 *   ARMED -owner boundary-> CLOSING -drainer, after END-> IDLE(e+1)
 *   REQUESTED -owner, no capture window after 3 boundaries-> ARMING -> IDLE (abandoned, nothing written)
 * Shutdown (the sealing drainer) adds F_STOP to whatever word it observes. From then on:
 *   - the requester refuses (ESTOPPED); one in CLAIMING fails its publish CAS, frees what it allocated and leaves IDLE;
 *   - the owner never touches a block again once it has observed STOP (every record, arm and boundary checks it) and
 *     says so with F_OWNER_OUT; the drainer says it is done with F_DRAINER_OUT; whichever of the two leaves second
 *     frees the staging. Neither waits for the other: an owner that never runs again keeps the blocks allocated
 *     until the process exits, it is never freed under it.
 * END: the owner's own counters when it closed or left the capture; otherwise (shutdown with the owner still inside)
 * the drainer's counts of the blocks it wrote, so every END count matches the records in the file.
 * Staging blocks: allocated by the requester before REQUESTED, owned block by block by the owner (FILLING) and the
 * drainer (SEALED). The side that frees them does so before it releases the slot to IDLE. */
#include "rsxf.h"
#include "../core/prof_internal.h"
#include "../rsx/rsxcap.h"
#include "../rsx/gpu_port_gcm.h"
#include "../auto_rsx/auto_ring_commands.h"
#include <ps3dk/prof_auto.h>
#include <string.h>

extern const ps3dk_prof_rsx_frame_options ps3dk_prof_automatic_rsx_frame_options __attribute__((weak));

enum { BIND_SLOTS = 64, FP_CACHE = 32, FP_SAMPLE = 48, PROGRAMS = 512, LABELS = 256, SCOPES = 8, BLOCKS_MAX = 16,
       UNIT_MAX = 512 };
enum { B_FREE = 0, B_FILLING = 1, B_SEALED = 2 };
#define F_STOP RSXCAP_F_STOP
#define F_OWNER_OUT 0x400ull   /* the owner observed STOP and will not touch a block again */
#define F_DRAINER_OUT 0x800ull /* the sealing drainer is done with the blocks */
#define WORD(st, flags, e) RSXCAP_WORD((st), (flags), (e), 0, 0)

/* The FP program bound at an offset: hashed in the bind call, never read again. flags: RSXF_PF_NOT_REVALIDATED (bound
 * before the capture was armed) and RSXF_PF_UNREADABLE (identity unavailable). */
typedef struct bind { uint32_t offset, valid, flags, armed_seq; rsxf_hash h; } bind;
/* Containers hashed before, by (ucode, offset, size) and sampled bytes: a rebind outside a capture costs a lookup. */
typedef struct fp_cached { const void *ucode; uint32_t offset, bytes; uint8_t sample[FP_SAMPLE]; rsxf_hash h; } fp_cached;
typedef struct program { uint64_t hash, key; uint32_t index, profile, flags; } program;
typedef struct label { uint64_t hash; uint32_t index, n; char text[RSXF_TEXT_MAX]; } label;
typedef struct block {
    uint8_t *base;
    uint32_t state;              /* atomic: B_* */
    uint32_t used, records, seq; /* owner while FILLING, drainer while SEALED */
    uint32_t draws, programs, labels, states, baseline; /* baseline: BASELINE register pairs */
} block;

typedef struct rsxf_state {
    rsxf_decoder dec;
    void *ctx;
    uintptr_t cursor;
    uint32_t block_bytes, nblocks;
    /* owner: labels */
    const char *label, *scope[SCOPES];
    uint32_t depth, depth_excess;
    /* owner: bound programs by offset, and the hash cache */
    bind binds[BIND_SLOTS];
    fp_cached fpc[FP_CACHE];
    uint64_t fp_hashed, fp_reused; /* bind statistics (host tests) */
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
    uint32_t armed, arm_seq, window_waits, window_asked, cur, next_seq;
    uint64_t frame, words_at_arm;
    uint32_t draws_seen, hooked, decoded_only, mismatches, untimed, truncated_at, labels_refused, programs_refused;
    uint32_t nprograms, nlabels, lost_at_arm;
    uint64_t bytes_streamed, bytes_dropped;
    /* the decoder's part of END, fixed when the owner closes or leaves (it keeps decoding afterwards) */
    uint64_t end_words;
    uint32_t end_reject_reason, end_reject_header, end_lost;
    /* the records of sealed blocks: what END counts (an open block at seal is not written) */
    uint32_t sealed_draws, sealed_programs, sealed_labels, sealed_states, sealed_baseline;
    uint64_t sealed_bytes;
    /* the register state the records describe so far (owner, armed): a STATE record carries what differs */
    uint32_t emit_val[RSXF_REGS];
    uint32_t emit_known[RSXF_REG_WORDS];
    uint32_t emit_cleared; /* dec.regs_cleared when emit_known was last valid */
    program programs[PROGRAMS];
    label labels[LABELS];
    uint64_t vp_key;             /* (start, generation) of the cached VP index */
    uint32_t vp_index, fp_addr, fp_index, fp_cached, vp_cached;
    /* owner -> drainer */
    uint8_t begin_buf[RSXF_BEGIN_FIXED + 64];
    uint32_t begin_len, begin_ready;  /* begin_ready atomic */
    uint8_t end_buf[RSXF_END_BYTES];
    uint32_t end_ready;               /* atomic */
    /* drainer: the blocks it wrote for the open capture */
    uint32_t begin_emitted, drain_seq, blocks_emitted, d_draws, d_programs, d_labels, d_states, d_baseline;
    uint64_t d_bytes;
    /* the latest LIVE request's status for the live link (single-word atomics; app requests leave it alone) */
    uint32_t st_id, st_request, st_state, st_reason, st_draws;
    uint64_t st_frame, st_bytes;
} rsxf_state;

/* Published at attach (release) when the report ring is attached; NULL: dormant. auto_runtime.c tests it with one
 * load. The state, once allocated, is kept (g_state) so a later attach without the ring can unpublish it safely. */
void *ps3dk_prof__rsxf_active;
#define S (*(rsxf_state **)(void *)&ps3dk_prof__rsxf_active)
static rsxf_state *g_state;
static uint32_t g_ringless;      /* the last attach had no report ring */
static uint64_t g_slot;          /* RSXCAP_WORD layout */
static uint32_t g_selected;      /* 0 unknown, 1 no, 2 yes */
static uint32_t g_config_emitted;
static uint32_t g_drain_sealed;  /* drainer: the seal ran; nothing is written after it */
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

static uint64_t slot_load(void) { return LD_ACQ(&g_slot); }
static int slot_cas(uint64_t *seen, uint64_t to)
{
    return __atomic_compare_exchange_n(&g_slot, seen, to, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* The live link's status, only for a live request (written before the slot word moves on, so a later request never
 * sees an earlier capture's write land on its own id). */
static void status(rsxf_state *s, uint32_t state, uint32_t reason)
{
    if (s->req_source != 1u)
        return;
    ST_RLX(&s->st_reason, reason);
    ST_REL(&s->st_state, state);
}

static void on_event(void *user, const rsxf_draw_event *e);

void rsxf_attach(void *ctx, int ringed)
{
    rsxf_state *s = LD_ACQ(&g_state);
    if (!rsxf_selected())
        return;
    if (!ringed) {
        /* no report ring: dormant (an override attach without the ring unpublishes; nothing is armed yet) */
        ST_RLX(&g_ringless, 1u);
        ST_REL(&S, NULL);
        return;
    }
    ST_RLX(&g_ringless, 0u);
    if (!s) {
        s = pal_alloc(sizeof *s);
        if (!s)
            return; /* CONFIG then records the decoder as unavailable */
        memset(s, 0, sizeof *s);
        ps3dk_prof__rsxf_decode_init(&s->dec);
        s->dec.sink = on_event;
        s->dec.sink_user = s;
        s->block_bytes = opt_kb() * 1024u;
        s->nblocks = opt_blocks();
        ST_REL(&g_state, s);
    }
    /* an override attach (ps3dk_prof_auto_attach after cellGcmInit) runs on the owner before any covered operation:
     * nothing is armed yet. Decoding starts at the context's current word: everything written from here on is seen. */
    s->ctx = ctx;
    s->cursor = ctx ? (uintptr_t)(void *)((gpp_ctx *)ctx)->current : 0;
    ST_REL(&S, s);
}

/* ---- staging (owner) ---------------------------------------------------------------------------------------------- */

static void free_staging(rsxf_state *s)
{
    for (uint32_t i = 0; i < BLOCKS_MAX; i++) {
        if (s->blocks[i].base)
            pal_free(s->blocks[i].base);
        s->blocks[i].base = NULL;
        ST_RLX(&s->blocks[i].state, B_FREE);
    }
}

static void seal_current(rsxf_state *s)
{
    block *b = &s->blocks[s->cur];
    if (LD_RLX(&b->state) != B_FILLING || !b->records)
        return;
    b->seq = s->next_seq++;
    s->sealed_draws += b->draws;
    s->sealed_programs += b->programs;
    s->sealed_labels += b->labels;
    s->sealed_states += b->states;
    s->sealed_baseline += b->baseline;
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
            b->records = b->draws = b->programs = b->labels = b->states = b->baseline = 0;
            ST_RLX(&b->state, B_FILLING);
            s->cur = i;
            return 1;
        }
    }
    return 0;
}

/* Room for `len` bytes of records in the current block, or a new one: where to write them, or NULL when no block is
 * free or they would not fit an empty block (records never span blocks). Nothing is counted until commit(). */
static uint8_t *reserve(rsxf_state *s, uint32_t len)
{
    block *b = &s->blocks[s->cur];
    if (s->truncated_at != RSXF_NO_ORDINAL || len > s->block_bytes - RSXF_DRAWS_HEADER)
        return NULL;
    if (LD_RLX(&b->state) != B_FILLING || b->used + len > s->block_bytes) {
        seal_current(s);
        if (!take_block(s))
            return NULL;
        b = &s->blocks[s->cur];
    }
    return b->base + b->used;
}

typedef struct counts { uint32_t records, draws, programs, labels, states, baseline; } counts;

static void commit(rsxf_state *s, uint32_t len, const counts *c)
{
    block *b = &s->blocks[s->cur];
    b->used += len;
    b->records += c->records;
    b->draws += c->draws;
    b->programs += c->programs;
    b->labels += c->labels;
    b->states += c->states;
    b->baseline += c->baseline;
    s->bytes_streamed += len;
}

/* ---- register records (owner) ------------------------------------------------------------------------------------- */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* Writes the pairs of `want` (a register bitmap) as records of `type` from `at`, RSXF_REG_PAIRS_MAX pairs each (a STATE
 * reset with no pair still gets one record). Returns the bytes; `records` the records. */
static uint32_t put_reg_records(rsxf_state *s, uint8_t *at, uint16_t type, uint32_t ordinal, uint32_t reset,
                                const uint32_t *want, uint32_t n, uint32_t *records)
{
    uint8_t *p = at, *head = NULL;
    uint32_t in = 0, left = n, recs = 0;
    for (uint32_t w = 0; w < RSXF_REG_WORDS && left; w++) {
        for (uint32_t bits = want[w]; bits; bits &= bits - 1u) {
            uint32_t i = w * 32u + (uint32_t)__builtin_ctz(bits);
            if (!head || in == RSXF_REG_PAIRS_MAX) {
                uint32_t take = left < RSXF_REG_PAIRS_MAX ? left : RSXF_REG_PAIRS_MAX;
                head = p;
                memset(head, 0, RSXF_REG_HEADER);
                put16(head, type);
                put16(head + 2, RSXF_REG_HEADER + 8u * take);
                put32(head + 4, ordinal);
                put16(head + 8, take);
                put16(head + 10, recs == 0 ? reset : 0u);
                p += RSXF_REG_HEADER;
                in = 0;
                recs++;
            }
            put16(p, i);
            put16(p + 2, 0);
            put32(p + 4, s->dec.reg[i]);
            p += 8;
            in++;
            left--;
        }
    }
    if (!recs && reset) {
        memset(p, 0, RSXF_REG_HEADER);
        put16(p, type);
        put16(p + 2, RSXF_REG_HEADER);
        put32(p + 4, ordinal);
        put16(p + 10, reset);
        p += RSXF_REG_HEADER;
        recs = 1;
    }
    *records = recs;
    return (uint32_t)(p - at);
}

static uint32_t reg_bytes(uint32_t n, uint32_t reset)
{
    uint32_t recs = (n + RSXF_REG_PAIRS_MAX - 1u) / RSXF_REG_PAIRS_MAX;
    if (!recs && reset)
        recs = 1;
    return recs * RSXF_REG_HEADER + 8u * n;
}

/* The registers a STATE record must carry at this draw: known, written since the last record, and different from
 * what the records already say (every known one after a decoder reset). */
static uint32_t state_delta(rsxf_state *s, uint32_t *want, uint32_t *reset)
{
    const rsxf_decoder *d = &s->dec;
    uint32_t n = 0;
    *reset = d->regs_cleared != s->emit_cleared ? RSXF_STATE_RESET : 0u;
    for (uint32_t w = 0; w < RSXF_REG_WORDS; w++) {
        uint32_t cand = d->reg_dirty[w] & d->reg_known[w], keep = 0;
        for (uint32_t bits = cand; bits; bits &= bits - 1u) {
            uint32_t b = (uint32_t)__builtin_ctz(bits), i = w * 32u + b;
            if (*reset || !(s->emit_known[w] & (1u << b)) || s->emit_val[i] != d->reg[i])
                keep |= 1u << b;
        }
        want[w] = keep;
        n += (uint32_t)__builtin_popcount(keep);
    }
    return n;
}

/* After the records were written: they now describe the shadow. */
static void state_emitted(rsxf_state *s, const uint32_t *want, uint32_t reset)
{
    rsxf_decoder *d = &s->dec;
    if (reset)
        memset(s->emit_known, 0, sizeof s->emit_known);
    for (uint32_t w = 0; w < RSXF_REG_WORDS; w++) {
        for (uint32_t bits = want[w]; bits; bits &= bits - 1u)
            s->emit_val[w * 32u + (uint32_t)__builtin_ctz(bits)] = d->reg[w * 32u + (uint32_t)__builtin_ctz(bits)];
        s->emit_known[w] |= want[w];
    }
    memset(d->reg_dirty, 0, sizeof d->reg_dirty);
    s->emit_cleared = d->regs_cleared;
}

/* At arming: every known register as BASELINE records, the first records of the capture. 0 when they do not fit. */
static int put_baseline(rsxf_state *s)
{
    rsxf_decoder *d = &s->dec;
    uint32_t n = 0;
    for (uint32_t w = 0; w < RSXF_REG_WORDS; w++)
        n += (uint32_t)__builtin_popcount(d->reg_known[w]);
    s->emit_cleared = d->regs_cleared;
    memset(s->emit_known, 0, sizeof s->emit_known);
    memset(d->reg_dirty, 0, sizeof d->reg_dirty);
    /* each record on its own (a block may end between them) */
    for (uint32_t w = 0, done = 0; done < n;) {
        uint32_t want[RSXF_REG_WORDS], take = 0, recs = 0, len;
        uint8_t *p;
        counts c;
        memset(want, 0, sizeof want);
        for (; w < RSXF_REG_WORDS && take < RSXF_REG_PAIRS_MAX; w++) {
            uint32_t bits = d->reg_known[w] & ~s->emit_known[w];
            while (bits && take < RSXF_REG_PAIRS_MAX) {
                uint32_t b = bits & (0u - bits);
                want[w] |= b;
                bits &= bits - 1u;
                take++;
            }
            if (bits)
                break; /* this word continues in the next record */
        }
        len = reg_bytes(take, 0);
        if (!(p = reserve(s, len)))
            return 0;
        (void)put_reg_records(s, p, RSXF_REC_BASELINE, 0, 0, want, take, &recs);
        memset(&c, 0, sizeof c);
        c.records = recs;
        c.baseline = take;
        commit(s, len, &c);
        for (uint32_t k = 0; k < RSXF_REG_WORDS; k++) {
            for (uint32_t bits = want[k]; bits; bits &= bits - 1u)
                s->emit_val[k * 32u + (uint32_t)__builtin_ctz(bits)] = d->reg[k * 32u + (uint32_t)__builtin_ctz(bits)];
            s->emit_known[k] |= want[k];
        }
        done += take;
    }
    return 1;
}

/* The decoder's END counters as of now (owner): what END reports, not what the owner decodes after it. */
static void snap_decoder(rsxf_state *s)
{
    s->end_words = s->dec.words - s->words_at_arm;
    s->end_reject_reason = s->dec.bad_reason;
    s->end_reject_header = s->dec.bad_reason ? s->dec.bad_header : 0;
    s->end_lost = s->dec.lost_events - s->lost_at_arm;
}

/* The owner leaves a stopped capture (F_STOP observed in ARMING or ARMED): it never touches a block again. When the
 * drainer already left, the owner frees the staging and releases the slot; otherwise it marks F_OWNER_OUT and the
 * drainer frees. */
static void owner_leave(rsxf_state *s)
{
    uint64_t w = slot_load();
    s->armed = 0;
    s->hook.active = 0;
    for (;;) {
        uint32_t st = RSXCAP_STATE(w);
        if (!(w & F_STOP) || (st != RSXCAP_ARMING && st != RSXCAP_ARMED) || (w & F_OWNER_OUT))
            return;
        status(s, 4u, RSXF_R_SHUTDOWN); /* over a CLOSED or ARMED this owner wrote before it saw STOP */
        snap_decoder(s);                /* published by the CAS below for the drainer's END */
        if (w & F_DRAINER_OUT) {
            if (slot_cas(&w, WORD(RSXCAP_IDLE, F_STOP, RSXCAP_EPOCH(w)))) {
                free_staging(s); /* STOP stays set: no request can claim the slot again */
                return;
            }
        } else if (slot_cas(&w, w | F_OWNER_OUT)) {
            return;
        }
    }
}

/* 1 when shutdown stopped the capture (the owner then left it). */
static int owner_stopped(rsxf_state *s)
{
    if (!(slot_load() & F_STOP))
        return 0;
    owner_leave(s);
    return 1;
}

/* ---- identities (owner, armed) ---------------------------------------------------------------------------------- */

static program *find_program(rsxf_state *s, uint32_t profile, uint64_t hash, uint64_t key, uint32_t flags)
{
    for (uint32_t i = 0; i < s->nprograms; i++) {
        program *p = &s->programs[i];
        if (p->profile == profile && p->flags == flags && (hash ? p->hash == hash : (!p->hash && p->key == key)))
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
    program *p = find_program(s, profile, hash, key, flags);
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
    p->flags = flags;
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
    if (b->valid && fp_offset(b->offset) == off) {
        /* a bind before this capture was armed: its identity is not revalidated against the bytes bound now */
        uint32_t f = b->flags | (b->armed_seq != s->arm_seq ? RSXF_PF_NOT_REVALIDATED : 0u);
        s->fp_index = program_index(s, unit, len, defs, RSXF_PROFILE_FP, &b->h, RSXF_SRC_CONTAINER, f,
                                    d->fp_address, off, ordinal);
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
    /* "scope/scope/label", valid UTF-8, cut on a character boundary at RSXF_TEXT_MAX bytes */
    for (uint32_t i = 0; i < np; i++) {
        if (i) {
            if (n + 1u >= RSXF_TEXT_MAX)
                break;
            path[n++] = '/';
        }
        n = ps3dk_prof__rsxf_utf8_append(path, n, RSXF_TEXT_MAX, parts[i], RSXF_TEXT_MAX);
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
    uint32_t len = 0, defs_p = 0, defs_l = 0, unknown = 0, programs_before, labels_before;
    rsxf_wire_draw w;
    uint32_t ordinal;
    if (owner_stopped(s))
        return; /* shutdown: no block is touched again */
    programs_before = s->nprograms;
    labels_before = s->nlabels;
    ordinal = s->draws_seen++;
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
    {
        /* the draw's STATE records (what changed since the last draw) right before its definitions and DRAW, in the
         * same block */
        uint32_t want[RSXF_REG_WORDS], reset, n = state_delta(s, want, &reset), sbytes = reg_bytes(n, reset), recs = 0;
        uint8_t *p = reserve(s, sbytes + len);
        if (p) {
            counts c;
            (void)put_reg_records(s, p, RSXF_REC_STATE, ordinal, reset, want, n, &recs);
            memcpy(p + sbytes, unit, len);
            memset(&c, 0, sizeof c);
            c.records = recs + defs_p + defs_l + 1u;
            c.draws = 1;
            c.programs = defs_p;
            c.labels = defs_l;
            c.states = recs;
            commit(s, sbytes + len, &c);
            state_emitted(s, want, reset);
        } else {
            /* no block: this draw and every later one are counted only; definitions made for it are undone */
            if (s->truncated_at == RSXF_NO_ORDINAL)
                s->truncated_at = ordinal;
            s->bytes_dropped += sbytes + len;
            s->nprograms = programs_before;
            s->nlabels = labels_before;
            s->vp_cached = s->fp_cached = 0;
        }
    }
    if (s->req_source == 1u) {
        ST_RLX(&s->st_draws, s->draws_seen);
        ST_RLX(&s->st_bytes, s->bytes_streamed);
    }
}

/* The decoder's sink: each completed draw or clear at once, with the registers of that draw. */
static void on_event(void *user, const rsxf_draw_event *e)
{
    rsxf_state *s = user;
    if (!s->armed)
        return;
    if (s->hook.active && !s->hook.claimed) {
        s->hook.claimed = 1;
        record(s, e, 1);
    } else {
        record(s, e, 0);
    }
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

void rsxf_hook_admitted(void *ctx, uint64_t op_id)
{
    rsxf_state *s = owner_state(ctx);
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

/* Up to FP_SAMPLE bytes of the ucode (its first, middle and last 16) that a cached hash must still match. */
static void fp_sample(const uint8_t *u, uint32_t bytes, uint8_t out[FP_SAMPLE])
{
    memset(out, 0, FP_SAMPLE);
    memcpy(out, u, bytes < 16u ? bytes : 16u);
    if (bytes >= 32u)
        memcpy(out + 16, u + ((bytes / 2u) & ~15u), 16);
    if (bytes >= 48u)
        memcpy(out + 32, u + bytes - 16u, 16);
}

/* The program container's ucode bound at `offset` (owner, inside the application's bind call): hashed here, so the
 * capture never reads application memory later (a container freed or reused after its bind cannot be misread).
 * While a capture is armed every bind hashes every instruction byte (exact identity). Outside a capture a container
 * bound before with the same address, offset and size whose sampled bytes still match reuses its hash; one whose
 * sampled bytes differ is identity unavailable (no hash outside a capture for a rewritten container). Either way a
 * draw of the captured frame that uses a bind from before arming carries RSXF_PF_NOT_REVALIDATED. */
void ps3dk_prof__rsxf_bind(void *ctx, const void *ucode, uint32_t bytes, uint32_t offset);
void ps3dk_prof__rsxf_bind(void *ctx, const void *ucode, uint32_t bytes, uint32_t offset)
{
    rsxf_state *s = owner_state(ctx);
    uint8_t sample[FP_SAMPLE];
    fp_cached *c;
    bind *b;
    uint32_t flags = 0;
    int same;
    if (!s || !ucode || !bytes)
        return;
    rsxf_scan(ctx); /* the draws before this bind keep the program bound before it */
    fp_sample(ucode, bytes, sample);
    c = &s->fpc[((uintptr_t)ucode >> 4 ^ (uintptr_t)(offset >> 6) ^ bytes) % FP_CACHE];
    same = c->ucode == ucode && c->offset == offset && c->bytes == bytes;
    if (!s->armed && same && !memcmp(c->sample, sample, FP_SAMPLE)) {
        s->fp_reused++;
    } else if (!s->armed && same) {
        flags = RSXF_PF_UNREADABLE; /* rewritten in place outside a capture: never the old identity */
        c->ucode = NULL;
    } else {
        ps3dk_prof__rsxf_fp_hash(ucode, bytes, &c->h);
        c->ucode = ucode;
        c->offset = offset;
        c->bytes = bytes;
        memcpy(c->sample, sample, FP_SAMPLE);
        s->fp_hashed++;
    }
    b = &s->binds[(fp_offset(offset) >> 6) % BIND_SLOTS];
    b->offset = offset;
    b->h = c->h;
    b->flags = flags;
    b->armed_seq = s->armed ? s->arm_seq : 0u;
    b->valid = 1;
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

/* The requester gives its CLAIMING word back: IDLE, keeping STOP (the only flag another actor adds to it). */
static void release_claim(void)
{
    uint64_t w = slot_load();
    while (!slot_cas(&w, WORD(RSXCAP_IDLE, w & F_STOP, RSXCAP_EPOCH(w))))
        ;
}

static int request(uint32_t source, uint32_t request_id, uint32_t flags, const char *name, uint32_t name_len,
                   uint32_t *id_out)
{
    rsxf_state *s = LD_ACQ(&S);
    uint64_t w, want;
    uint32_t e;
    if (!rsxf_selected() || !s)
        return PS3DK_PROF_ENOTSUP;
    if (flags || name_len > RSXF_NAME_MAX || (name_len && (!name || memchr(name, 0, name_len))))
        return PS3DK_PROF_EINVAL;
    if (!p_ready())
        return PS3DK_PROF_ENOTINIT;
    if (LD_ACQ(&g_prof->stop_latch))
        return PS3DK_PROF_ESTOPPED;
    w = slot_load();
    if (w & F_STOP)
        return PS3DK_PROF_ESTOPPED;
    if (RSXCAP_STATE(w) != RSXCAP_IDLE)
        return PS3DK_PROF_EBUSY;
    e = RSXCAP_EPOCH(w) + 1u;
    if (!slot_cas(&w, WORD(RSXCAP_CLAIMING, 0, e)))
        return (w & F_STOP) ? PS3DK_PROF_ESTOPPED : PS3DK_PROF_EBUSY;
    for (uint32_t i = 0; i < s->nblocks; i++) {
        s->blocks[i].base = pal_alloc(s->block_bytes);
        if (!s->blocks[i].base) {
            free_staging(s);
            release_claim();
            return PS3DK_PROF_ENOMEM;
        }
        ST_RLX(&s->blocks[i].state, B_FREE);
    }
    s->req_source = source;
    s->req_id = request_id;
    /* the name as valid UTF-8 (an invalid byte becomes '?'), so the reader never has to refuse it */
    s->name_len = ps3dk_prof__rsxf_utf8_append(s->name, 0, RSXF_NAME_MAX, name, name_len);
    if (source == 1u) {
        ST_RLX(&s->st_id, e);
        ST_RLX(&s->st_request, request_id);
        ST_RLX(&s->st_draws, 0u);
        ST_RLX(&s->st_frame, 0ull);
        ST_RLX(&s->st_bytes, 0ull);
    }
    status(s, 5u, 0u);
    want = WORD(RSXCAP_CLAIMING, 0, e);
    if (!slot_cas(&want, WORD(RSXCAP_REQUESTED, 0, e))) {
        /* shutdown added STOP while the blocks were allocated: nobody else touched them */
        status(s, 4u, RSXF_R_SHUTDOWN);
        free_staging(s);
        release_claim();
        return PS3DK_PROF_ESTOPPED;
    }
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
    if (!slot_cas(&want, WORD(RSXCAP_ARMING, 0, e)))
        return;
    s->frame = frame;
    s->draws_seen = s->hooked = s->decoded_only = s->mismatches = s->untimed = 0;
    s->labels_refused = s->programs_refused = s->nprograms = s->nlabels = 0;
    s->truncated_at = RSXF_NO_ORDINAL;
    s->bytes_streamed = s->bytes_dropped = s->sealed_bytes = 0;
    s->sealed_draws = s->sealed_programs = s->sealed_labels = s->sealed_states = s->sealed_baseline = 0;
    s->vp_cached = s->fp_cached = 0;
    s->next_seq = 0;
    s->cur = s->nblocks - 1u;
    s->words_at_arm = s->dec.words;
    s->lost_at_arm = s->dec.lost_events;
    s->hook.active = 0;
    (void)take_block(s);
    if (!put_baseline(s))
        s->truncated_at = 0; /* the baseline does not fit the staging: no draw gets a record (larger blocks help) */
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
    ST_REL(&s->begin_ready, 1u);
    if (s->req_source == 1u)
        ST_RLX(&s->st_frame, frame);
    status(s, 1u, 0u);
    want = WORD(RSXCAP_ARMING, 0, e);
    if (!slot_cas(&want, WORD(RSXCAP_ARMED, 0, e))) {
        owner_leave(s); /* shutdown added STOP while arming */
        return;
    }
    if (!++s->arm_seq)
        s->arm_seq = 1; /* 0 marks a bind made outside every capture */
    s->armed = 1;
}

/* END from the sealed blocks only: every count matches the records the drainer writes. The owner's counters, read by
 * the owner at close or by the drainer once the owner left (snap_decoder ran first in both cases). */
static void build_end(rsxf_state *s, uint8_t *out, uint32_t id, uint16_t outcome, uint16_t reason)
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
    e.state_records = s->sealed_states;
    e.baseline_registers = s->sealed_baseline;
    e.labels_refused = s->labels_refused;
    e.programs_refused = s->programs_refused;
    e.bytes_streamed = s->sealed_bytes;
    e.bytes_dropped = s->bytes_dropped + (s->bytes_streamed - s->sealed_bytes);
    e.tb_end = p_tb();
    e.decoder_words = s->end_words;
    e.reject_reason = s->end_reject_reason;
    e.reject_header = s->end_reject_header;
    e.lost_events = s->end_lost;
    e.untimed = s->untimed;
    (void)ps3dk_prof__rsxf_put_end(out, RSXF_END_BYTES, &e);
}

/* END at shutdown while the owner may still be inside the capture: only the drainer's own counts of the blocks it
 * wrote (the owner's counters are not read). */
static void build_end_drained(rsxf_state *s, uint8_t *out, uint32_t id)
{
    rsxf_wire_end e;
    memset(&e, 0, sizeof e);
    e.id = id;
    e.outcome = RSXF_OUT_ABANDONED;
    e.reason = RSXF_R_SHUTDOWN;
    e.blocks = s->blocks_emitted;
    e.draws_seen = e.draws_recorded = s->d_draws;
    e.truncated_at = RSXF_NO_ORDINAL;
    e.programs = s->d_programs;
    e.labels = s->d_labels;
    e.state_records = s->d_states;
    e.baseline_registers = s->d_baseline;
    e.bytes_streamed = s->d_bytes;
    e.tb_end = p_tb();
    (void)ps3dk_prof__rsxf_put_end(out, RSXF_END_BYTES, &e);
}

static void close_capture(rsxf_state *s, uint64_t w)
{
    uint32_t e = RSXCAP_EPOCH(w);
    uint64_t want = w;
    int truncated = s->truncated_at != RSXF_NO_ORDINAL;
    seal_current(s);
    if (LD_RLX(&s->blocks[s->cur].state) == B_FILLING)
        ST_REL(&s->blocks[s->cur].state, B_FREE); /* an empty open block */
    snap_decoder(s);
    build_end(s, s->end_buf, e, truncated ? RSXF_OUT_TRUNCATED : RSXF_OUT_DONE,
              truncated ? RSXF_R_STAGING : RSXF_R_NONE);
    s->armed = 0;
    s->hook.active = 0;
    ST_REL(&s->end_ready, 1u);
    status(s, 2u, truncated ? RSXF_R_STAGING : 0u);
    if (!slot_cas(&want, WORD(RSXCAP_CLOSING, 0, e)))
        owner_leave(s); /* shutdown added STOP: the drainer ends it from the counters published here */
}

void rsxf_frame(uint64_t frame, uint64_t tb, uint32_t owner, int window_open, uint32_t window_id)
{
    rsxf_state *s = LD_ACQ(&S);
    uint64_t w;
    if (!s)
        return;
    s->label = NULL;
    s->depth = s->depth_excess = 0;
    w = slot_load();
    if (w & F_STOP) {
        owner_leave(s);
        return;
    }
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
        /* abandoned: ARMING holds the blocks for the owner while it frees them, then the slot goes back to IDLE */
        if (slot_cas(&want, WORD(RSXCAP_ARMING, 0, RSXCAP_EPOCH(w)))) {
            status(s, 4u, RSXF_R_CANCELLED);
            free_staging(s);
            want = slot_load();
            while (!slot_cas(&want, WORD(RSXCAP_IDLE, want & F_STOP, RSXCAP_EPOCH(want))))
                ;
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
        s->d_draws += b->draws;
        s->d_programs += b->programs;
        s->d_labels += b->labels;
        s->d_states += b->states;
        s->d_baseline += b->baseline;
        s->d_bytes += b->used - RSXF_DRAWS_HEADER;
        ST_REL(&b->state, B_FREE);
    }
}

/* The sealing drainer's last step (STOP already set): end what was begun, then leave. It frees the staging only when
 * no other actor can touch it (IDLE, REQUESTED, CLOSING, or the owner already out); otherwise it marks F_DRAINER_OUT
 * and the requester or the owner frees on its way out. */
static void seal_finish(rsxf_state *s, rsxf_emit_fn emit, void *user)
{
    for (;;) {
        uint64_t w = slot_load();
        uint32_t st = RSXCAP_STATE(w), id = RSXCAP_EPOCH(w);
        uint8_t end[RSXF_END_BYTES];
        switch (st) {
        case RSXCAP_IDLE:
            return;
        case RSXCAP_CLAIMING: /* the requester allocates: its publish fails on STOP and it frees what it made */
            if (slot_cas(&w, w | F_DRAINER_OUT))
                return;
            continue;
        case RSXCAP_REQUESTED: /* no BEGIN was written; neither the owner nor the requester touches the blocks */
        case RSXCAP_CLOSING:   /* the owner is out; END could not be written (the sink refused) */
            status(s, 4u, RSXF_R_SHUTDOWN);
            if (slot_cas(&w, WORD(RSXCAP_IDLE, F_STOP, id))) {
                free_staging(s);
                ST_RLX(&s->begin_ready, 0u);
                s->begin_emitted = 0;
                return;
            }
            continue;
        case RSXCAP_ARMING:
        case RSXCAP_ARMED:
            if (s->begin_emitted) {
                if (w & F_OWNER_OUT) {
                    /* the owner's counters are final: write every block it sealed, then its END */
                    if (!emit_blocks(s, emit, user, id))
                        return;
                    build_end(s, end, id, RSXF_OUT_ABANDONED, RSXF_R_SHUTDOWN);
                } else {
                    build_end_drained(s, end, id);
                }
                if (emit(user, RSXF_CHUNK_END, end, RSXF_END_BYTES) != 0)
                    return;
                s->begin_emitted = 0;
            }
            status(s, 4u, RSXF_R_SHUTDOWN);
            if (w & F_OWNER_OUT) {
                if (slot_cas(&w, WORD(RSXCAP_IDLE, F_STOP, id))) {
                    free_staging(s);
                    ST_RLX(&s->begin_ready, 0u);
                    return;
                }
            } else if (slot_cas(&w, w | F_DRAINER_OUT)) {
                return; /* the owner frees when it observes STOP */
            }
            continue;
        default:
            return;
        }
    }
}

void rsxf_drain(rsxf_emit_fn emit, void *user, int auto_available, int seal)
{
    rsxf_state *s = LD_ACQ(&g_state);
    uint64_t w;
    uint32_t st, id;
    if (!rsxf_selected())
        return;
    if (!LD_RLX(&g_config_emitted)) {
        uint8_t b[RSXF_CONFIG_BYTES];
        rsxf_wire_config c;
        c.availability = !auto_available ? RSXF_NO_AUTO : LD_RLX(&g_ringless) ? RSXF_NO_RING
                         : !s ? RSXF_NO_DECODER : RSXF_AVAILABLE;
        c.block_bytes = opt_kb() * 1024u;
        c.blocks = opt_blocks();
        c.programs = PROGRAMS;
        c.labels = LABELS;
        c.label_bytes = RSXF_TEXT_MAX;
        if (!ps3dk_prof__rsxf_put_config(b, sizeof b, &c) || emit(user, RSXF_CHUNK_CONFIG, b, sizeof b) != 0)
            return;
        ST_RLX(&g_config_emitted, 1u);
    }
    if (!s || g_drain_sealed)
        return;
    w = slot_load();
    if (seal)
        while (!(w & F_STOP) && !slot_cas(&w, w | F_STOP))
            ;
    if ((w & F_DRAINER_OUT) || ((w & F_STOP) && !seal))
        return; /* sealed already: nothing is written after the seal */
    st = RSXCAP_STATE(w);
    id = RSXCAP_EPOCH(w);
    if (LD_ACQ(&s->begin_ready) && !s->begin_emitted) {
        if (emit(user, RSXF_CHUNK_BEGIN, s->begin_buf, s->begin_len) != 0)
            return;
        s->begin_emitted = 1;
        s->drain_seq = s->blocks_emitted = 0;
        s->d_draws = s->d_programs = s->d_labels = s->d_states = s->d_baseline = 0;
        s->d_bytes = 0;
    }
    if (s->begin_emitted && !emit_blocks(s, emit, user, id))
        return;
    if (st == RSXCAP_CLOSING && LD_ACQ(&s->end_ready) && s->drain_seq == s->next_seq) {
        if (emit(user, RSXF_CHUNK_END, s->end_buf, RSXF_END_BYTES) != 0)
            return;
        free_staging(s);
        ST_RLX(&s->begin_ready, 0u);
        s->begin_emitted = 0;
        status(s, 3u, LD_RLX(&s->st_reason));
        /* CLOSING belongs to the drainer; only this thread's seal adds STOP to it */
        while (!slot_cas(&w, WORD(RSXCAP_IDLE, w & F_STOP, id)))
            ;
    }
    if (seal) {
        seal_finish(s, emit, user);
        g_drain_sealed = 1;
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
    rsxf_state *s = LD_ACQ(&g_state);
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
    return rsxf_selected() && LD_ACQ(&S) != NULL;
}
static const rsxf_live_ops g_live_ops = {live_request, live_status, live_available};
const rsxf_live_ops *ps3dk_prof__rsxf_live_ops(void) { return rsxf_selected() ? &g_live_ops : NULL; }

#ifdef PS3DK_PROF_RSXF_TEST
/* Host tests: how many bind calls hashed and how many reused a cached hash. */
void ps3dk_prof__rsxf_test_bind_stats(uint64_t *hashed, uint64_t *reused);
void ps3dk_prof__rsxf_test_bind_stats(uint64_t *hashed, uint64_t *reused)
{
    rsxf_state *s = LD_ACQ(&g_state);
    *hashed = s ? s->fp_hashed : 0;
    *reused = s ? s->fp_reused : 0;
}
/* Host tests: the standing state's size (allocated once at attach with the ring). */
size_t ps3dk_prof__rsxf_test_state_bytes(void);
size_t ps3dk_prof__rsxf_test_state_bytes(void) { return sizeof(rsxf_state); }
/* Host tests: the slot word, to aim a seal at a state. */
uint64_t ps3dk_prof__rsxf_test_slot(void);
uint64_t ps3dk_prof__rsxf_test_slot(void) { return slot_load(); }
#endif
