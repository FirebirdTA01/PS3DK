/* ps3dk-prof drainer: the chunk encoder, the sink, ordinary drains, the drain thread, the quiescence wait, the frozen
 * frontier, sealing, ps3dk_prof_drain and ps3dk_prof_shutdown.  The drainer is the ONLY consumer (5.4) and the only
 * code that frees a slot (r2 3.1).  References as in prof_core.c. */
#include "prof_internal.h"
#ifdef PS3DK_PROF_WITH_AUTO
#include "../auto_rsx/auto_internal.h"
#endif
#ifdef PS3DK_PROF_WITH_WAITS
#include "../waits/wait_internal.h"
#endif
#ifdef PS3DK_PROF_WITH_FUNC
#include "../func/func_internal.h"
#endif
#ifdef PS3DK_PROF_WITH_COUNTERS
#include "../counters/counter_internal.h"
#endif
#ifdef PS3DK_PROF_WITH_FIFO
#include "../fifo/fifo_internal.h"
#endif

#include <string.h>

/* ---- sink (8, r2 8 / B5) ------------------------------------------------------------------------------------------- */

#define P_SINK_TRIES 16u   /* tries with no progress in a row */

static void sink_fail(sink *k, int code)
{
    if (!k->failed && !k->first_error)
        k->first_error = code ? code : -1;
    k->failed = 1;
}

/* Write all n bytes or fail.  A short count is retried for the remainder; EINTR (file sink) is retried and counted
 * as a recovered error; 16 tries in a row without progress, any other error, a negative callback return or a count
 * above n is a SINK FAILURE, after which nothing more is written. */
