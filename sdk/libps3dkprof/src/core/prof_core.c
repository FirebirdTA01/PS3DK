/* ps3dk-prof portable core: init and the sink setters, thread registration and retirement, zones, frames, the STOP
 * batch and descriptor registration.  The drainer, the sink and sealing are in prof_drain.c.
 *
 * References: "5.1" etc. are sections of docs/design/ps3dk-prof-format.md; "r2 3.2" is STAGE3-PLAN-r2; "E2" and "F1"
 * are its errata 1 and 2; "BC" is the boundary correction (format 5.1, "Counter ceiling (fail-closed)").
 * CODE-REVIEW.md maps every rule to the code. */
#include "prof_internal.h"

#include <stdio.h>
#include <string.h>

prof *g_prof;
uint32_t g_gate;
_Thread_local tls_state g_tls;

/* The single capture's state.  Never freed once the gate is READY (7, storage lifetime). */
static prof g_storage;

/* The sink chosen by the setters (E3): written only while the gate is CONFIGURING, read only while INITIALIZING. */
static struct {
    int kind;                   /* 0 default directory, 1 a set directory, 2 callbacks */
    char dir[256];
    ps3dk_prof_sink_callbacks cb;
} g_pending;

#ifdef PS3DK_PROF_TEST_HOOKS
ps3dk_prof_test_hooks_t ps3dk_prof_test_hooks;
uint64_t ps3dk_prof_test_stats[128];
#endif

/* ---- primitives --------------------------------------------------------------------------------------------------- */

void p_le16(uint8_t *d, uint16_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
}

void p_le32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16);
    d[3] = (uint8_t)(v >> 24);
}

void p_le64(uint8_t *d, uint64_t v)
{
    p_le32(d, (uint32_t)v);
    p_le32(d + 4, (uint32_t)(v >> 32));
}

uint64_t p_fnv(uint64_t h, const void *data, size_t n)
{
    const uint8_t *b = (const uint8_t *)data;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint32_t crc_table[256];

/* Called only by init (gate INITIALIZING), before anything that reads the table can run. */
void p_crc32_init(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        crc_table[i] = c;
    }
}

