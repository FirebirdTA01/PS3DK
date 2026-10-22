/* ps3dk-prof drainer: the chunk encoder, the sink, ordinary drains, the drain thread, the quiescence wait, the frozen
 * frontier, sealing, ps3dk_prof_drain and ps3dk_prof_shutdown.  The drainer is the ONLY consumer (5.4) and the only
 * code that frees a slot (r2 3.1).  References as in prof_core.c. */
#include "prof_internal.h"

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
    p_le32(b, d->chunk_type);
    p_le32(b + 4, (uint32_t)d->pos);
    p_le64(b + 8, d->chunk_seq);
    p_le32(b + 16 + d->pos, p_crc32(b, 16 + d->pos));
    if (sink_write(p, b, 16 + d->pos + 4) == 0)
        d->chunk_seq++;
}

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

/* The file header, HEADER (chunk 0) and CLOCKS (chunk 1), written by the first drain. */
static void dr_start(prof *p)
{
    static const uint8_t magic[16] = {'P', 'S', '3', 'D', 'K', 'P', 'R', 'F', 1, 0, 0, 0, 0, 0, 0, 0};
    if (p->dr.started)
        return;
    p->dr.started = 1;
    if (sink_write(p, magic, sizeof magic) != 0)
        return;
    enc_begin(p, C_HEADER);
    enc_str(p, p->app, p->app_len);
    enc_str(p, p->build_id, p->build_id_len);
    enc_str(p, PS3DK_PROF_SDK_VERSION, clen(PS3DK_PROF_SDK_VERSION));
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

/* One ordinary drain: steps 1-4 of 5.4 with the r2 3.1 snapshot protocol.  Never runs after the frontier. */
int p_drain_once(prof *p)
{
    drainer *d = &p->dr;
    uint32_t i, n = 0, cnt, nret = 0;
    if (d->frozen) {
        STAT_ADD(PS3DK_PROF_TEST_STAT_ORDINARY_DRAINS_AFTER_FRONTIER, 1);
        return -1;
    }
    if (p->snk.failed)
        return -1;
    dr_start(p);

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

    /* 3. Records up to the snapshot. */
    for (i = 0; i < n; i++)
        drain_ring(p, d->snaps[i].slot, d->snaps[i].incarnation, d->snaps[i].widx);

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
            d->retired_gen[sn->slot] = sn->generation;
        }
        list_close(p, &cnt);
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
    ST_REL(&d->frozen, 1);
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
        if (trailer && p->snk.kind == 2 && p->snk.cb.flush && p->snk.cb.flush(p->snk.cb.ctx) < 0)
            sink_fail(&p->snk, -4);   /* B5: a flush failure is a sink failure before close */
    }
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
    return rc;
}