static int sink_write(prof *p, const uint8_t *buf, size_t n)
{
    sink *k = &p->snk;
    unsigned noprog = 0;
    if (k->failed)
        return -1;
    /* Refuse before invoking a sink that could accept unrepresentable bytes. */
    if ((uint64_t)n > UINT64_MAX - p->prefix_bytes) {
        sink_fail(k, -6);
        return -1;
    }
    while (n > 0) {
        long r;
        int err = 0;
        if (k->kind == 1)
            r = pal_write(k->fd, buf, (unsigned long)n, &err);
        else
            r = k->cb.write(k->cb.ctx, buf, (unsigned long)n);
        if (r < 0) {
            if (k->kind == 1 && err == PAL_EINTR && ++noprog < P_SINK_TRIES) {
                k->error_count++;
                if (!k->first_error)
                    k->first_error = err;
                continue;
            }
            sink_fail(k, k->kind == 1 ? err : (int)r);
            return -1;
        }
        if ((unsigned long)r > (unsigned long)n) {
            sink_fail(k, -1);   /* B5: a count above n is invalid */
            return -1;
        }
        if (r == 0) {
            if (++noprog >= P_SINK_TRIES) {
                sink_fail(k, -1);
                return -1;
            }
            continue;
        }
        noprog = 0;
        /* Count only positive, in-range acknowledgements, before advancing
         * buf. Zero/retry/error responses never contribute to the prefix. */
#ifdef PS3DK_PROF_WITH_ARCS
        p->prefix_crc32 = ps3dk_prof__crc32_extend(p->prefix_crc32, buf, (size_t)r);
#endif
        p->prefix_bytes += (uint64_t)r;
        buf += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Close exactly once.  A close failure after the trailer FAILS the run (ECLOSE). */
static void sink_close(prof *p)
{
    sink *k = &p->snk;
    int err = 0;
    if (k->closed)
        return;
    k->closed = 1;
    if (k->kind == 1) {
        if (pal_close(k->fd, &err) != 0)
            k->close_failed = 1;
    } else if (k->kind == 2) {
        if (k->cb.close(k->cb.ctx) < 0)
            k->close_failed = 1;
    }
}

/* ---- chunk encoder (2, 3) ------------------------------------------------------------------------------------------ */

static uint8_t *pl(prof *p) { return p->dr.buf + 16; }

static void enc_begin(prof *p, uint32_t type)
{
    p->dr.chunk_type = type;
    p->dr.pos = 0;
}

static void enc_u8(prof *p, uint8_t v) { pl(p)[p->dr.pos++] = v; }

static void enc_u16(prof *p, uint16_t v)
{
    p_le16(pl(p) + p->dr.pos, v);
    p->dr.pos += 2;
}

static void enc_u32(prof *p, uint32_t v)
{
    p_le32(pl(p) + p->dr.pos, v);
    p->dr.pos += 4;
}

static void enc_u64(prof *p, uint64_t v)
{
    p_le64(pl(p) + p->dr.pos, v);
    p->dr.pos += 8;
}

static void enc_str(prof *p, const char *s, size_t n)
{
    enc_u16(p, (uint16_t)n);
    if (n)
        memcpy(pl(p) + p->dr.pos, s, n);
    p->dr.pos += n;
}

/* Finish the chunk: {type, len, chunk_seq}, payload, CRC over type..payload; one sink write. */
static void enc_end(prof *p)
{
    drainer *d = &p->dr;
    uint8_t *b = d->buf;
#if defined(PS3DK_PROF_WITH_ARCS) || defined(PS3DK_PROF_WITH_WAITS) || defined(PS3DK_PROF_WITH_FUNC) || defined(PS3DK_PROF_WITH_COUNTERS)
    if (d->chunk_seq == UINT64_MAX) {
        sink_fail(&p->snk, -6);
        return;
    }
#elif defined(PS3DK_PROF_WITH_SPU)
    /* Extension mode: the ceiling guards EVERY chunk (v1, extension and the TRAILER), before any sink call.
     * flags = 0 keeps the base behaviour. */
    if (p->ext_flags && d->chunk_seq == UINT64_MAX) {
        sink_fail(&p->snk, -6);
        return;
    }
#endif
    p_le32(b, d->chunk_type);
    p_le32(b + 4, (uint32_t)d->pos);
    p_le64(b + 8, d->chunk_seq);
    p_le32(b + 16 + d->pos, p_crc32(b, 16 + d->pos));
    if (sink_write(p, b, 16 + d->pos + 4) == 0)
        d->chunk_seq++;
}

#ifdef PS3DK_PROF_WITH_WAITS
int p_wait_emit_chunk(prof *p,uint32_t type,const uint8_t *payload,size_t len)
{
    uint64_t seq=p->dr.chunk_seq;
    if(p->snk.failed || len>P_CHUNK_CAP)return -1;
    enc_begin(p,type);
    if(payload!=pl(p))memcpy(pl(p),payload,len);
    p->dr.pos=len;enc_end(p);
    return !p->snk.failed && p->dr.chunk_seq==seq+1 ? 0:-1;
}
#endif
#ifdef PS3DK_PROF_WITH_COUNTERS
/* One COUNTERS chunk (type 39) through the common framing; 0 only when the sink accepted the whole chunk. */
int p_counter_emit_chunk(prof *p, const uint8_t *payload, size_t len)
{
    uint64_t seq = p->dr.chunk_seq;
    if (p->snk.failed || len > P_CHUNK_CAP)
        return -1;
    enc_begin(p, CNT_CHUNK);
    if (payload != pl(p))
        memcpy(pl(p), payload, len);
    p->dr.pos = len;
    enc_end(p);
    return !p->snk.failed && p->dr.chunk_seq == seq + 1 ? 0 : -1;
}
#endif
#ifdef PS3DK_PROF_WITH_FUNC
/* One function-path chunk through the common framing; 0 only when the sink accepted the whole chunk. */
int p_func_emit_chunk(prof *p, uint32_t type, const uint8_t *payload, size_t len)
{
    uint64_t seq = p->dr.chunk_seq;
    if (p->snk.failed || len > P_CHUNK_CAP)
        return -1;
    enc_begin(p, type);
    if (payload != pl(p))
        memcpy(pl(p), payload, len);
    p->dr.pos = len;
    enc_end(p);
    return !p->snk.failed && p->dr.chunk_seq == seq + 1 ? 0 : -1;
}
#endif
#ifdef PS3DK_PROF_WITH_FIFO
/* One FIFO chunk through the common framing, encoded in place in the chunk buffer; 0 only when the sink accepted the
 * whole chunk. */
uint8_t *p_fifo_payload(prof *p) { return pl(p); }
int p_fifo_emit_chunk(prof *p, uint32_t type, const uint8_t *payload, size_t len)
{
    uint64_t seq = p->dr.chunk_seq;
    if (p->snk.failed || len > P_CHUNK_CAP)
        return -1;
    enc_begin(p, type);
    if (payload != pl(p))
        memcpy(pl(p), payload, len);
    p->dr.pos = len;
    enc_end(p);
    return !p->snk.failed && p->dr.chunk_seq == seq + 1 ? 0 : -1;
}
#endif
#ifdef PS3DK_PROF_WITH_SPU
/* One extension chunk through the common framing; 0 only when the sink ACCEPTED the whole chunk. */
int p_ext_emit(prof *p, uint32_t type, const uint8_t *payload, size_t len)
{
    uint64_t seq = p->dr.chunk_seq;
    if (p->snk.failed || len > P_CHUNK_CAP)
        return -1;
    enc_begin(p, type);
    memcpy(pl(p), payload, len);
    p->dr.pos = len;
    enc_end(p);
    return (!p->snk.failed && p->dr.chunk_seq == seq + 1) ? 0 : -1;
}

void p_ext_sink_fail(prof *p, int code) { sink_fail(&p->snk, code); }
#endif

/* Counted lists (DESCRIPTORS, THREADS, RETIRED): split into further chunks when an entry would pass 1 MiB. */
static void list_open(prof *p, uint32_t type, uint32_t *cnt)
{
    enc_begin(p, type);
    enc_u32(p, 0);
    *cnt = 0;
}

static void list_close(prof *p, uint32_t *cnt)
{
    if (*cnt) {
        p_le32(pl(p), *cnt);
        enc_end(p);
    }
    *cnt = 0;
}

static void list_need(prof *p, uint32_t *cnt, size_t need)
{
    if (p->dr.pos + need > P_CHUNK_CAP) {
        uint32_t type = p->dr.chunk_type;
        list_close(p, cnt);
        list_open(p, type, cnt);
    }
}

#define P_FINAL_ENTRY 56u

static void enc_final(prof *p, uint32_t inc, uint8_t state, uint8_t lim, uint64_t produced, uint64_t written,
                      uint64_t dropped, uint64_t refused, uint64_t regerr, uint64_t open_spans)
{
    enc_u32(p, inc);
    enc_u8(p, state);
    enc_u8(p, lim);
    enc_u16(p, 0);
    enc_u64(p, produced);
    enc_u64(p, written);
    enc_u64(p, dropped);
    enc_u64(p, refused);
    enc_u64(p, regerr);
    enc_u64(p, open_spans);
}

/* ---- drain steps (5.4) --------------------------------------------------------------------------------------------- */

#if defined(__clang__)
#define P_COMPILER "clang " __clang_version__
#elif defined(__GNUC__)
#define P_COMPILER "gcc " __VERSION__
#else
#define P_COMPILER "unknown"
#endif

static size_t clen(const char *s)
{
    size_t n = 0;
    while (n < P_STR_MAX && s[n] && (uint8_t)s[n] < 0x80)
        n++;   /* ASCII only, at most 512: always a valid string */
    return n;
}

#ifdef PS3DK_PROF_WITH_PROFILE
#include "prof_identity.h"
/* Link-time declarations (status DECLARED_BUILD): an undefined weak symbol resolves to 0.
 * ps3dk_sample_main exists exactly when the run-hook injection renamed the sample's main
 * (tests/hardware/psgl-sample-boot/run-hook-inject.cmake); ps3dk_prof_live_start when the live members are linked. */
extern int ps3dk_sample_main(int, char **) __attribute__((weak));
extern int ps3dk_prof_live_start(const ps3dk_prof_live_config *cfg) __attribute__((weak));

static void enc_field(prof *p, uint16_t id, uint8_t status, uint8_t width, const uint8_t *value)
{
    uint8_t k;
    enc_u16(p, id);
    enc_u8(p, status);
    enc_u8(p, width);
    for (k = 0; k < width; k++)
        enc_u8(p, value ? value[k] : 0u);
}

/* PROFILE (chunk 2, profile-format-spec.md 3): version 1, the generated identity, the 7 version-1 fields.  Fields
 * this runtime has no source for are NOT_READABLE with zero values (never a guess): SYS_SW_VERSION (no query in the
 * SDK headers), HW_MODEL_CLASS, SPU_USABLE. */
static void enc_profile(prof *p)
{
    const ps3dk_prof__identity_t *id = &ps3dk_prof__identity;
    uint8_t tb[8], hook, live;
    uint8_t n = id->id_len, k;
    p_le64(tb, p->tb_freq);
    hook = ps3dk_sample_main ? 1u : 0u;
    live = ps3dk_prof_live_start ? 1u : 0u;
    enc_begin(p, 17);
    enc_u16(p, 1);
    enc_u16(p, 0);
    enc_u8(p, n); /* written as generated: an invalid length makes the reader refuse the record (fail closed) */
    for (k = 0; k < n && k < 32; k++)
        enc_u8(p, (uint8_t)id->id[k]);
    enc_u32(p, 1); /* ARCHIVE-DIGEST-V1 */
    for (k = 0; k < 32; k++)
        enc_u8(p, id->digest[k]);
    enc_u32(p, id->premise_bits);
    enc_u32(p, id->kinds);
    enc_u16(p, 7);
    enc_field(p, 1, 3, 4, NULL);
    enc_field(p, 2, 3, 4, NULL);
    enc_field(p, 3, 1, 8, tb);
    enc_field(p, 4, 3, 4, NULL);
    enc_field(p, 5, 2, 1, &hook);
    enc_field(p, 6, 2, 1, &live);
    enc_field(p, 7, 1, 40, p->prof_ext_cfg);
    enc_end(p);
}
#endif

/* The file header, HEADER (chunk 0) and CLOCKS (chunk 1), written by the first drain. */
static void dr_start(prof *p)
{
    static const uint8_t magic[16] = {'P', 'S', '3', 'D', 'K', 'P', 'R', 'F', 1, 0, 0, 0, 0, 0, 0, 0};
    uint8_t head[16];
    if (p->dr.started)
        return;
    p->dr.started = 1;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.chunk_seq_seed)
        p->dr.chunk_seq = ps3dk_prof_test_hooks.chunk_seq_seed; /* the chunk-sequence ceiling tests */
#endif
    memcpy(head, magic, sizeof head);
#ifdef PS3DK_PROF_WITH_SPU
    head[12] = p->ext_flags; /* file_flags bit 0 (DOMAIN_EXTENSIONS); 0 when the extension is off or disabled */
#endif
#ifdef PS3DK_PROF_WITH_WAITS
    head[12]|=16;
#endif
#ifdef PS3DK_PROF_WITH_FUNC
    head[12] |= 32; /* file_flags bit 5: function paths (chunk types 28-30) */
#endif
#ifdef PS3DK_PROF_WITH_COUNTERS
    head[13] |= 1; /* file_flags bit 8: named per-frame counters (chunk type 39) */
#endif
#ifdef PS3DK_PROF_WITH_FIFO
    if (p->ext_flags)
        head[12] |= 64; /* file_flags bit 6: FIFO wrap events (chunk types 31-33); needs bit 0 */
#endif
#ifdef PS3DK_PROF_WITH_AUTO
    if (p->ext_flags)
        head[12] |= 128; /* automatic RSX observation: chunks 34..38, requires bit 0 */
#endif
    if (sink_write(p, head, sizeof head) != 0)
        return;
    enc_begin(p, C_HEADER);
    enc_str(p, p->app, p->app_len);
    enc_str(p, p->build_id, p->build_id_len);
    enc_str(p, ps3dk_prof_sdk_version, ps3dk_prof_sdk_version_len);
    enc_str(p, P_COMPILER, clen(P_COMPILER));
    enc_str(p, p->flags, p->flags_len);
    enc_str(p, p->workload_tag, p->workload_tag_len);
    enc_u8(p, sizeof(void *) == 4 ? 1 : 2);
    enc_u8(p, (uint8_t)p->cfg.mode);
    enc_u16(p, 0);
    enc_u64(p, p->tb_at_init);   /* session_id */
    enc_u32(p, p->cfg.ring_records);
    enc_u32(p, p->cfg.max_threads);
    enc_u32(p, p->cfg.max_descriptors);
    enc_end(p);
    enc_begin(p, C_CLOCKS);
    enc_u32(p, 1);
    enc_u32(p, 1);   /* PPU_TB */
    enc_u32(p, 0);
    enc_u64(p, p->tb_freq);
    enc_u64(p, p->tb_at_init);
    enc_end(p);
#ifdef PS3DK_PROF_WITH_PROFILE
    if (p->ext_flags & 2)
        enc_profile(p); /* chunk 2, in the same start step as HEADER and CLOCKS */
#endif
}

static void enc_threads(prof *p, const tdecl *t, uint32_t n)
{
    uint32_t i, cnt;
    size_t nl;
    if (!n)
        return;
    list_open(p, C_THREADS, &cnt);
    for (i = 0; i < n; i++) {
        nl = strlen(t[i].name);
        list_need(p, &cnt, 4 + 4 + 8 + 2 + nl);
        enc_u32(p, t[i].incarnation);
        enc_u32(p, t[i].slot);
        enc_u64(p, t[i].os_id);
        enc_str(p, t[i].name, nl);
        cnt++;
        p->dr.decl_gen[t[i].slot] = t[i].generation;
        p->dr.written[t[i].slot] = 0;
    }
    list_close(p, &cnt);
#ifdef PS3DK_PROF_WITH_SPU
    if (p->ext_flags && !p->snk.failed)
        p_ext_threads_accepted(p, t, n); /* exact identities, only once their THREADS chunks were accepted */
#endif
#ifdef PS3DK_PROF_WITH_WAITS
    if(!p->snk.failed)p_wait_declared(p,t,n);
#endif
}

/* Step 2a: every non-FREE slot whose generation is not declared yet, copied under the mutex, encoded after it. */
static int declare_threads(prof *p)
{
    drainer *d = &p->dr;
    uint32_t i, n = 0;
    if (p_lock(p, &p->thread_mutex) != 0)
        return -1;
    for (i = 0; i < p->nslots; i++) {
        slot *s = &p->slots[i];
        if (s->state != SLOT_FREE && d->decl_gen[i] != s->generation) {
            tdecl *t = &d->tscratch[n++];
            t->slot = i;
            t->generation = s->generation;
            t->incarnation = s->incarnation;
            t->os_id = s->os_id;
            memcpy(t->name, s->name, sizeof t->name);
        }
    }
    p_unlock(p, &p->thread_mutex);
    enc_threads(p, d->tscratch, n);
    return 0;
}

/* Step 2b: the descriptors below upto (an ACQUIRE of desc_count, or Fd). */
static void declare_descs(prof *p, uint32_t upto)
{
    drainer *d = &p->dr;
    uint32_t i, cnt;
    if (d->desc_emitted >= upto)
        return;
    list_open(p, C_DESCRIPTORS, &cnt);
    for (i = d->desc_emitted; i < upto; i++) {
        const desc_entry *e = &p->descs[i];
        list_need(p, &cnt, 4 + 8 + 1 + 1 + 4 + 6 + (size_t)e->name_len + e->tag_len + e->file_len);
        enc_u32(p, i);
        enc_u64(p, e->id);
        enc_u8(p, (uint8_t)e->scope);
        enc_u8(p, 0);
        enc_u32(p, e->line);
        enc_str(p, e->name, e->name_len);
        enc_str(p, e->tag, e->tag_len);
        enc_str(p, e->file, e->file_len);
        cnt++;
    }
    list_close(p, &cnt);
    d->desc_emitted = upto;
}

/* Step 3: one ring up to w, whole publications only (r2 7).  A boundary that fails validation is library-internal
 * corruption: it is handled as a SINK FAILURE (nothing more written, no trailer, ESINKFAIL). */
static void drain_ring(prof *p, uint32_t si, uint32_t inc, uint64_t w)
{
    ring *rg = p->slots[si].rg;
    uint64_t r = LD_RLX(&rg->read_idx);   /* only the drainer stores it */
    uint32_t n, j, cnt = 0;
    const uint8_t *src;
    if (r == w || p->snk.failed)
        return;
    enc_begin(p, C_RECORDS);
    enc_u32(p, inc);
    enc_u32(p, 0);
    while (r < w) {
        n = rg->pub_len[r & rg->mask];
        if (n < 1 || n > P_MAX_PUB || n > w - r)
            goto corrupt;
        for (j = 1; j < n; j++)
            if (rg->pub_len[(r + j) & rg->mask] != 0)
                goto corrupt;
        if (cnt + n > P_CHUNK_RECORDS) {
            p_le32(pl(p) + 4, cnt);
            enc_end(p);
            ST_REL(&rg->read_idx, r);
            enc_begin(p, C_RECORDS);
            enc_u32(p, inc);
            enc_u32(p, 0);
            cnt = 0;
        }
        for (j = 0; j < n; j++) {
            src = rg->slots + (size_t)((r + j) & rg->mask) * P_REC;
            memcpy(pl(p) + p->dr.pos, src, P_REC);
            p->dr.pos += P_REC;
            if (src[0] != K_GAP)
                p->dr.written[si]++;
        }
        cnt += n;
        r += n;
    }
    p_le32(pl(p) + 4, cnt);
    enc_end(p);
    ST_REL(&rg->read_idx, r);   /* the space is free once the records are copied out */
    return;
corrupt:
    p->dr.corrupt = 1;
    sink_fail(&p->snk, -2);
}

#ifdef PS3DK_PROF_WITH_ARCS
/* Called only by the sole drainer, outside all runtime mutexes. For known
 * states its caller acquired the owner's last-write ack; for unknown states
 * it must not inspect even one byte of the mutable collector. Identity stays
 * held until acceptance, so a slow consumer temporarily lowers capacity. */
static int consume_arcs(prof *p, uint32_t slot_id, uint32_t generation,
                        uint32_t incarnation, uint64_t os_id, const char *name, uint32_t state)
{
    struct ps3dk_prof_arc_view v = {0};
    v.generation = generation;
    v.thread.incarnation = incarnation;
    v.thread.slot = slot_id;
    v.thread.os_thread_id = os_id;
    v.thread.name = name;
    v.thread.state = (uint8_t)state;
    if (state == ST_NOT_QUIESCED) {
        v.thread.limit_flags = 2; /* COUNTS_UNKNOWN */
        v.thread.attempted = v.thread.recorded = v.thread.refused = v.thread.lost = UINT64_MAX;
    } else {
        const struct ps3dk_prof_arc_collector *a = &p->slots[slot_id].arcs;
        v.thread.rows = a->rows;
        v.thread.row_count = a->row_count;
        v.thread.limit_flags = a->limit_flags;
        v.thread.attempted = a->attempted;
        v.thread.recorded = a->recorded;
        v.thread.refused = a->refused;
    }
    if (p->snk.failed)
        return -1;
    if (p->arc_options.consume(p->arc_options.ctx, &v) != 0) {
        sink_fail(&p->snk, -5);
        return -1; /* terminal, including partial acceptance; never retry */
    }
    return 0;
}
#endif

/* One ordinary drain: steps 1-4 of 5.4 with the r2 3.1 snapshot protocol.  Never runs after the frontier. */
int p_drain_once(prof *p)
{
    drainer *d = &p->dr;
    uint32_t i, n = 0, cnt, nret = 0;
    if (d->frozen) {
        STAT_ADD(PS3DK_PROF_TEST_STAT_ORDINARY_DRAINS_AFTER_FRONTIER, 1);
        return -1;
    }
    if (p->snk.failed) {
#ifdef PS3DK_PROF_WITH_SPU
        /* no output, but lane lifetime still progresses: end requests are retired and released */
        if (p->ext_flags)
            p_ext_step3b(p);
#endif
        return -1;
    }
    dr_start(p);
#ifdef PS3DK_PROF_WITH_COUNTERS
    /* 0. The counter snapshot BEFORE the table snapshot: every sample below it was made by a thread step 2 declares. */
    if (p_counter_snapshot(p) != 0)
        goto lockfail;
#endif

    /* 1. Snapshot the table under the mutex, then (outside it) each ack word BEFORE its write index, so a RETIRED
     * incarnation's every publication is below the snapshot. */
    if (p_lock(p, &p->thread_mutex) != 0)
        goto lockfail;
    for (i = 0; i < p->nslots; i++) {
        slot *s = &p->slots[i];
        if (s->state != SLOT_FREE) {
            d->snaps[n].slot = i;
            d->snaps[n].generation = s->generation;
            d->snaps[n].incarnation = s->incarnation;
            d->snaps[n].state = s->state;
            n++;
        }
    }
    p_unlock(p, &p->thread_mutex);
    d->nsnaps = n;
    for (i = 0; i < n; i++) {
        slot *s = &p->slots[d->snaps[i].slot];
        d->snaps[i].retire_now = LD_ACQ(&s->cb->ack_retire) == ST_RETIRED;
        d->snaps[i].widx = LD_ACQ(&s->rg->write_idx);
        nret += d->snaps[i].retire_now;
    }
    if (nret) {
        if (p_lock(p, &p->thread_mutex) != 0)
            goto lockfail;
        for (i = 0; i < n; i++)
            if (d->snaps[i].retire_now)
                p->slots[d->snaps[i].slot].state = SLOT_RETIRED_PENDING;
        p_unlock(p, &p->thread_mutex);
    }
    HOOK(drainer_after_snapshot);

    /* 2. Tables. */
    if (declare_threads(p) != 0)
        goto lockfail;
    declare_descs(p, LD_ACQ(&p->desc_count));
    HOOK(drainer_after_tables);

    /* 3. Records up to the snapshot. */
    for (i = 0; i < n; i++)
        drain_ring(p, d->snaps[i].slot, d->snaps[i].incarnation, d->snaps[i].widx);
#ifdef PS3DK_PROF_WITH_AUTO
    if (p->ext_flags)
        p_auto_drain(p, 0);
#endif
#ifdef PS3DK_PROF_WITH_FIFO
    /* 3a. FIFO wrap events, before the extension step.  This step takes no lock; an owner parked in a wrap holds no
     * profiler mutex (no reserve is made under one), so its queued boundaries reach the sink while it waits. */
    if (p->ext_flags)
        p_fifo_drain(p, 0);
#endif
#ifdef PS3DK_PROF_WITH_SPU
    /* 3b. The SPU lane extension: after the v1 records, before retirement frees any owner slot. */
    if (p->ext_flags)
        p_ext_step3b(p);
#endif

#ifdef PS3DK_PROF_WITH_WAITS
    (void)p_wait_emit_ready(p);
#endif
#ifdef PS3DK_PROF_WITH_COUNTERS
    if (p_counter_emit(p, 0) != 0)
        sink_fail(&p->snk, -3); /* a lock failure; a sink failure keeps its own first error */
#endif
#ifdef PS3DK_PROF_WITH_FUNC
    /* 3c. Function paths: published cuts, LAGGING rows and the FINAL of each retiring incarnation, before its slot is
     * freed; then the next cut request. */
    p_func_drain(p, d->snaps, n);
#endif
    /* 4. Retirement: the RETIRED entry, then (and only then) the slot is free. */
    if (nret) {
        list_open(p, C_RETIRED, &cnt);
        for (i = 0; i < n; i++) {
            const snap *sn = &d->snaps[i];
            const cblock *cb = p->slots[sn->slot].cb;
            if (!sn->retire_now)
                continue;
            list_need(p, &cnt, P_FINAL_ENTRY);
            enc_final(p, sn->incarnation, ST_RETIRED, cb->limit ? 1 : 0, cb->next_seq, d->written[sn->slot],
                      cb->dropped, cb->refused, cb->regerr, cb->open_spans);
            cnt++;
            if (cb->limit)
                d->any_retired_limit = 1;
#ifdef PS3DK_PROF_WITH_ARCS
            if (!p->arc_options.max_arcs_per_thread)
#endif
                d->retired_gen[sn->slot] = sn->generation;
        }
        list_close(p, &cnt);
#ifdef PS3DK_PROF_WITH_ARCS
        if (p->arc_options.max_arcs_per_thread) {
            if (p->snk.failed)
                return -1; /* no successful transfer, therefore no reuse */
            for (i = 0; i < n; ++i) {
                const snap *sn = &d->snaps[i];
                const slot *s = &p->slots[sn->slot];
                if (!sn->retire_now)
                    continue;
                if (consume_arcs(p, sn->slot, sn->generation, sn->incarnation, s->os_id, s->name, ST_RETIRED))
                    return -1;
                /* Completion means BOTH timing output and arc acceptance. */
                d->retired_gen[sn->slot] = sn->generation;
            }
        }
#endif
        HOOK(drainer_before_free);
        if (p_lock(p, &p->thread_mutex) != 0)
            goto lockfail;
        for (i = 0; i < n; i++) {
            if (!d->snaps[i].retire_now)
                continue;
            if (d->frozen)
                STAT_ADD(PS3DK_PROF_TEST_STAT_POST_FRONTIER_TRANSITIONS, 1);   /* never: frozen drains return above */
            p->slots[d->snaps[i].slot].state = SLOT_FREE;
            d->decl_gen[d->snaps[i].slot] = 0;
#ifdef PS3DK_PROF_WITH_SPU
            if (p->ext_flags)
                p_ext_slot_freed(p, d->snaps[i].slot);
#endif
        }
        p_unlock(p, &p->thread_mutex);
    }
    STAT_ADD(PS3DK_PROF_TEST_STAT_DRAINS, 1);
    return p->snk.failed ? -1 : 0;

lockfail:
    sink_fail(&p->snk, -3);
    return -1;
}

/* ---- quiescence, the frozen frontier and sealing (7, E1, F2) ------------------------------------------------------- */

static void mark_done(prof *p)
{
    uint32_t i;
    for (i = 0; i < p->dr.nfront; i++) {
        fentry *e = &p->dr.front[i];
        if (!e->done && p->dr.retired_gen[e->slot] == e->generation)
            e->done = 1;   /* an ordinary drain before the frontier already wrote its RETIRED entry */
    }
}

static void quiesce_wait(prof *p)
{
    drainer *d = &p->dr;
    uint64_t now, deadline, rem;
    uint32_t i;
    int all;
    if (p_mono_now(p, &now) != 0) {
        p->clock_failed = 1;   /* Q8: the wait ends at once; unquiesced incarnations stay NOT_QUIESCED */
        return;
    }
    deadline = now > UINT64_MAX - p->cfg.quiesce_timeout_us ? UINT64_MAX : now + p->cfg.quiesce_timeout_us;
    for (;;) {
        all = 1;
        mark_done(p);
        for (i = 0; i < d->nfront && all; i++) {
            const fentry *e = &d->front[i];
            if (!e->done && LD_ACQ(&p->slots[e->slot].cb->ack_retire) == 0)
                all = 0;
        }
        if (all)
            return;
        if (p_mono_now(p, &now) != 0) {
            p->clock_failed = 1;
            return;
        }
        if (now >= deadline)
            return;
        rem = deadline - now;
        pal_sleep_us(rem < 1000 ? rem : 1000);
    }
}

/* Take the frontier exactly once, run the final drain to it, write the TRAILER, flush and close.  Returns the
 * shutdown result in the 8 severity order.  The caller holds the drainer role (in_drain set). */
int p_quiesce_freeze_seal(prof *p)
{
    drainer *d = &p->dr;
    uint32_t i, cnt, ntr = 0, nt = 0, fd, end_flags = 0;
    uint64_t unreg;
    int trailer = 0, any3 = 0, anylim;

#ifdef PS3DK_PROF_WITH_FIFO
    p_fifo_seal_claim(); /* before the wait and every sink check: no FIFO attach publishes after this */
#endif
#ifdef PS3DK_PROF_WITH_AUTO
    p_auto_seal_claim();
#endif
    quiesce_wait(p);
    HOOK(before_frontier);

    /* The frontier, in the 7 order.  Step 1: the classification by the ack/retire PUBLICATION (E1), permanent. */
    mark_done(p);
    for (i = 0; i < d->nfront; i++) {
        fentry *e = &d->front[i];
        uint32_t w;
        if (e->done)
            continue;
        w = LD_ACQ(&p->slots[e->slot].cb->ack_retire);
        e->cls = w == ST_ACKED ? ST_ACKED : w == ST_RETIRED ? ST_RETIRED : ST_NOT_QUIESCED;
    }
    for (i = 0; i < d->nfront; i++)   /* step 2: F[i] */
        if (!d->front[i].done)
            d->front[i].widx = LD_ACQ(&p->slots[d->front[i].slot].rg->write_idx);
    fd = LD_ACQ(&p->desc_count);      /* step 3: Fd (Ft was fixed by the latch) */
    for (i = 0; i < d->nfront; i++) { /* step 4: the final counters of ACKED and RETIRED entries */
        fentry *e = &d->front[i];
        const cblock *cb = p->slots[e->slot].cb;
        if (e->done || e->cls == ST_NOT_QUIESCED)
            continue;
        e->produced = cb->next_seq;
        e->dropped = cb->dropped;
        e->refused = cb->refused;
        e->regerr = cb->regerr;
        e->open_spans = cb->open_spans;
        e->limit = cb->limit;
    }
    unreg = __atomic_load_n(&p->unregistered_refusals, __ATOMIC_RELAXED);   /* one read, informational */
#ifdef PS3DK_PROF_WITH_ARCS
    p->prefix_unregistered = unreg; /* same frozen value as the timing trailer and arc finish */
#endif
    ST_REL(&d->frozen, 1);
#ifdef PS3DK_PROF_WITH_WAITS
    p_wait_freeze(p);
#endif
#ifdef PS3DK_PROF_WITH_COUNTERS
    if (p_counter_freeze(p) != 0) /* after the frontier's thread set is fixed: every sample's incarnation is in it */
        sink_fail(&p->snk, -3);   /* no FINAL from an older snapshot */
#endif
    ST_REL(&p->phase, 2u);            /* SEALING; self-instrumentation is off (the caller is in_drain) */

    if (LD_RLX(&p->lock_failed))
        sink_fail(&p->snk, -3);
    if (!p->snk.failed) {
        /* The final drain: exactly the incarnations below Ft, the descriptors below Fd, the records below F[i]. */
        dr_start(p);
        for (i = 0; i < d->nfront; i++) {
            const fentry *e = &d->front[i];
            if (e->done || d->decl_gen[e->slot] == e->generation)
                continue;
            d->tscratch[nt].slot = e->slot;
            d->tscratch[nt].generation = e->generation;
            d->tscratch[nt].incarnation = e->incarnation;
            d->tscratch[nt].os_id = e->os_id;
            memcpy(d->tscratch[nt].name, e->name, sizeof e->name);
            nt++;
        }
        enc_threads(p, d->tscratch, nt);
        declare_descs(p, fd);
        for (i = 0; i < d->nfront; i++)
            if (!d->front[i].done)
                drain_ring(p, d->front[i].slot, d->front[i].incarnation, d->front[i].widx);
#ifdef PS3DK_PROF_WITH_FUNC
        p_func_seal(p); /* after the frontier's records, before its RETIRED and TRAILER */
#endif

        /* F2: frozen-RETIRED entries in a RETIRED chunk, from the step-4 counters; the slot is NOT freed. */
        list_open(p, C_RETIRED, &cnt);
        for (i = 0; i < d->nfront; i++) {
            const fentry *e = &d->front[i];
            if (e->done || e->cls != ST_RETIRED)
                continue;
            list_need(p, &cnt, P_FINAL_ENTRY);
            enc_final(p, e->incarnation, ST_RETIRED, e->limit ? 1 : 0, e->produced, d->written[e->slot], e->dropped,
                      e->refused, e->regerr, e->open_spans);
            cnt++;
            if (e->limit)
                d->any_retired_limit = 1;
        }
        list_close(p, &cnt);

#ifdef PS3DK_PROF_WITH_ARCS
        if (p->arc_options.max_arcs_per_thread) {
            for (i = 0; i < d->nfront; ++i) {
                const fentry *e = &d->front[i];
                if (!e->done && consume_arcs(p, e->slot, e->generation, e->incarnation,
                                             e->os_id, e->name, e->cls))
                    goto close_capture;
            }
            if (p->snk.failed)
                goto close_capture;
            /* The EXACT frozen trailer value, not a second load/counter.
             * Required even when ordinary drains retired every producer. */
            if (p->arc_options.finish(p->arc_options.ctx, unreg) != 0) {
                sink_fail(&p->snk, -5);
                goto close_capture;
            }
        }
#endif
#ifdef PS3DK_PROF_WITH_SPU
        /* The extension seal: after the frontier's THREADS and RETIRED, before the TRAILER. */
        if (p->ext_flags)
            p_ext_seal(p);
#endif
#ifdef PS3DK_PROF_WITH_AUTO
        if (p->ext_flags)
            p_auto_drain(p, 1);
#endif
#ifdef PS3DK_PROF_WITH_FIFO
        if (p->ext_flags)
            p_fifo_drain(p, 1); /* after the extension seal (its DOMAIN declarations), before the TRAILER */
#endif
#ifdef PS3DK_PROF_WITH_WAITS
        (void)p_wait_emit_status(p);
#endif
#ifdef PS3DK_PROF_WITH_COUNTERS
        if (p_counter_emit(p, 1) != 0) /* the FINAL COUNTERS chunk, after the frontier's THREADS, before the TRAILER */
            sink_fail(&p->snk, -3);    /* no TRAILER after a failed FINAL */
#endif
        /* The TRAILER: ACKED and NOT_QUIESCED entries (6). */
        anylim = d->any_retired_limit;
        for (i = 0; i < d->nfront; i++) {
            const fentry *e = &d->front[i];
            if (e->done || e->cls == ST_RETIRED)
                continue;
            ntr++;
            if (e->cls == ST_NOT_QUIESCED)
                any3 = 1;
            else if (e->limit)
                anylim = 1;
        }
        end_flags = (any3 ? 1u : 0u) | (anylim ? 2u : 0u) | (p->snk.error_count ? 4u : 0u);
        enc_begin(p, C_TRAILER);
        enc_u32(p, end_flags);
        enc_u32(p, ntr);
        for (i = 0; i < d->nfront; i++) {
            const fentry *e = &d->front[i];
            if (e->done || e->cls == ST_RETIRED)
                continue;
            if (e->cls == ST_NOT_QUIESCED)
                enc_final(p, e->incarnation, ST_NOT_QUIESCED, 0x02, P_UNKNOWN, d->written[e->slot], P_UNKNOWN,
                          P_UNKNOWN, P_UNKNOWN, P_UNKNOWN);
            else
                enc_final(p, e->incarnation, ST_ACKED, e->limit ? 1 : 0, e->produced, d->written[e->slot],
                          e->dropped, e->refused, e->regerr, e->open_spans);
        }
        enc_u64(p, p->snk.error_count);
        enc_u32(p, (uint32_t)p->snk.first_error);
        enc_u32(p, 0);
        enc_u64(p, unreg);
        enc_u64(p, d->desc_emitted);
        enc_end(p);
        trailer = !p->snk.failed;
        p->prefix_trailer = (uint32_t)trailer;
        if (trailer && p->snk.kind == 2 && p->snk.cb.flush && p->snk.cb.flush(p->snk.cb.ctx) < 0)
            sink_fail(&p->snk, -4);   /* B5: a flush failure is a sink failure before close */
    }
#ifdef PS3DK_PROF_WITH_ARCS
close_capture:
#endif
#ifdef PS3DK_PROF_WITH_FIFO
    p_fifo_seal_close();
#endif
#ifdef PS3DK_PROF_WITH_SPU
    /* a failed sink skipped the seal above: retire and release every lane anyway (psp_seal is idempotent) */
    if (p->ext_flags)
        p_ext_seal(p);
#endif
    sink_close(p);

    if (p->snk.failed || !trailer)
        return PS3DK_PROF_ESINKFAIL;
    if (p->snk.close_failed)
        return PS3DK_PROF_ECLOSE;
    if (end_flags & 1u)
        return PS3DK_PROF_ENOTQUIESCED;
    if (end_flags & 2u)
        return PS3DK_PROF_ESEQLIMIT;
    if (end_flags & 4u)
        return PS3DK_PROF_ESINKRECOVERED;
    if (p->clock_failed)
        return PS3DK_PROF_ECLOCK;
    return PS3DK_PROF_OK;
}

/* ---- the API and the drain thread ----------------------------------------------------------------------------------- */

static prof *ready_prof(void) { return p_ready() ? g_prof : NULL; }

int ps3dk_prof__shutdown_claimed(void)
{
    prof *p = ready_prof();
    return p && LD_ACQ(&p->shutdown_once) != 0u;
}

const struct ps3dk_prof_finalization *ps3dk_prof__finalization(void)
{
    prof *p = ready_prof();
    return p && LD_ACQ(&p->finalization_ready) ? &p->finalization : NULL;
}

/* Only the winning shutdown calls this, after close and (THREAD mode) join.
 * SEALED alone is insufficient: the join outcome is not yet known there.
 * Text refers to immutable runtime-owned or static storage, never caller cfg. */
static void publish_finalization(prof *p, int result)
{
    p->finalization = (struct ps3dk_prof_finalization){
        .accepted_bytes = p->prefix_bytes,
        .trailer_written = (uint8_t)p->prefix_trailer,
        .io_finalized = p->prefix_trailer && p->snk.closed && !p->snk.failed && !p->snk.close_failed && !p->join_failed,
        .join_failed = (uint8_t)p->join_failed, .shutdown_result = result,
#ifdef PS3DK_PROF_WITH_ARCS
        .completed_chunks = p->dr.chunk_seq,
        .last_chunk_sequence = p->dr.chunk_seq ? p->dr.chunk_seq - 1 : UINT64_MAX,
        .prefix_crc32 = p->prefix_crc32,
        .unregistered_refusals = p->prefix_unregistered,
        .session_id = p->tb_at_init, .freq_hz = p->tb_freq, .tb_at_init = p->tb_at_init,
        .abi = sizeof(void *) == 4 ? 1 : 2, .consumer_mode = (uint8_t)p->cfg.mode,
        .app = {p->app, p->app_len}, .build_id = {p->build_id, p->build_id_len},
        .sdk_version = {ps3dk_prof_sdk_version, ps3dk_prof_sdk_version_len},
        .compiler_identity = {P_COMPILER, (uint32_t)clen(P_COMPILER)},
        .flags = {p->flags, p->flags_len}, .workload_tag = {p->workload_tag, p->workload_tag_len},
#endif
    };
    ST_REL(&p->finalization_ready, 1u);
}

int ps3dk_prof_drain(void)
{
    prof *p = ready_prof();
    tls_state *t = &g_tls;
    uint32_t z = 0;
    int rc;
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (t->in_drain)
        return PS3DK_PROF_EBUSY;   /* reentrant, e.g. from a sink callback */
    if (p->cfg.mode == PS3DK_PROF_MODE_THREAD)
        return PS3DK_PROF_EBUSY;   /* the internal drain thread is the only drainer */
    if (!pal_owner_is_self(&p->owner))
        return PS3DK_PROF_EPERM;   /* a failed identity read is never the owner (Q1) */
    if (!CAS_ACQREL(&p->drain_busy, &z, 1u))
        return PS3DK_PROF_EBUSY;
    if (LD_ACQ(&p->stop_latch)) {
        rc = PS3DK_PROF_ESTOPPED;
    } else {
        t->in_drain = 1;
        rc = p_drain_once(p) == 0 ? PS3DK_PROF_OK : PS3DK_PROF_ESINKFAIL;
        t->in_drain = 0;
    }
    ST_REL(&p->drain_busy, 0u);
    return rc;
}

void *p_drain_main(void *arg)
{
    prof *p = (prof *)arg;
    tls_state *t = &g_tls;
    uint64_t it = 0;
    uint32_t z;
    int rc;

    t->in_drain = 1;   /* the drain thread is inside the drainer for its whole life: shutdown from it is EBUSY */
    while (!LD_ACQ(&p->start_gate))
        pal_sleep_us(100);
    for (;;) {
        it++;
        STAT_SET(PS3DK_PROF_TEST_STAT_DRAIN_THREAD_ITERATIONS, it);
        if (LD_ACQ(&p->wake)) {
            STAT_SET(PS3DK_PROF_TEST_STAT_WAKE_SEEN_ITERATION, it);
            break;
        }
        z = 0;
        if (CAS_ACQREL(&p->drain_busy, &z, 1u)) {
            (void)p_drain_once(p);
            ST_REL(&p->drain_busy, 0u);
        }
        pal_sleep_us(p->cfg.drain_interval_us);   /* one slice, then the wake flag is checked */
    }
    (void)it;   /* read only by the test statistics */
    if (t->slot1 && !t->acked && !t->retired)
        p_observe_stop(p);   /* the drain thread self-acks if a callback registered it */
    rc = p_quiesce_freeze_seal(p);
    __atomic_store_n(&p->thread_result, rc, __ATOMIC_RELEASE);
    ST_REL(&p->phase, 3u);
    return NULL;
}

int ps3dk_prof_shutdown(void)
{
    prof *p = ready_prof();
    tls_state *t = &g_tls;
    uint32_t z = 0, i, n = 0;
    int locked, rc;

    if (!p)
        return PS3DK_PROF_ENOTINIT;
    /* The 7 entry checks, in order, before any state change, lock or join. */
    if (t->in_drain)
        return PS3DK_PROF_EBUSY;
    if (p->cfg.mode == PS3DK_PROF_MODE_APP && !pal_owner_is_self(&p->owner))
        return PS3DK_PROF_EPERM;
    if (!CAS_ACQREL(&p->shutdown_once, &z, 1u))
        return PS3DK_PROF_EALREADY;
    HOOK(after_shutdown_claim);   /* claimed: shutdown has begun, the stop latch is not yet set */
#ifdef PS3DK_PROF_WITH_SPU
    {
        /* Capture RSX frame (S2, R40): the stopper sets STOP right after the claim, before the seal */
        const rsxcap_ops *ro = __atomic_load_n(&p->rsxcap, __ATOMIC_ACQUIRE);
        if (ro)
            ro->stop(p);
    }
#endif

    /* The latch, under thread_mutex, fixing Ft (5.4).  A lock failure still latches, and fails the capture closed. */
    locked = p_lock(p, &p->thread_mutex) == 0;
    ST_REL(&p->stop_latch, 1u);
    for (i = 0; i < p->nslots; i++) {
        const slot *s = &p->slots[i];
        fentry *e;
        if (s->state == SLOT_FREE)
            continue;
        e = &p->dr.front[n++];
        memset(e, 0, sizeof *e);
        e->slot = i;
        e->generation = s->generation;
        e->incarnation = s->incarnation;
        e->os_id = s->os_id;
        memcpy(e->name, s->name, sizeof e->name);
    }
    p->dr.nfront = n;
    if (locked)
        p_unlock(p, &p->thread_mutex);
    ST_REL(&p->phase, 1u);   /* STOPPING */

    if (t->slot1 && !t->acked && !t->retired)
        p_observe_stop(p);   /* self-quiescence: the caller never waits for itself */

    if (p->cfg.mode == PS3DK_PROF_MODE_APP) {
        z = 0;
        if (!CAS_ACQREL(&p->drain_busy, &z, 1u))
            return PS3DK_PROF_EBUSY;   /* unreachable: only the owner drains, and it is not in a drain */
        t->in_drain = 1;
        rc = p_quiesce_freeze_seal(p);
        t->in_drain = 0;
        ST_REL(&p->drain_busy, 0u);
        ST_REL(&p->phase, 3u);
        publish_finalization(p, rc);
        return rc;
    }

    STAT_SET(PS3DK_PROF_TEST_STAT_WAKE_SET_ITERATION, STAT_GET(PS3DK_PROF_TEST_STAT_DRAIN_THREAD_ITERATIONS));
    ST_REL(&p->wake, 1u);
    if (pal_thread_join(p->drain_thread) != 0) {
        p->join_failed = 1;
        while (LD_ACQ(&p->phase) != 3u)   /* the result is still the drain thread's; wait for it */
            pal_sleep_us(1000);
    }
    rc = __atomic_load_n(&p->thread_result, __ATOMIC_ACQUIRE);
    if (rc == PS3DK_PROF_OK && p->join_failed)
        rc = PS3DK_PROF_ETHREAD;   /* after every sink and trailer severity */
    publish_finalization(p, rc);
    return rc;
}