uint32_t p_crc32(const void *data, size_t n)
{
    const uint8_t *b = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < n; i++)
        c = crc_table[(c ^ b[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* 5.5: the timebase read between compiler barriers. */
uint64_t p_tb(void)
{
    uint64_t t;
    __asm__ __volatile__("" ::: "memory");
    t = pal_tb();
    __asm__ __volatile__("" ::: "memory");
    return t;
}

uint64_t p_tb_freq(void) { return pal_tb_freq(); }

int p_mono_now(prof *p, uint64_t *us)
{
    (void)p;
    return pal_mono_now_us(us);
}

/* Every mutex call is checked (r2 2).  A failure is recorded; the capture then fails closed at sealing. */
int p_lock(prof *p, pal_mutex *m)
{
    if (pal_mutex_lock(m) != 0) {
        ST_RLX(&p->lock_failed, 1);
        return -1;
    }
    return 0;
}

void p_unlock(prof *p, pal_mutex *m)
{
    if (pal_mutex_unlock(m) != 0)
        ST_RLX(&p->lock_failed, 1);
}

static prof *ready(void) { return LD_ACQ(&g_gate) == G_READY ? g_prof : NULL; }

int p_ready(void) { return ready() != NULL; }

/* ---- strings (1: u16-length UTF-8, no NUL, at most 512 bytes) ---------------------------------------------------- */

static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static int utf8_valid(const char *str, size_t n)
{
    const uint8_t *s = (const uint8_t *)str;
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        uint32_t cp, min;
        size_t len, j;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1Fu;
            min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0Fu;
            min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07u;
            min = 0x10000;
        } else {
            return 0;
        }
        if (n - i < len)
            return 0;
        for (j = 1; j < len; j++) {
            if ((s[i + j] & 0xC0) != 0x80)
                return 0;
            cp = (cp << 6) | (s[i + j] & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += len;
    }
    return 1;
}

/* The length of s cut to at most max bytes, backed off to a UTF-8 character boundary. */
static size_t utf8_prefix(const char *s, size_t max)
{
    size_t n = bounded_len(s, max + 1);
    if (n <= max)
        return n;
    n = max;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80)
        n--;
    return n;
}

/* A thread name: at most 63 bytes, every non-ASCII byte replaced, so the THREADS string is always valid. */
static void copy_name(char *dst, const char *src)
{
    size_t i = 0;
    if (src) {
        for (; i < 63 && src[i]; i++)
            dst[i] = ((uint8_t)src[i] < 0x80) ? src[i] : '?';
    }
    dst[i] = 0;
}

/* ---- counters (BC: exact, or fail closed) ------------------------------------------------------------------------ */

/* A counter or id ceiling was reached: the incarnation records nothing more (SEQ_LIMIT, limit_flags bit0). */
static void exhaust(cblock *cb)
{
    cb->limit = 1;
    cb->exhausted = 1;
}

/* refused_count += k, never past 2^63.  An increment that does not fit is NOT applied; SEQ_LIMIT records the loss. */
static void count_refused(cblock *cb, uint64_t k)
{
    if (cb->refused > P_CEIL - k) {
        exhaust(cb);
        return;
    }
    cb->refused += k;
}

/* ---- ring publication (5.1, r2 7) --------------------------------------------------------------------------------- */

static uint8_t *rslot(ring *rg, uint64_t idx) { return rg->slots + (size_t)(idx & rg->mask) * P_REC; }

static void put_rec(uint8_t *d, uint8_t kind, uint8_t depth, uint16_t flags, uint32_t a, uint64_t seq, uint64_t b,
                    uint64_t c)
{
    d[0] = kind;
    d[1] = depth;
    p_le16(d + 2, flags);
    p_le32(d + 4, a);
    p_le64(d + 8, seq);
    p_le64(d + 16, b);
    p_le64(d + 24, c);
}

/* 5.1 admission of an n-slot event: 1 = it may take sequences; 0 = REFUSED, nothing assigned.  An event that would
 * need sequence 2^63 is refused WHOLE and sets SEQ_LIMIT. */
static int seq_admit(cblock *cb, uint32_t n)
{
    if (cb->limit)
        return 0;
    if (cb->next_seq > P_CEIL - n) {
        cb->limit = 1;
        return 0;
    }
    return 1;
}

static uint64_t seq_assign(cblock *cb, uint32_t n)
{
    uint64_t first = cb->next_seq;
    cb->next_seq += n;
    STEP(PS3DK_PROF_TEST_STEP_ASSIGN, n);
    return first;
}

/* Reserve one publication for n ASSIGNED slots [first, first + n - 1], plus the pending GAP if there is one.  The whole
 * publication's space is checked before any slot is written.  Returns 1 with *ws = the publication's first slot and
 * *we = its first event slot (the GAP, if any, is already written at *ws); returns 0 if the event was DROPPED. */
static int reserve(cblock *cb, ring *rg, uint32_t n, uint64_t first, uint64_t *ws, uint64_t *we)
{
    uint64_t w = LD_RLX(&rg->write_idx);   /* only this producer stores it */
    uint64_t r = LD_ACQ(&rg->read_idx);
    uint32_t need = n + (cb->pend ? 1u : 0u);
    if ((uint64_t)rg->cap - (w - r) < need) {
        if (!cb->pend) {
            cb->pend = 1;
            cb->pend_first = first;
        }
        cb->pend_last = first + n - 1;
        cb->dropped += n;
        STEP(PS3DK_PROF_TEST_STEP_DROP, n);
        return 0;
    }
    *ws = w;
    if (cb->pend) {
        put_rec(rslot(rg, w), K_GAP, 0, 0, 0, cb->pend_first, cb->pend_first, cb->pend_last);
        cb->pend = 0;
        w++;
    }
    *we = w;
    return 1;
}

/* Mark the publication [ws, wend) and publish it with ONE release store of write_idx. */
static void commit(ring *rg, uint64_t ws, uint64_t wend)
{
    uint32_t n = (uint32_t)(wend - ws), i;
    rg->pub_len[ws & rg->mask] = (uint16_t)n;
    for (i = 1; i < n; i++)
        rg->pub_len[(ws + i) & rg->mask] = 0;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.corrupt_next_pub_len) {
        ps3dk_prof_test_hooks.corrupt_next_pub_len = 0;
        rg->pub_len[ws & rg->mask] = (uint16_t)(P_MAX_PUB + 1);
    }
#endif
    HOOK(after_reserve_before_publish);
    STEP(PS3DK_PROF_TEST_STEP_PUBLISH, n);
    ST_REL(&rg->write_idx, wend);
}

/* One 1-slot logical event (END, REGERR): 1 = assigned (published or dropped); 0 = REFUSED and counted. */
static int emit1(cblock *cb, ring *rg, uint8_t kind, uint8_t depth, uint16_t flags, uint32_t a, uint64_t b, uint64_t c)
{
    uint64_t seq, ws, we;
    if (!seq_admit(cb, 1)) {
        count_refused(cb, 1);
        return 0;
    }
    seq = seq_assign(cb, 1);
    if (reserve(cb, rg, 1, seq, &ws, &we)) {
        put_rec(rslot(rg, we), kind, depth, flags, a, seq, b, c);
        commit(rg, ws, we + 1);
    }
    return 1;
}

/* ---- descriptor registration (5.3, Q5) ---------------------------------------------------------------------------- */

/* 5.3: increment regerr_count unconditionally, then try a REGERR event.  BC: at the ceiling the increment is not
 * applied, the incarnation fails closed, and no REGERR is attempted (a REGERR never outnumbers its counter). */
static void regerr(cblock *cb, ring *rg, uint32_t code, const char *name, size_t nl, const char *tag, size_t tl,
                   uint64_t id)
{
    uint64_t key;
    if (cb->regerr >= P_CEIL) {
        exhaust(cb);
        return;
    }
    cb->regerr++;
    key = p_fnv(P_FNV_BASIS, name, nl);
    key = p_fnv(key, "", 1);
    key = p_fnv(key, tag, tl);
    (void)emit1(cb, rg, K_REGERR, 0, 0, code, key, id);
}

/* The cold path: lock, RE-CHECK the call-site state, then register or fail.  Returns the new call-site state (index + 1
 * or a failure code), or 0 if the lock failed (nothing cached; the zone is refused). */
static uint32_t desc_register(prof *p, cblock *cb, ring *rg, ps3dk_prof_desc *d)
{
    const char *tag = d->logical_tag ? d->logical_tag : "";
    const char *file = d->file ? d->file : "";
    size_t nl = 0, tl, fl;
    uint32_t st, n, i, code = 0;
    uint64_t id = 0, cand, h;
    uint8_t le[4];
    desc_entry *e;

    if (p_lock(p, &p->desc_mutex) != 0)
        return 0;
    st = LD_RLX(&d->state);   /* stored only under desc_mutex */
    if (st != 0)
        goto unlock;
    tl = utf8_prefix(tag, P_STR_MAX);
    fl = utf8_prefix(file, P_STR_MAX);
    if (d->name)
        nl = utf8_prefix(d->name, P_STR_MAX);
    if (nl == 0 || (d->scope != 1 && d->scope != 2) || !utf8_valid(d->name, nl) || !utf8_valid(tag, tl) ||
        !utf8_valid(file, fl)) {
        st = DESC_FAILED_INVALID;   /* a descriptor the format cannot carry is refused, never written */
        goto out;
    }
    p_le32(le, d->line);
    h = p_fnv(p->fnv_build, file, fl);
    h = p_fnv(h, "", 1);
    h = p_fnv(h, le, 4);
    id = p_fnv(h, d->name, nl);
    cand = id;
#ifdef PS3DK_PROF_TEST_HOOKS
    /* SYNTHETIC branch coverage only: a test may replace the candidate id of this comparison (never the stored id). */
    if (ps3dk_prof_test_hooks.candidate_desc_id)
        cand = ps3dk_prof_test_hooks.candidate_desc_id(d->name, d->line, id);
#endif
    n = LD_RLX(&p->desc_count);   /* stored only under desc_mutex */
    for (i = 0; i < n; i++) {
        e = &p->descs[i];
        if (e->id != cand)
            continue;
        if (e->line == d->line && e->name_len == nl && e->file_len == fl && memcmp(e->name, d->name, nl) == 0 &&
            memcmp(e->file, file, fl) == 0) {
            st = i + 1;   /* the same (file, line, name) from another static: one descriptor */
            goto out;
        }
        code = 2;
        st = DESC_FAILED_COLLISION;
        goto out;
    }
    if (n >= p->cfg.max_descriptors) {
        code = 1;
        st = DESC_FAILED_TABLE_FULL;
        goto out;
    }
    e = &p->descs[n];
    e->name = d->name;
    e->tag = tag;
    e->file = file;
    e->name_len = (uint32_t)nl;
    e->tag_len = (uint32_t)tl;
    e->file_len = (uint32_t)fl;
    e->line = d->line;
    e->scope = d->scope;
    e->id = id;
    HOOK(between_desc_entry_and_publish);
    ST_REL(&p->desc_count, n + 1);   /* the complete entry, then the count */
    st = n + 1;
out:
    ST_REL(&d->state, st);           /* then the call site */
unlock:
    p_unlock(p, &p->desc_mutex);
    if (code)
        regerr(cb, rg, code, d->name, nl, tag, tl, id);   /* outside the mutex; c = the id it would have had */
    return st;
}

/* 1 = *idx is a published descriptor index; 0 = the zone is REFUSED (the caller counts it). */
static int desc_index(prof *p, cblock *cb, ring *rg, ps3dk_prof_desc *d, uint32_t *idx)
{
    uint32_t st = LD_ACQ(&d->state);
    if (st == 0)
        st = desc_register(p, cb, rg, d);
    if (st == 0 || st > p->cfg.max_descriptors)
        return 0;
    *idx = st - 1;
    return 1;
}

/* ---- threads (5.4, 7, r2 3.1, E1) --------------------------------------------------------------------------------- */

static int thread_begin_impl(prof *p, const char *name)
{
    tls_state *t = &g_tls;
    slot *s = NULL;
    uint32_t i, inc;
    uint64_t os_id;

    if (t->slot1 && !t->acked && !t->retired)
        return PS3DK_PROF_EALREADY;
    if (t->acked)
        return PS3DK_PROF_ESTOPPED;   /* an ack implies the latch */
    if (p_lock(p, &p->thread_mutex) != 0)
        return PS3DK_PROF_ETHREAD;
    if (LD_ACQ(&p->stop_latch)) {     /* 5.4: the incarnation set is fixed at the latch */
        p_unlock(p, &p->thread_mutex);
        return PS3DK_PROF_ESTOPPED;
    }
    for (i = 0; i < p->nslots; i++) {
        if (p->slots[i].state == SLOT_FREE) {
            s = &p->slots[i];
            break;
        }
    }
    if (!s || p->next_incarnation == 0) {
        __atomic_fetch_add(&p->unregistered_refusals, 1, __ATOMIC_RELAXED);
        p_unlock(p, &p->thread_mutex);
        return PS3DK_PROF_EFULL;
    }
    /* R3 (codex 2026-10-22): the OS thread id is read BEFORE anything changes.  A failed read refuses the registration
     * with ETHREAD while the slot is still FREE and the incarnation counter, the slot generation and the TLS state are
     * untouched, so there is nothing to roll back and no placeholder id is ever recorded. */
    if (pal_os_thread_id(&os_id) != 0) {
        p_unlock(p, &p->thread_mutex);
        return PS3DK_PROF_ETHREAD;
    }
    inc = p->next_incarnation;
    p->next_incarnation = (inc == UINT32_MAX) ? 0 : inc + 1;   /* 0: ids exhausted, later registrations refused */
    s->generation++;
    s->incarnation = inc;
    s->os_id = os_id;
    copy_name(s->name, name);
    /* Reset in FREE state, under the mutex, before the LIVE publication (r2 3.1).  The drainer's last access to the
     * previous incarnation happened before it freed the slot under this mutex. */
    memset(s->cb, 0, sizeof *s->cb);
    s->cb->next_span_id = 1;
    s->cb->next_refused_id = 1;
    ST_RLX(&s->rg->write_idx, 0);
    ST_RLX(&s->rg->read_idx, 0);
    s->state = SLOT_LIVE;
    p_unlock(p, &p->thread_mutex);

    t->slot1 = i + 1;
    t->generation = s->generation;
    t->incarnation = inc;
    t->acked = 0;
    t->retired = 0;
    t->depth = 0;
    t->refused_top = 0;
    return PS3DK_PROF_OK;
}

int ps3dk_prof_thread_begin(const char *name)
{
    prof *p = ready();
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    return thread_begin_impl(p, name);
}

/* Q10: opt-in, and never for a thread whose TLS is acked or retired. */
static int auto_register(prof *p)
{
    tls_state *t = &g_tls;
    if (!p->cfg.auto_register || t->acked || t->retired)
        return -1;
    return thread_begin_impl(p, "auto") == PS3DK_PROF_OK ? 0 : -1;
}

/* The STOP batch and the ack (5.1, 7).  The caller has checked: an incarnation, not acked, not retired, latch set. */
void p_observe_stop(prof *p)
{
    tls_state *t = &g_tls;
    slot *s = &p->slots[t->slot1 - 1];
    cblock *cb = s->cb;
    ring *rg = s->rg;
    uint32_t k = t->depth, i, d;
    uint64_t seq, ws, we, tb;

    cb->open_spans = k;   /* 1. the snapshot: the k spans the batch covers */
    if (k > 0) {
        if (cb->limit || cb->next_seq > P_CEIL - k) {
            /* Not admitted: refused WHOLE.  BC: refused_count += k only if it stays within 2^63. */
            STEP(PS3DK_PROF_TEST_STEP_REFUSE, k);
            if (cb->refused <= P_CEIL - k)
                cb->refused += k;
            else
                cb->exhausted = 1;
            cb->limit = 1;
        } else {
            /* Admitted WHOLE: all k sequences at once, then one publication of k (+ GAP) or all k dropped. */
            tb = p_tb();
            seq = seq_assign(cb, k);
            if (reserve(cb, rg, k, seq, &ws, &we)) {
                for (i = 0; i < k; i++) {
                    d = k - 1 - i;   /* innermost first */
                    put_rec(rslot(rg, we + i), K_END, (uint8_t)d, 1, t->stack[d].desc, seq + i, tb,
                            t->stack[d].span_id);
                }
                commit(rg, ws, we + k);
            }
        }
    }
    t->depth = 0;         /* 3. clear the stack */
    t->refused_top = 0;
    t->acked = 1;         /* 4. the thread-local flag */
    HOOK(before_ack_store);
    ST_REL(&cb->ack_retire, (uint32_t)ST_ACKED);   /* 5. the LAST write */
}

void ps3dk_prof_thread_end(void)
{
    tls_state *t = &g_tls;
    prof *p;
    slot *s;
    cblock *cb;

    if (t->acked || t->retired || !t->slot1)
        return;   /* after the ack: refused silently, nothing changes */
    p = ready();
    if (!p)
        return;
    if (LD_ACQ(&p->stop_latch)) {
        p_observe_stop(p);   /* thread_end as the first call to observe STOP: it ends ACKED */
        return;
    }
    s = &p->slots[t->slot1 - 1];
    cb = s->cb;
    HOOK(before_live_to_retiring);
    if (p_lock(p, &p->thread_mutex) == 0) {   /* allowed after the latch too (E1) */
        s->state = SLOT_RETIRING;
        p_unlock(p, &p->thread_mutex);
    }
    cb->open_spans = t->depth;   /* 1. the snapshot; no END is synthesised */
    t->depth = 0;                /* 3. clear the stack */
    t->refused_top = 0;
    t->retired = 1;              /* 4. */
    HOOK(before_retire_store);
    ST_REL(&cb->ack_retire, (uint32_t)ST_RETIRED);   /* 5. the LAST write */
}

/* ---- zones (5.1, 5.2, r2 3.2, E2, F1) ----------------------------------------------------------------------------- */

ps3dk_prof_zone ps3dk_prof_zone_begin(ps3dk_prof_desc *desc, ps3dk_prof_token token)
{
    ps3dk_prof_zone h;
    tls_state *t = &g_tls;
    prof *p;
    slot *s;
    cblock *cb;
    ring *rg;
    uint32_t depth, di;
    uint64_t top, id, seq, ws, we, tb;

    memset(&h, 0, sizeof h);   /* INERT */
    p = ready();
    if (!p || !desc || t->in_drain || t->acked || t->retired)
        return h;              /* terminal TLS touches nothing (E2 step 2); no self-instrumentation in a drain */
    if (!t->slot1 && auto_register(p) != 0)
        return h;
    if (LD_ACQ(&p->stop_latch)) {
        p_observe_stop(p);
        return h;
    }
    s = &p->slots[t->slot1 - 1];
    cb = s->cb;
    rg = s->rg;
    depth = t->depth;
    top = depth ? t->stack[depth - 1].span_id : 0;
    h.incarnation = t->incarnation;
    h.generation = t->generation;
    h.prev_refused_top = t->refused_top;
    h.span_top_at_begin = top;

    if (!desc_index(p, cb, rg, desc, &di))
        goto refused;          /* 5.3: a failed descriptor refuses every zone of its site */
    if (depth >= P_MAX_DEPTH)
        goto refused;          /* 5.2 */
    if (cb->next_span_id >= P_CEIL) {
        exhaust(cb);           /* BC; unreachable below the sequence limit (Q6: at most 2^62 span ids) */
        goto refused;
    }
    if (!seq_admit(cb, 2))
        goto refused;          /* 5.1: refused WHOLE at the boundary */
    id = cb->next_span_id++;   /* every non-refused BEGIN takes an id, dropped ones included (Q6) */
    tb = p_tb();
    seq = seq_assign(cb, 2);
    if (reserve(cb, rg, 2, seq, &ws, &we)) {
        put_rec(rslot(rg, we), K_BEGIN, (uint8_t)depth, 0, di, seq, tb, id);
        put_rec(rslot(rg, we + 1), K_BEGIN_PARENT, (uint8_t)depth, token.owner ? 1 : 0, token.owner, seq + 1, top,
                token.owner ? token.frame : 0);
        commit(rg, ws, we + 2);
    }
    t->stack[depth].span_id = id;   /* 5.1: a dropped BEGIN is still pushed */
    t->stack[depth].desc = di;
    t->depth = depth + 1;
    h.kind = ZK_PUSHED;
    h.id = id;
    return h;

refused:
    count_refused(cb, 1);
    if (cb->next_refused_id >= P_CEIL) {
        /* F1 + BC: a refused-begin id past 2^63 is never issued.  The incarnation fails closed and the handle is
         * INERT (it takes no part in the LIFO check).  Reachable only with refused_count at its ceiling. */
        exhaust(cb);
        memset(&h, 0, sizeof h);
        return h;
    }
    h.kind = ZK_REFUSED;
    h.id = cb->next_refused_id++;
    t->refused_top = h.id;
    return h;
}

void ps3dk_prof_zone_end(ps3dk_prof_zone *h)
{
    tls_state *t = &g_tls;
    prof *p;
    slot *s;
    cblock *cb;
    uint32_t kind, depth;
    uint64_t top;

    if (!h || (h->kind != ZK_PUSHED && h->kind != ZK_REFUSED))
        return;                /* 1. SPENT or INERT (or not a handle): no slot access, no counter */
    kind = h->kind;
    h->kind = ZK_SPENT;        /* every path below leaves the handle SPENT: one-shot per object */
    if (t->acked || t->retired)
        return;                /* 2. */
    p = ready();
    if (!p || !t->slot1)
        return;                /* 3. */
    if (LD_ACQ(&p->stop_latch)) {
        p_observe_stop(p);     /* 4. the batch covers this span; no END handling, no mismatch counting */
        return;
    }
    if (h->incarnation != t->incarnation || h->generation != t->generation)
        return;                /* 5. stale or foreign: no pop, no counter */
    s = &p->slots[t->slot1 - 1];
    cb = s->cb;
    depth = t->depth;
    top = depth ? t->stack[depth - 1].span_id : 0;
    if (kind == ZK_REFUSED) {
        if (t->refused_top == h->id && top == h->span_top_at_begin)
            t->refused_top = h->prev_refused_top;
        else
            count_refused(cb, 1);   /* F1: a nesting misuse of this incarnation */
        return;
    }
    if (depth && top == h->id && t->refused_top == h->prev_refused_top) {
        (void)emit1(cb, s->rg, K_END, (uint8_t)(depth - 1), 0, t->stack[depth - 1].desc, p_tb(), h->id);
        t->depth = depth - 1;  /* the stack follows the calls; a refused END still closes its span */
        return;
    }
    count_refused(cb, 1);      /* F1: misuse */
}

/* ---- frames (5.6) -------------------------------------------------------------------------------------------------- */

void ps3dk_prof_frame(void)
{
    tls_state *t = &g_tls;
    prof *p = ready();
    slot *s;
    cblock *cb;
    ring *rg;
    uint64_t fid, seq, ws, we, tb;

    if (!p || t->in_drain || t->acked || t->retired)
        return;
    if (!t->slot1 && auto_register(p) != 0)
        return;
    if (LD_ACQ(&p->stop_latch)) {
        p_observe_stop(p);
        return;
    }
    s = &p->slots[t->slot1 - 1];
    cb = s->cb;
    rg = s->rg;
    if (!seq_admit(cb, 1)) {
        count_refused(cb, 1);  /* a refused FRAME assigns no id and leaves current_frame unchanged */
        return;
    }
    if (cb->next_frame_id >= P_CEIL) {
        exhaust(cb);           /* BC; unreachable: a frame id is at most its own seq (4) */
        count_refused(cb, 1);
        return;
    }
    fid = cb->next_frame_id++;
    tb = p_tb();
    seq = seq_assign(cb, 1);
    if (reserve(cb, rg, 1, seq, &ws, &we)) {
        put_rec(rslot(rg, we), K_FRAME, 0, 0, 0, seq, tb, fid);
        commit(rg, ws, we + 1);
    }
    ST_REL(&cb->current_frame, fid + 1);   /* after the publish or the drop */
}

ps3dk_prof_token ps3dk_prof_frame_token(void)
{
    ps3dk_prof_token tok;
    tls_state *t = &g_tls;
    prof *p = ready();
    uint64_t cf;

    memset(&tok, 0, sizeof tok);
    if (!p || t->in_drain || t->acked || t->retired || !t->slot1)
        return tok;
    if (LD_ACQ(&p->stop_latch)) {
        p_observe_stop(p);
        return tok;
    }
    cf = LD_ACQ(&p->slots[t->slot1 - 1].cb->current_frame);
    if (cf) {
        tok.owner = t->incarnation;
        tok.frame = cf - 1;
    }
    return tok;
}

/* ---- setters and init (E3, F3, r2 3.3) ---------------------------------------------------------------------------- */

static int gate_enter(uint32_t to)
{
    uint32_t g = G_UNINIT;
    if (CAS_ACQREL(&g_gate, &g, to))
        return PS3DK_PROF_OK;
    return (g == G_READY || g == G_TERMINAL) ? PS3DK_PROF_EALREADY : PS3DK_PROF_EBUSY;
}

int ps3dk_prof_set_sink_path(const char *dir)
{
    int rc = gate_enter(G_CONFIGURING);
    size_t n;
    if (rc != PS3DK_PROF_OK)
        return rc;
    n = dir ? bounded_len(dir, 256) : 0;
    if (n == 0 || n >= 256) {
        rc = PS3DK_PROF_EINVAL;
    } else {
        memcpy(g_pending.dir, dir, n);
        g_pending.dir[n] = 0;
        g_pending.kind = 1;
    }
    ST_REL(&g_gate, (uint32_t)G_UNINIT);
    return rc;
}

int ps3dk_prof_set_sink_callbacks(const ps3dk_prof_sink_callbacks *cb)
{
    int rc = gate_enter(G_CONFIGURING);
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (!cb || !cb->write || !cb->close) {
        rc = PS3DK_PROF_EINVAL;
    } else {
        g_pending.cb = *cb;
        g_pending.kind = 2;
    }
    ST_REL(&g_gate, (uint32_t)G_UNINIT);
    return rc;
}

void ps3dk_prof_config_defaults(ps3dk_prof_config *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof *cfg);
    cfg->mode = PS3DK_PROF_MODE_APP;
    cfg->max_threads = 64;
    cfg->max_descriptors = 1024;
    cfg->ring_records = 4096;
    cfg->drain_interval_us = 10000;    /* Q8 defaults, provisional */
    cfg->drain_stack_bytes = 65536;
    cfg->quiesce_timeout_us = 1000000;
    cfg->auto_register = 0;            /* Q10: off */
    cfg->app = "app";
}

