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
       UNIT_MAX = RSXF_UNIT_DEFS };
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
/* A program container seen at bind (owner, bounded): its uniforms and, for an FP, its constant blocks with the
 * container's values. Looked up by (kind, hash) when a capture defines the program or records a draw. */
typedef struct uniform_info { uint32_t resource; uint16_t count; uint8_t kind, name_len; char name[RSXF_UNI_NAME]; } uniform_info;
typedef struct prog_info {
    const void *container;
    uint32_t kind, ucode_bytes, stamp, valid;
    uint64_t hash;
    uint32_t nuni, uni_trunc;
    uniform_info uni[RSXF_UNI_MAX];
    uint32_t nconst, const_trunc;
    uint32_t c_off[RSXF_FPC_MAX];
    uint32_t c_def[RSXF_FPC_MAX][4];
} prog_info;
enum { PINFO = 16 };
typedef struct fpc_entry { uint32_t off, prov[4], v[4]; } fpc_entry; /* provenance per component */

typedef struct block {
    uint8_t *base;
    uint32_t state;              /* atomic: B_* */
    uint32_t used, records, seq; /* owner while FILLING, drainer while SEALED */
    uint32_t draws, programs, labels, states, baseline; /* baseline: BASELINE register pairs */
    uint32_t vpcs, fpcs, unis, ucodes;
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
    uint32_t sealed_vpcs, sealed_fpcs, sealed_unis, sealed_ucodes;
    /* request flags (requester, before REQUESTED); this capture's mode while armed (owner) */
    uint32_t req_flags, iso, second_half;
    /* program bytes stored in this capture: by (profile, hash), and the FP bytes against the cap */
    uint32_t nsent, fp_bytes_used;
    uint64_t sent_hash[RSXF_SENT_MAX];
    uint8_t sent_profile[RSXF_SENT_MAX];
    uint64_t sealed_bytes;
    /* the register state the records describe so far (owner, armed): a STATE record carries what differs */
    uint32_t emit_val[RSXF_REGS];
    uint32_t emit_known[RSXF_REG_WORDS];
    uint32_t emit_cleared; /* dec.regs_cleared when emit_known was last valid */
    /* the VP constants and the FP constant list the records describe so far */
    uint32_t emit_vpc[RSXF_VP_CONSTS][4];
    uint8_t emit_vpc_known[RSXF_VP_CONSTS];
    uint32_t emit_vpc_cleared;
    uint32_t fpc_prog, fpc_n;
    fpc_entry fpc_last[RSXF_FPC_MAX];
    /* programs the current draw's unit defines (for their UNIFORM records) */
    uint32_t unit_new, unit_new_idx[2], unit_new_prof[2];
    uint64_t unit_new_hash[2];
    uint32_t unit_new_addr[2], unit_new_n[2];
    /* program containers seen at bind; the last FP bind's hash */
    prog_info pinfo[PINFO];
    uint32_t pinfo_stamp, last_fp_ok;
    uint64_t last_fp_hash;
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
    uint32_t d_vpcs, d_fpcs, d_unis, d_ucodes;
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

/* Staging block KiB: 256 by default, never below RSXF_STAGING_KB_MIN (the largest per-draw unit fits one block). */
static uint32_t opt_kb(void)
{
    uint32_t kb = ps3dk_prof_automatic_rsx_frame_options.staging_kb;
    return !kb ? 256u : kb < RSXF_STAGING_KB_MIN ? RSXF_STAGING_KB_MIN : kb;
}
/* FP ucode bytes stored per capture when requested (PS3_PROF_RSX_FRAME_FP_BYTES_KB; 256 KiB by default). */
static uint32_t opt_fp_cap(void)
{
    uint32_t kb = ps3dk_prof_automatic_rsx_frame_options.fp_bytes_kb;
    return (kb ? kb : 256u) * 1024u;
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
        if (o->size == sizeof *o && o->fp_bytes_kb <= 4096u && kb >= 4u && kb <= 1024u && b >= 2u && b <= BLOCKS_MAX)
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
    s->sealed_vpcs += b->vpcs;
    s->sealed_fpcs += b->fpcs;
    s->sealed_unis += b->unis;
    s->sealed_ucodes += b->ucodes;
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
            b->vpcs = b->fpcs = b->unis = b->ucodes = 0;
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

typedef struct counts { uint32_t records, draws, programs, labels, states, baseline, vpcs, fpcs, unis, ucodes; } counts;

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
    b->vpcs += c->vpcs;
    b->fpcs += c->fpcs;
    b->unis += c->unis;
    b->ucodes += c->ucodes;
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

/* ---- VP constant records (owner) --------------------------------------------------------------------------------- */

enum { VPC_WORDS = (RSXF_VP_CONSTS + 31) / 32 };

static uint32_t vpc_bytes(uint32_t n, uint32_t reset)
{
    uint32_t recs = (n + RSXF_VPC_PER_REC - 1u) / RSXF_VPC_PER_REC;
    if (!recs && reset)
        recs = 1;
    return recs * RSXF_REG_HEADER + RSXF_VPC_ENTRY * n;
}

/* VPCONST records of the slots in `want`, RSXF_VPC_PER_REC each; `flags` on the first (RESET) or every (BASELINE). */
static uint32_t put_vpc_records(rsxf_state *s, uint8_t *at, uint32_t ordinal, uint32_t flags, const uint32_t *want,
                                uint32_t n, uint32_t *records)
{
    uint8_t *p = at;
    uint32_t in = RSXF_VPC_PER_REC, left = n, recs = 0;
    for (uint32_t w = 0; w < VPC_WORDS && left; w++) {
        for (uint32_t bits = want[w]; bits; bits &= bits - 1u) {
            uint32_t slot = w * 32u + (uint32_t)__builtin_ctz(bits);
            if (in == RSXF_VPC_PER_REC) {
                uint32_t take = left < RSXF_VPC_PER_REC ? left : RSXF_VPC_PER_REC;
                memset(p, 0, RSXF_REG_HEADER);
                put16(p, RSXF_REC_VPCONST);
                put16(p + 2, RSXF_REG_HEADER + RSXF_VPC_ENTRY * take);
                put32(p + 4, ordinal);
                put16(p + 8, take);
                put16(p + 10, (flags & RSXF_VPC_BASELINE) ? flags : (recs == 0 ? flags : 0u));
                p += RSXF_REG_HEADER;
                in = 0;
                recs++;
            }
            memset(p, 0, RSXF_VPC_ENTRY);
            put16(p, slot);
            p[2] = s->dec.vpc_known[slot];
            for (uint32_t k = 0; k < 4; k++)
                put32(p + 8 + 4 * k, s->dec.vpc[slot][k]);
            p += RSXF_VPC_ENTRY;
            in++;
            left--;
        }
    }
    if (!recs && (flags & RSXF_VPC_RESET)) {
        memset(p, 0, RSXF_REG_HEADER);
        put16(p, RSXF_REC_VPCONST);
        put16(p + 2, RSXF_REG_HEADER);
        put32(p + 4, ordinal);
        put16(p + 10, flags);
        p += RSXF_REG_HEADER;
        recs = 1;
    }
    *records = recs;
    return (uint32_t)(p - at);
}

/* Slots to send at this draw: known, written since the last record, and different (components or values). */
static uint32_t vpc_delta(rsxf_state *s, uint32_t *want, uint32_t *reset)
{
    const rsxf_decoder *d = &s->dec;
    uint32_t n = 0;
    *reset = d->regs_cleared != s->emit_vpc_cleared ? RSXF_VPC_RESET : 0u;
    for (uint32_t w = 0; w < VPC_WORDS; w++) {
        uint32_t keep = 0;
        for (uint32_t bits = d->vpc_dirty[w]; bits; bits &= bits - 1u) {
            uint32_t b = (uint32_t)__builtin_ctz(bits), slot = w * 32u + b;
            if (slot >= RSXF_VP_CONSTS || !d->vpc_known[slot])
                continue;
            if (*reset || s->emit_vpc_known[slot] != d->vpc_known[slot] ||
                memcmp(s->emit_vpc[slot], d->vpc[slot], 16))
                keep |= 1u << b;
        }
        want[w] = keep;
        n += (uint32_t)__builtin_popcount(keep);
    }
    return n;
}

static void vpc_emitted(rsxf_state *s, const uint32_t *want, uint32_t reset)
{
    rsxf_decoder *d = &s->dec;
    if (reset)
        memset(s->emit_vpc_known, 0, sizeof s->emit_vpc_known);
    for (uint32_t w = 0; w < VPC_WORDS; w++)
        for (uint32_t bits = want[w]; bits; bits &= bits - 1u) {
            uint32_t slot = w * 32u + (uint32_t)__builtin_ctz(bits);
            memcpy(s->emit_vpc[slot], d->vpc[slot], 16);
            s->emit_vpc_known[slot] = d->vpc_known[slot];
        }
    memset(d->vpc_dirty, 0, sizeof d->vpc_dirty);
    s->emit_vpc_cleared = d->regs_cleared;
}

/* At arming, after the register BASELINE: every known VP constant, VPCONST records flagged BASELINE. */
static int put_vpc_baseline(rsxf_state *s)
{
    rsxf_decoder *d = &s->dec;
    uint32_t all[VPC_WORDS], slot = 0;
    memset(all, 0, sizeof all);
    memset(s->emit_vpc_known, 0, sizeof s->emit_vpc_known);
    s->emit_vpc_cleared = d->regs_cleared;
    while (slot < RSXF_VP_CONSTS) {
        uint32_t want[VPC_WORDS], take = 0, recs = 0, len;
        uint8_t *p;
        counts c;
        memset(want, 0, sizeof want);
        for (; slot < RSXF_VP_CONSTS && take < RSXF_VPC_PER_REC; slot++)
            if (d->vpc_known[slot]) {
                want[slot >> 5] |= 1u << (slot & 31u);
                take++;
            }
        if (!take)
            break;
        len = vpc_bytes(take, 0);
        if (!(p = reserve(s, len)))
            return 0;
        (void)put_vpc_records(s, p, 0, RSXF_VPC_BASELINE, want, take, &recs);
        memset(&c, 0, sizeof c);
        c.records = recs;
        c.vpcs = recs;
        commit(s, len, &c);
        for (uint32_t w = 0; w < VPC_WORDS; w++)
            all[w] |= want[w];
    }
    vpc_emitted(s, all, 0);
    return 1;
}

/* ---- program containers (owner, in the bind call) ----------------------------------------------------------------- */

static rsxf_state *owner_state(void *ctx);
static uint32_t fp_offset(uint32_t v);

static uint32_t rd32n(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v; /* the container's own (native) order */
}
static uint32_t be32b(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

static prog_info *find_pinfo(rsxf_state *s, uint32_t kind, uint64_t hash)
{
    for (uint32_t i = 0; i < PINFO; i++)
        if (s->pinfo[i].valid && s->pinfo[i].kind == kind && s->pinfo[i].hash == hash)
            return &s->pinfo[i];
    return NULL;
}

void ps3dk_prof__rsxf_container(void *ctx, const void *container, uint32_t kind);
void ps3dk_prof__rsxf_container(void *ctx, const void *container, uint32_t kind)
{
    rsxf_state *s = owner_state(ctx);
    const uint8_t *c = container;
    prog_info *pi = NULL;
    uint32_t total, npar, par, usize, uoff;
    uint64_t hash = 0;
    if (!s || !c || (kind != RSXF_UNI_VP && kind != RSXF_UNI_FP))
        return;
    total = rd32n(c + 8);
    npar = rd32n(c + 12);
    par = rd32n(c + 16);
    usize = rd32n(c + 24);
    uoff = rd32n(c + 28);
    if (total < 32u || total > (1u << 24) || par > total || npar > (total - par) / 48u || uoff > total ||
        usize > total - uoff)
        return; /* not a container this table can read */
    if (kind == RSXF_UNI_FP) {
        if (!s->last_fp_ok)
            return; /* its fp_bind found no identity */
        hash = s->last_fp_hash;
    }
    for (uint32_t i = 0; i < PINFO; i++) {
        prog_info *q = &s->pinfo[i];
        if (q->valid && q->container == container && q->kind == kind && q->ucode_bytes == usize &&
            (kind == RSXF_UNI_VP || q->hash == hash)) {
            q->stamp = ++s->pinfo_stamp; /* seen before: nothing to read */
            return;
        }
        if (!pi || !q->valid || (pi->valid && q->stamp < pi->stamp))
            pi = q;
    }
    if (kind == RSXF_UNI_VP) {
        rsxf_hash h;
        ps3dk_prof__rsxf_vp_hash((const uint32_t *)(const void *)(c + uoff), usize / 16u, &h);
        hash = h.hash;
    }
    memset(pi, 0, sizeof *pi);
    pi->valid = 1;
    pi->container = container;
    pi->kind = kind;
    pi->ucode_bytes = usize;
    pi->hash = hash;
    pi->stamp = ++s->pinfo_stamp;
    for (uint32_t i = 0; i < npar; i++) {
        const uint8_t *q = c + par + 48u * i;
        uint32_t var = rd32n(q + 8), name = rd32n(q + 16), emb = rd32n(q + 24), resource, count;
        int32_t res_index = (int32_t)rd32n(q + 12);
        uniform_info *u;
        if (var != 0x1006u || !name || name >= total)
            continue; /* not a named uniform */
        if (kind == RSXF_UNI_VP) {
            if (res_index < 0 || res_index >= RSXF_VP_CONSTS)
                continue;
            resource = (uint32_t)res_index;
            count = 1;
        } else {
            if (!emb || emb > total - 8u)
                continue;
            count = rd32n(c + emb);
            if (!count || count > (total - emb - 4u) / 4u)
                continue;
            resource = rd32n(c + emb + 4u);
        }
        if (pi->nuni == RSXF_UNI_MAX) {
            pi->uni_trunc = 1;
            continue;
        }
        u = &pi->uni[pi->nuni];
        u->name_len = (uint8_t)ps3dk_prof__rsxf_utf8_append(u->name, 0, RSXF_UNI_NAME, (const char *)c + name,
                                                            total - name);
        if (!u->name_len)
            continue;
        u->resource = resource;
        u->count = (uint16_t)(count > 0xffffu ? 0xffffu : count);
        u->kind = (uint8_t)kind;
        pi->nuni++;
    }
    if (kind == RSXF_UNI_FP) {
        /* the constant blocks (an instruction reading a constant is followed by its 16-byte value) */
        const uint8_t *u = c + uoff;
        for (uint32_t pos = 0, n = 0; pos + 16u <= usize && n < 65536u; n++) {
            const uint8_t *in = u + pos;
            int constant = 0, end = ((be32b(in) << 16 | be32b(in) >> 16) & 1u) != 0;
            for (int k = 1; k < 4; k++) {
                uint32_t wv = be32b(in + 4 * k);
                if (((wv << 16 | wv >> 16) & 3u) == 2u)
                    constant = 1;
            }
            pos += 16u;
            if (constant && pos + 16u <= usize) {
                if (pi->nconst < RSXF_FPC_MAX) {
                    pi->c_off[pi->nconst] = pos;
                    for (int k = 0; k < 4; k++)
                        pi->c_def[pi->nconst][k] = rd32n(u + pos + 4u * (uint32_t)k);
                    pi->nconst++;
                } else {
                    pi->const_trunc = 1;
                }
                pos += 16u;
            }
            if (end)
                break;
        }
    }
}

/* The FP embedded constants of the draw's FP, component by component: the last inline write the decoder saw to that
 * word (INLINE), else the container's value (CONTAINER when no FIFO write to it can have been missed, else UNKNOWN). */
static uint32_t fp_constants(rsxf_state *s, uint32_t fp_index, fpc_entry *out, uint32_t *trunc)
{
    const rsxf_decoder *d = &s->dec;
    const prog_info *pi;
    uint32_t base, local;
    *trunc = 0;
    if (!fp_index || !s->programs[fp_index - 1u].hash || !(d->known & 2u))
        return 0;
    if (!(pi = find_pinfo(s, RSXF_UNI_FP, s->programs[fp_index - 1u].hash)))
        return 0;
    base = fp_offset(d->fp_address);
    local = (d->fp_address & 3u) == 1u;
    for (uint32_t i = 0; i < pi->nconst; i++) {
        out[i].off = pi->c_off[i];
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t v = 0, p;
            if (local)
                p = ps3dk_prof__rsxf_inline_word(d, base + pi->c_off[i] + 4u * k, &v);
            else
                p = d->main_written || d->imap_cleared ? RSXF_FPC_UNKNOWN : RSXF_FPC_CONTAINER; /* main: not mapped */
            out[i].prov[k] = p;
            out[i].v[k] = p == RSXF_FPC_INLINE ? v : pi->c_def[i][k];
        }
    }
    *trunc = pi->const_trunc;
    return pi->nconst;
}

static uint32_t put_fpc_record(uint8_t *p, uint32_t ordinal, uint32_t program, const fpc_entry *e, uint32_t n,
                               uint32_t trunc)
{
    memset(p, 0, RSXF_REG_HEADER + RSXF_FPC_ENTRY * n);
    put16(p, RSXF_REC_FPCONST);
    put16(p + 2, RSXF_REG_HEADER + RSXF_FPC_ENTRY * n);
    put32(p + 4, ordinal);
    put16(p + 8, n);
    put16(p + 10, trunc ? RSXF_FPC_TRUNCATED : 0u);
    put32(p + 12, program);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *q = p + RSXF_REG_HEADER + RSXF_FPC_ENTRY * i;
        put32(q, e[i].off);
        for (uint32_t k = 0; k < 4; k++)
            q[4 + k] = (uint8_t)e[i].prov[k];
        for (uint32_t k = 0; k < 4; k++)
            put32(q + 8 + 4 * k, e[i].v[k]);
    }
    return RSXF_REG_HEADER + RSXF_FPC_ENTRY * n;
}

static uint32_t uniform_bytes(const prog_info *pi) { return pi && pi->nuni ? RSXF_REG_HEADER + RSXF_UNI_ENTRY * pi->nuni : 0u; }

static uint32_t put_uniform_record(uint8_t *p, uint32_t program, const prog_info *pi)
{
    uint32_t len = uniform_bytes(pi);
    memset(p, 0, len);
    put16(p, RSXF_REC_UNIFORM);
    put16(p + 2, len);
    put32(p + 4, program);
    put16(p + 8, pi->nuni);
    put16(p + 10, pi->uni_trunc ? RSXF_UNI_TRUNCATED : 0u);
    for (uint32_t i = 0; i < pi->nuni; i++) {
        uint8_t *q = p + RSXF_REG_HEADER + RSXF_UNI_ENTRY * i;
        put32(q, pi->uni[i].resource);
        put16(q + 4, pi->uni[i].count);
        q[6] = pi->uni[i].kind;
        q[7] = pi->uni[i].name_len;
        memcpy(q + 8, pi->uni[i].name, pi->uni[i].name_len);
    }
    return len;
}

/* ---- program bytes (owner, armed) -------------------------------------------------------------------------------- */

static int ucode_sent(rsxf_state *s, uint32_t profile, uint64_t hash)
{
    for (uint32_t i = 0; i < s->nsent; i++)
        if (s->sent_hash[i] == hash && s->sent_profile[i] == profile)
            return 1;
    return 0;
}

/* One program's bytes as UCODE records of at most RSXF_UCODE_PART bytes each, each its own record (a block may end
 * between them). `byte_at(ctx, i)` gives byte i. 0 when a part did not fit: the parts written stay (the reader shows
 * the program's bytes incomplete). */
static int put_ucode(rsxf_state *s, uint32_t profile, uint64_t hash, uint32_t total,
                     uint8_t (*byte_at)(const void *, uint32_t), const void *ctx)
{
    if (s->nsent >= RSXF_SENT_MAX || !total || ucode_sent(s, profile, hash))
        return 1;
    s->sent_hash[s->nsent] = hash;
    s->sent_profile[s->nsent++] = (uint8_t)profile;
    for (uint32_t off = 0, part = 0; off < total; part++) {
        uint32_t n = total - off < RSXF_UCODE_PART ? total - off : RSXF_UCODE_PART;
        uint32_t len = RSXF_UCODE_HEADER + ((n + 7u) & ~7u);
        uint8_t *p = reserve(s, len);
        counts c;
        if (!p)
            return 0;
        memset(p, 0, len);
        put16(p, RSXF_REC_UCODE);
        put16(p + 2, len);
        p[4] = (uint8_t)profile;
        p[5] = off + n == total ? RSXF_UCODE_LAST : 0u;
        put16(p + 6, part);
        put32(p + 8, total);
        put32(p + 12, (uint32_t)hash);
        put32(p + 16, (uint32_t)(hash >> 32));
        put32(p + 20, off);
        put32(p + 24, n);
        for (uint32_t i = 0; i < n; i++)
            p[RSXF_UCODE_HEADER + i] = byte_at(ctx, off + i);
        memset(&c, 0, sizeof c);
        c.records = 1;
        c.ucodes = 1;
        commit(s, len, &c);
        off += n;
    }
    return 1;
}

/* VP bytes from the decoder's VP memory: instruction words big-endian, from the start slot. */
typedef struct vp_bytes { const rsxf_decoder *d; uint32_t start; } vp_bytes;
static uint8_t vp_byte(const void *ctx, uint32_t i)
{
    const vp_bytes *v = ctx;
    uint32_t w = v->d->vp[v->start + i / 16u][(i / 4u) & 3u];
    return (uint8_t)(w >> (24u - 8u * (i & 3u)));
}
static uint8_t mem_byte(const void *ctx, uint32_t i) { return ((const uint8_t *)ctx)[i]; }

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
    if (s->unit_new < 2u && hash) {
        s->unit_new_idx[s->unit_new] = w.index;
        s->unit_new_prof[s->unit_new] = profile == RSXF_PROFILE_VP ? RSXF_UNI_VP : RSXF_UNI_FP;
        s->unit_new_hash[s->unit_new] = hash;
        s->unit_new_addr[s->unit_new] = address;
        s->unit_new_n[s->unit_new] = h ? h->instructions : 0u;
        s->unit_new++;
    }
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
    s->unit_new = 0;
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
            w.index_location = (uint8_t)(s->hook.location & 1u);
        }
        if (s->hook.kind == RSXF_K_INDEX || s->hook.kind == RSXF_K_INLINE_INDEX)
            w.index_type = (uint8_t)(s->hook.index_type & 1u);
        w.clear_mask = s->hook.kind == RSXF_K_CLEAR ? s->hook.clear_mask : 0;
        w.flags = RSXF_DF_HOOKED | (e ? RSXF_DF_DECODED : 0);
        if (e && (e->kind != s->hook.kind ||
                  (e->kind != RSXF_K_CLEAR && e->kind != RSXF_K_IMMEDIATE && e->count != s->hook.count))) {
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
        w.count = e->kind == RSXF_K_IMMEDIATE || e->kind == RSXF_K_CLEAR ? 0 : e->count;
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
        /* in one block, in this order: STATE (registers changed since the last draw), VPCONST (VP constants
         * changed), the definitions, UNIFORM (for programs defined now), FPCONST (when the FP constants differ from
         * the last list), DRAW */
        uint32_t want[RSXF_REG_WORDS], reset, n = state_delta(s, want, &reset), sbytes = reg_bytes(n, reset), recs = 0;
        uint32_t vwant[VPC_WORDS], vreset, vn = vpc_delta(s, vwant, &vreset), vbytes = vpc_bytes(vn, vreset), vrecs = 0;
        fpc_entry fpc[RSXF_FPC_MAX];
        uint32_t ftrunc = 0, fn = w.kind == RSXF_K_CLEAR ? 0u : fp_constants(s, w.fp, fpc, &ftrunc), fbytes = 0;
        const prog_info *upi[2] = {NULL, NULL};
        uint32_t ubytes = 0, nuni = 0, defs_len = len - RSXF_DRAW_BYTES;
        uint8_t *p;
        if (w.kind != RSXF_K_CLEAR &&
            (s->fpc_prog != w.fp || s->fpc_n != fn || memcmp(s->fpc_last, fpc, sizeof fpc[0] * fn)))
            fbytes = RSXF_REG_HEADER + RSXF_FPC_ENTRY * fn;
        for (uint32_t k = 0; k < s->unit_new; k++) {
            upi[k] = find_pinfo(s, s->unit_new_prof[k], s->unit_new_hash[k]);
            if (uniform_bytes(upi[k])) {
                ubytes += uniform_bytes(upi[k]);
                nuni++;
            }
        }
        /* a vertex program defined now: its instruction bytes once per capture (its own records) */
        for (uint32_t k = 0; k < s->unit_new; k++)
            if (s->unit_new_prof[k] == RSXF_UNI_VP && s->unit_new_addr[k] + s->unit_new_n[k] <= RSXF_VP_SLOTS) {
                vp_bytes vb = {&s->dec, s->unit_new_addr[k]};
                (void)put_ucode(s, RSXF_PROFILE_VP, s->unit_new_hash[k], 16u * s->unit_new_n[k], vp_byte, &vb);
            }
        p = reserve(s, sbytes + vbytes + ubytes + fbytes + len);
        if (p) {
            counts c;
            uint8_t *q = p;
            q += put_reg_records(s, q, RSXF_REC_STATE, ordinal, reset, want, n, &recs);
            q += put_vpc_records(s, q, ordinal, vreset, vwant, vn, &vrecs);
            memcpy(q, unit, defs_len);
            q += defs_len;
            for (uint32_t k = 0; k < s->unit_new; k++)
                if (uniform_bytes(upi[k]))
                    q += put_uniform_record(q, s->unit_new_idx[k], upi[k]);
            if (fbytes)
                q += put_fpc_record(q, ordinal, w.fp, fpc, fn, ftrunc);
            memcpy(q, unit + defs_len, RSXF_DRAW_BYTES);
            memset(&c, 0, sizeof c);
            c.records = recs + vrecs + nuni + (fbytes ? 1u : 0u) + defs_p + defs_l + 1u;
            c.draws = 1;
            c.programs = defs_p;
            c.labels = defs_l;
            c.states = recs;
            c.vpcs = vrecs;
            c.unis = nuni;
            c.fpcs = fbytes ? 1u : 0u;
            commit(s, sbytes + vbytes + ubytes + fbytes + len, &c);
            state_emitted(s, want, reset);
            vpc_emitted(s, vwant, vreset);
            if (fbytes) {
                s->fpc_prog = w.fp;
                s->fpc_n = fn;
                memcpy(s->fpc_last, fpc, sizeof fpc[0] * fn);
            }
        } else {
            /* no block: this draw and every later one are counted only; definitions made for it are undone */
            if (s->truncated_at == RSXF_NO_ORDINAL)
                s->truncated_at = ordinal;
            s->bytes_dropped += sbytes + vbytes + ubytes + fbytes + len;
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

int rsxf_isolated(void)
{
    rsxf_state *s = LD_ACQ(&S);
    return s && s->armed && s->iso;
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
    /* another program (or one this slot does not know) at this address: the inline words remembered over its ucode
     * may belong to the program that was there before, so they are UNKNOWN until written again */
    if ((flags & RSXF_PF_UNREADABLE) ||
        !(b->valid && fp_offset(b->offset) == fp_offset(offset) && b->h.hash == c->h.hash && b->h.bytes == c->h.bytes))
        ps3dk_prof__rsxf_inline_forget(&s->dec, fp_offset(offset), fp_offset(offset) + bytes);
    b->offset = offset;
    b->h = c->h;
    b->flags = flags;
    b->armed_seq = s->armed ? s->arm_seq : 0u;
    b->valid = 1;
    s->last_fp_ok = !(flags & RSXF_PF_UNREADABLE);
    s->last_fp_hash = c->h.hash;
    /* FP bytes requested: stored once per program bound while armed, within the capture's cap (from the bind call's
     * memory, never later) */
    if (s->armed && (s->req_flags & RSXF_REQ_FP_BYTES) && s->last_fp_ok && c->h.bytes &&
        !ucode_sent(s, RSXF_PROFILE_FP, c->h.hash) && s->fp_bytes_used + c->h.bytes <= opt_fp_cap()) {
        s->fp_bytes_used += c->h.bytes;
        (void)put_ucode(s, RSXF_PROFILE_FP, c->h.hash, c->h.bytes, mem_byte, ucode);
    }
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
    if ((flags & ~(uint32_t)RSXF_REQ_ALL) || ((flags & RSXF_REQ_ISOLATED) && (flags & RSXF_REQ_BOTH)) ||
        name_len > RSXF_NAME_MAX || (name_len && (!name || memchr(name, 0, name_len))))
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
    s->req_flags = flags;
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
    s->sealed_vpcs = s->sealed_fpcs = s->sealed_unis = 0;
    s->fpc_prog = s->fpc_n = 0;
    s->sealed_ucodes = 0;
    s->iso = (s->req_flags & RSXF_REQ_ISOLATED) != 0;
    s->second_half = 0;
    s->nsent = s->fp_bytes_used = 0;
    s->vp_cached = s->fp_cached = 0;
    s->next_seq = 0;
    s->cur = s->nblocks - 1u;
    s->words_at_arm = s->dec.words;
    s->lost_at_arm = s->dec.lost_events;
    s->hook.active = 0;
    (void)take_block(s);
    if (!put_baseline(s) || !put_vpc_baseline(s))
        s->truncated_at = 0; /* the baseline does not fit the staging: no draw gets a record (larger blocks help) */
    memset(&b, 0, sizeof b);
    b.id = e;
    b.source = (uint16_t)s->req_source;
    b.flags = (uint16_t)(s->req_flags & RSXF_REQ_ALL);
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
    e.vpconst_records = s->sealed_vpcs;
    e.fpconst_records = s->sealed_fpcs;
    e.uniform_records = s->sealed_unis;
    e.ucode_records = s->sealed_ucodes;
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
    e.vpconst_records = s->d_vpcs;
    e.fpconst_records = s->d_fpcs;
    e.uniform_records = s->d_unis;
    e.ucode_records = s->d_ucodes;
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
        if ((s->req_flags & RSXF_REQ_BOTH) && !s->second_half) {
            /* BOTH: this boundary starts the isolated frame; a SECOND record marks its first draw */
            uint8_t *p = reserve(s, 16u);
            s->second_half = 1;
            s->iso = 1;
            if (p) {
                counts c;
                memset(p, 0, 16);
                put16(p, RSXF_REC_SECOND);
                put16(p + 2, 16);
                put32(p + 4, s->draws_seen);
                put32(p + 8, (uint32_t)frame);
                put32(p + 12, (uint32_t)(frame >> 32));
                memset(&c, 0, sizeof c);
                c.records = 1;
                commit(s, 16, &c);
            } else if (s->truncated_at == RSXF_NO_ORDINAL) {
                s->truncated_at = s->draws_seen; /* no room for the marker: the isolated frame is not recorded */
            }
            return;
        }
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
        (void)ps3dk_prof_capture_begin(&g_window_desc, PS3DK_PROF_WINDOW_FRAMES,
                                       (s->req_flags & RSXF_REQ_BOTH) ? 3u : 2u);
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
        s->d_vpcs += b->vpcs;
        s->d_fpcs += b->fpcs;
        s->d_unis += b->unis;
        s->d_ucodes += b->ucodes;
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
        c.fp_bytes_cap = opt_fp_cap();
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
        s->d_vpcs = s->d_fpcs = s->d_unis = s->d_ucodes = 0;
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