static int app_valid(const char *a, size_t *len)
{
    size_t i, n;
    if (!a)
        return 0;
    n = bounded_len(a, 33);
    if (n == 0 || n > 32)
        return 0;
    for (i = 0; i < n; i++) {
        char c = a[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    *len = n;
    return 1;
}

/* A HEADER string: NULL is empty; more than 512 bytes, or (utf8) invalid UTF-8, is EINVAL. */
static int header_string(char *dst, uint32_t *len, const char *src, int utf8)
{
    size_t n = src ? bounded_len(src, P_STR_MAX + 1) : 0;
    if (n > P_STR_MAX || (utf8 && !utf8_valid(src, n)))
        return 0;
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
    *len = (uint32_t)n;
    return 1;
}

static int is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

/* Overflow-checked layout: reserve count x elem bytes, rounded up to 128, at *off. */
static int take(size_t *off, size_t count, size_t elem, size_t *at)
{
    size_t bytes;
    if (elem && count > (size_t)-1 / elem)
        return -1;
    bytes = count * elem;
    if (bytes > (size_t)-1 - 127)
        return -1;
    bytes = (bytes + 127) & ~(size_t)127;
    if (bytes > (size_t)-1 - *off)
        return -1;
    *at = *off;
    *off += bytes;
    return 0;
}

#define P_TOTAL_MAX ((size_t)256 * 1024 * 1024)

static int init_impl(const ps3dk_prof_config *cfg_in, int *terminal)
{
    prof *p = &g_storage;
    ps3dk_prof_config c;
    size_t off = 0, o_slots, o_cb, o_rg, o_data, o_pub, o_desc, o_buf, o_decl, o_wr, o_ret, o_snap, o_front, o_ts;
    size_t cbsz = (sizeof(cblock) + 127) & ~(size_t)127, rgsz = (sizeof(ring) + 127) & ~(size_t)127, app_len = 0;
    uint32_t i;
    uint64_t now;
    int rc, err = 0, n;
    const char *dir;

    if (!cfg_in)
        return PS3DK_PROF_EINVAL;
    c = *cfg_in;
    if ((c.mode != PS3DK_PROF_MODE_APP && c.mode != PS3DK_PROF_MODE_THREAD) || c.max_threads < 1 ||
        c.max_threads > 1024 || c.max_descriptors < 1 || c.max_descriptors > 65536 || !is_pow2(c.ring_records) ||
        c.ring_records < 512 || c.ring_records > 1048576 || c.drain_interval_us < 1000 ||
        c.drain_interval_us > 1000000 || c.drain_stack_bytes < 16384 || c.drain_stack_bytes > 1048576 ||
        c.quiesce_timeout_us > 60000000 || !app_valid(c.app, &app_len))
        return PS3DK_PROF_EINVAL;

    memset(p, 0, sizeof *p);
    memcpy(p->app, c.app, app_len);
    p->app[app_len] = 0;
    p->app_len = (uint32_t)app_len;
    if (!header_string(p->build_id, &p->build_id_len, c.build_id, 0) ||
        !header_string(p->flags, &p->flags_len, c.flags, 1) ||
        !header_string(p->workload_tag, &p->workload_tag_len, c.workload_tag, 1))
        return PS3DK_PROF_EINVAL;
    c.app = p->app;
    c.build_id = p->build_id;
    c.flags = p->flags;
    c.workload_tag = p->workload_tag;
    p->cfg = c;
    p->nslots = c.max_threads;

    /* r2 3.3 steps 2-3: every size checked in size_t (32-bit on ILP32), one block. */
    if (take(&off, c.max_threads, sizeof(slot), &o_slots) || take(&off, c.max_threads, cbsz, &o_cb) ||
        take(&off, c.max_threads, rgsz, &o_rg) || take(&off, c.max_threads, (size_t)c.ring_records * P_REC, &o_data) ||
        take(&off, c.max_threads, (size_t)c.ring_records * sizeof(uint16_t), &o_pub) ||
        take(&off, c.max_descriptors, sizeof(desc_entry), &o_desc) ||
        take(&off, 1, 16 + (size_t)P_CHUNK_CAP + 4, &o_buf) || take(&off, c.max_threads, sizeof(uint32_t), &o_decl) ||
        take(&off, c.max_threads, sizeof(uint64_t), &o_wr) || take(&off, c.max_threads, sizeof(uint32_t), &o_ret) ||
        take(&off, c.max_threads, sizeof(snap), &o_snap) || take(&off, c.max_threads, sizeof(fentry), &o_front) ||
        take(&off, c.max_threads, sizeof(tdecl), &o_ts) || off > P_TOTAL_MAX)
        return PS3DK_PROF_EINVAL;
    p->block = (uint8_t *)pal_alloc(off);
    if (!p->block)
        return PS3DK_PROF_ENOMEM;
    p->slots = (slot *)(p->block + o_slots);
    p->descs = (desc_entry *)(p->block + o_desc);
    p->dr.buf = p->block + o_buf;
    p->dr.decl_gen = (uint32_t *)(p->block + o_decl);
    p->dr.written = (uint64_t *)(p->block + o_wr);
    p->dr.retired_gen = (uint32_t *)(p->block + o_ret);
    p->dr.snaps = (snap *)(p->block + o_snap);
    p->dr.front = (fentry *)(p->block + o_front);
    p->dr.tscratch = (tdecl *)(p->block + o_ts);
    memset(p->slots, 0, sizeof(slot) * c.max_threads);
    memset(p->dr.decl_gen, 0, sizeof(uint32_t) * c.max_threads);
    memset(p->dr.written, 0, sizeof(uint64_t) * c.max_threads);
    memset(p->dr.retired_gen, 0, sizeof(uint32_t) * c.max_threads);
    for (i = 0; i < c.max_threads; i++) {
        slot *s = &p->slots[i];
        s->cb = (cblock *)(p->block + o_cb + (size_t)i * cbsz);
        s->rg = (ring *)(p->block + o_rg + (size_t)i * rgsz);
        memset(s->cb, 0, sizeof(cblock));
        memset(s->rg, 0, sizeof(ring));
        s->rg->cap = c.ring_records;
        s->rg->mask = c.ring_records - 1;
        s->rg->slots = p->block + o_data + (size_t)i * c.ring_records * P_REC;
        s->rg->pub_len = (uint16_t *)(p->block + o_pub + (size_t)i * (((size_t)c.ring_records * 2 + 127) & ~(size_t)127));
    }
    if (pal_mutex_init(&p->thread_mutex) != 0) {
        rc = PS3DK_PROF_ENOMEM;
        goto fail_block;
    }
    if (pal_mutex_init(&p->desc_mutex) != 0) {
        rc = PS3DK_PROF_ENOMEM;
        goto fail_mutex1;
    }
    p_crc32_init();
    p->fnv_build = p_fnv(p_fnv(P_FNV_BASIS, p->build_id, p->build_id_len), "", 1);

    /* Step 5 (F3): clocks are ECLOCK, the APP owner identity is ETHREAD; all retryable. */
    p->tb_freq = p_tb_freq();
    if (p->tb_freq == 0 || p_mono_now(p, &now) != 0) {
        rc = PS3DK_PROF_ECLOCK;
        goto fail_mutex2;
    }
    if (c.mode == PS3DK_PROF_MODE_APP && pal_owner_get(&p->owner) != 0) {
        rc = PS3DK_PROF_ETHREAD;
        goto fail_mutex2;
    }
    p->tb_at_init = p_tb();
    p->next_incarnation = 1;

    /* Step 6: the sink. */
    if (g_pending.kind == 2) {
        p->snk.kind = 2;
        p->snk.cb = g_pending.cb;   /* 6b: no callback runs during init */
    } else {
        dir = g_pending.kind == 1 ? g_pending.dir : PAL_DEFAULT_SINK_DIR;
        n = snprintf(p->snk.path, sizeof p->snk.path, "%s/%s-%016llx.prof", dir, p->app,
                     (unsigned long long)p->tb_at_init);
        if (n < 0 || (size_t)n >= sizeof p->snk.path) {
            rc = PS3DK_PROF_EINVAL;   /* Q7: a truncated name is never used */
            goto fail_mutex2;
        }
        if (pal_open_excl(p->snk.path, &p->snk.fd, &err) != 0) {
            rc = err == PAL_EEXIST ? PS3DK_PROF_EEXIST : PS3DK_PROF_ESINKFAIL;   /* no file created: retryable */
            goto fail_mutex2;
        }
        p->snk.kind = 1;
    }

    /* Step 7: THREAD mode's drain thread waits on the start gate until the state is published. */
    if (c.mode == PS3DK_PROF_MODE_THREAD) {
        if (pal_thread_create(&p->drain_thread, p_drain_main, p, c.drain_stack_bytes) != 0) {
            rc = PS3DK_PROF_ETHREAD;
            if (p->snk.kind == 1) {
                (void)pal_close(p->snk.fd, &err);   /* the file exists and is kept: TERMINAL */
                *terminal = 1;
            }
            goto fail_mutex2;
        }
        p->drain_thread_started = 1;
    }
    return PS3DK_PROF_OK;

fail_mutex2:
    (void)pal_mutex_destroy(&p->desc_mutex);
fail_mutex1:
    (void)pal_mutex_destroy(&p->thread_mutex);
fail_block:
    pal_free(p->block);
    memset(p, 0, sizeof *p);
    return rc;
}

int ps3dk_prof_init(const ps3dk_prof_config *cfg)
{
    int terminal = 0, rc = gate_enter(G_INITIALIZING);
    if (rc != PS3DK_PROF_OK)
        return rc;   /* EBUSY while a setter or init runs; EALREADY after READY or TERMINAL.  Never waits. */
    rc = init_impl(cfg, &terminal);
    if (rc != PS3DK_PROF_OK) {
        ST_REL(&g_gate, (uint32_t)(terminal ? G_TERMINAL : G_UNINIT));
        return rc;
    }
    g_prof = &g_storage;
    ST_REL(&g_gate, (uint32_t)G_READY);       /* step 8: publish, */
    ST_REL(&g_storage.start_gate, 1u);        /* then open the start gate */
    return PS3DK_PROF_OK;
}

/* ---- test hooks ---------------------------------------------------------------------------------------------------- */

#ifdef PS3DK_PROF_TEST_HOOKS
static cblock *self_cb(void)
{
    prof *p = ready();
    if (!p || !g_tls.slot1)
        return NULL;
    return p->slots[g_tls.slot1 - 1].cb;
}

/* Presets for the calling thread's live incarnation.  NEXT_SEQ moves the sequence forward AS IF the skipped
 * sequences had been dropped, so the capture stays consistent (a GAP covers them). */
void ps3dk_prof_test_preset(int field, uint64_t v)
{
    cblock *cb = self_cb();
    if (!cb)
        return;
    switch (field) {
    case PS3DK_PROF_TEST_NEXT_SEQ:
        if (v > cb->next_seq) {
            if (!cb->pend) {
                cb->pend = 1;
                cb->pend_first = cb->next_seq;
            }
            cb->pend_last = v - 1;
            cb->dropped += v - cb->next_seq;
            cb->next_seq = v;
        }
        break;
    case PS3DK_PROF_TEST_REFUSED: cb->refused = v; break;
    case PS3DK_PROF_TEST_REGERR: cb->regerr = v; break;
    case PS3DK_PROF_TEST_NEXT_REFUSED_ID: cb->next_refused_id = v; break;
    case PS3DK_PROF_TEST_NEXT_SPAN_ID: cb->next_span_id = v; break;
    case PS3DK_PROF_TEST_LIMIT: cb->limit = (uint32_t)v; break;
    default: break;
    }
}

uint64_t ps3dk_prof_test_read(int field)
{
    tls_state *t = &g_tls;
    prof *p = ready();
    cblock *cb = (p && t->slot1) ? p->slots[t->slot1 - 1].cb : NULL;
    switch (field) {
    case PS3DK_PROF_TEST_DEPTH: return t->depth;
    case PS3DK_PROF_TEST_REFUSED_TOP: return t->refused_top;
    case PS3DK_PROF_TEST_INCARNATION: return t->incarnation;
    case PS3DK_PROF_TEST_GENERATION: return t->generation;
    case PS3DK_PROF_TEST_SLOT: return t->slot1;
    default: break;
    }
    if (!cb || t->retired)
        return P_UNKNOWN;   /* a retired thread never reads its old slot (7) */
    switch (field) {
    case PS3DK_PROF_TEST_NEXT_SEQ: return cb->next_seq;
    case PS3DK_PROF_TEST_REFUSED: return cb->refused;
    case PS3DK_PROF_TEST_REGERR: return cb->regerr;
    case PS3DK_PROF_TEST_NEXT_REFUSED_ID: return cb->next_refused_id;
    case PS3DK_PROF_TEST_NEXT_SPAN_ID: return cb->next_span_id;
    case PS3DK_PROF_TEST_DROPPED: return cb->dropped;
    case PS3DK_PROF_TEST_LIMIT: return cb->limit;
    case PS3DK_PROF_TEST_OPEN_SPANS: return cb->open_spans;
    case PS3DK_PROF_TEST_EXHAUSTED: return cb->exhausted;
    default: return P_UNKNOWN;
    }
}

uint64_t ps3dk_prof_test_stat(int which)
{
    prof *p = ready();
    uint64_t v;
    if (which == PS3DK_PROF_TEST_STAT_GATE)
        return LD_ACQ(&g_gate);
    if (which >= PS3DK_PROF_TEST_STAT_SLOT_STATE_BASE) {
        uint32_t i = (uint32_t)(which - PS3DK_PROF_TEST_STAT_SLOT_STATE_BASE);
        if (!p || i >= p->nslots || p_lock(p, &p->thread_mutex) != 0)
            return P_UNKNOWN;
        v = p->slots[i].state;
        p_unlock(p, &p->thread_mutex);
        return v;
    }
    if (which == PS3DK_PROF_TEST_STAT_FROZEN)
        return p ? (uint64_t)LD_ACQ(&p->dr.frozen) : 0;
    if (which == PS3DK_PROF_TEST_STAT_LATCH)
        return p ? (uint64_t)LD_ACQ(&p->stop_latch) : 0;
    if (which < 0 || which >= 128)
        return P_UNKNOWN;
    return STAT_GET(which);
}
#endif
