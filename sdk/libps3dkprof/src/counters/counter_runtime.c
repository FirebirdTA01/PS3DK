/* Named per-frame counters: the producer call, the drain snapshot and the COUNTERS chunk (counter_internal.h). */
#include "counter_internal.h"

#include <string.h>

int p_counter_init(prof *p)
{
    counter_capture *c = pal_alloc(sizeof *c);
    if (!c)
        return PS3DK_PROF_ENOMEM;
    memset(c, 0, sizeof *c);
    if (pal_mutex_init(&c->mu) != 0) {
        pal_free(c);
        return PS3DK_PROF_ENOMEM;
    }
    p->counters = c;
    return PS3DK_PROF_OK;
}

void p_counter_unwind(prof *p)
{
    if (p->counters) {
        (void)pal_mutex_destroy(&p->counters->mu);
        pal_free(p->counters);
        p->counters = NULL;
    }
}

static int name_ok(const char *n, size_t *len)
{
    size_t i = 0;
    if (!n)
        return 0;
    while (i <= CNT_NAME_LEN && n[i]) {
        if ((unsigned char)n[i] < 0x21 || (unsigned char)n[i] > 0x7e)
            return 0;
        i++;
    }
    if (i == 0 || i > CNT_NAME_LEN)
        return 0;
    *len = i;
    return 1;
}

/* The drainer's lock sites (the test hook counter_lock_fail_site). */
enum { CNT_LOCK_SNAPSHOT = 1, CNT_LOCK_FREEZE = 2, CNT_LOCK_COPY = 3, CNT_LOCK_COMMIT = 4 };

/* The drainer's lock of the counter mutex.  A failure is terminal: the capture fails closed (lock_failed, which
 * also fails the sealing) and the table emits nothing more (failed), so no chunk is retried. */
static int cnt_lock(prof *p, counter_capture *c, uint32_t site)
{
    int rc = 0;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.counter_lock_fail_site == site) {
        if (ps3dk_prof_test_hooks.counter_lock_fail_skip == 0) {
            ps3dk_prof_test_hooks.counter_lock_fail_site = 0;
            rc = -1;
        } else {
            ps3dk_prof_test_hooks.counter_lock_fail_skip--;
        }
    }
#else
    (void)site;
#endif
    if (rc == 0 && pal_mutex_lock(&c->mu) == 0)
        return 0;
    c->failed = 1;
    ST_RLX(&p->lock_failed, 1);
    return -1;
}

static void sat_inc(uint64_t *v)
{
    if (*v != UINT64_MAX)
        (*v)++;
}

int ps3dk_prof_counter(const char *name, int64_t value)
{
    prof *p;
    counter_capture *c;
    size_t len = 0;
    uint32_t id;
    int rc = PS3DK_PROF_OK;

    if (!p_ready())
        return PS3DK_PROF_ENOTINIT;
    p = g_prof;
    c = p->counters;
    if (!c)
        return PS3DK_PROF_ENOTSUP;
    if (LD_ACQ(&p->stop_latch))
        return PS3DK_PROF_ESTOPPED;
    if (pal_mutex_lock(&c->mu) != 0)
        return PS3DK_PROF_EBUSY;
    if (c->frozen) {
        rc = PS3DK_PROF_ESTOPPED;
        goto out;
    }
    if (g_tls.in_drain) { /* a sink callback or the drainer itself */
        sat_inc(&c->refused);
        rc = PS3DK_PROF_EBUSY;
        goto out;
    }
    if (!g_tls.slot1 || g_tls.retired || g_tls.acked) {
        sat_inc(&c->refused);
        rc = PS3DK_PROF_EPERM;
        goto out;
    }
    if (!name_ok(name, &len)) {
        sat_inc(&c->refused);
        rc = PS3DK_PROF_EINVAL;
        goto out;
    }
    for (id = 0; id < c->names; id++)
        if (c->len[id] == len && memcmp(c->name[id], name, len) == 0)
            break;
    if (id == c->names) {
        if (c->names == CNT_NAMES) {
            sat_inc(&c->refused);
            rc = PS3DK_PROF_EFULL;
            goto out;
        }
        memcpy(c->name[id], name, len);
        c->len[id] = (uint16_t)len;
        c->names++;
    }
    if (c->head - c->tail >= CNT_RING) {
        sat_inc(&c->dropped);
        rc = PS3DK_PROF_EFULL;
        goto out;
    }
    {
        uint8_t *s = c->ring[c->head & (CNT_RING - 1)];
        uint64_t frame = LD_ACQ(&p->slots[g_tls.slot1 - 1].cb->current_frame);
        p_le32(s, g_tls.incarnation);
        p_le32(s + 4, id);
        p_le64(s + 8, frame);
        p_le64(s + 16, p_tb());
        p_le64(s + 24, (uint64_t)value);
        c->head++;
    }
out:
    (void)pal_mutex_unlock(&c->mu);
    return rc;
}

int p_counter_snapshot(prof *p)
{
    counter_capture *c = p->counters;
    if (!c || c->frozen)
        return 0;
    if (c->failed || cnt_lock(p, c, CNT_LOCK_SNAPSHOT) != 0)
        return -1;
    c->snap_head = c->head;
    c->snap_names = c->names;
    (void)pal_mutex_unlock(&c->mu);
    return 0;
}

int p_counter_freeze(prof *p)
{
    counter_capture *c = p->counters;
    if (!c || c->frozen)
        return 0;
    if (c->failed || cnt_lock(p, c, CNT_LOCK_FREEZE) != 0)
        return -1; /* not frozen: p_counter_emit refuses the FINAL, never writes one from an older snapshot */
    c->snap_head = c->head;
    c->snap_names = c->names;
    c->frozen = 1;
    (void)pal_mutex_unlock(&c->mu);
    return 0;
}

int p_counter_emit(prof *p, int final)
{
    counter_capture *c = p->counters;
    uint8_t *b, *w;
    uint32_t nn, ns;
    uint64_t dropped, refused;

    if (!c || c->final_done || c->failed || p->snk.failed || (final && !c->frozen))
        return -1;
    nn = c->snap_names - c->names_emitted;
    ns = (uint32_t)(c->snap_head - c->tail);
    if (!final && nn == 0 && ns == 0)
        return 0;
    if (cnt_lock(p, c, CNT_LOCK_COPY) != 0)
        return -1;
    dropped = c->dropped;
    refused = c->refused;
    b = p->dr.buf + 16; /* the chunk payload area of the drainer's buffer */
    p_le16(b, 1);
    p_le16(b + 2, final ? 1u : 0u);
    p_le32(b + 4, nn);
    p_le32(b + 8, ns);
    p_le32(b + 12, 0);
    p_le64(b + 16, dropped);
    p_le64(b + 24, refused);
    w = b + CNT_HEADER;
    for (uint32_t i = c->names_emitted; i < c->snap_names; i++) {
        p_le32(w, i);
        p_le16(w + 4, c->len[i]);
        memcpy(w + 6, c->name[i], c->len[i]);
        w += 6 + c->len[i];
    }
    for (uint64_t i = c->tail; i < c->snap_head; i++) {
        memcpy(w, c->ring[i & (CNT_RING - 1)], CNT_SAMPLE);
        w += CNT_SAMPLE;
    }
    (void)pal_mutex_unlock(&c->mu);
    if (p_counter_emit_chunk(p, b, (size_t)(w - b)))
        return -1;
    /* The sink accepted the chunk: its names and the FINAL are done even if the commit lock fails below. */
    c->names_emitted = c->snap_names;
    if (final)
        c->final_done = 1;
    if (cnt_lock(p, c, CNT_LOCK_COMMIT) != 0)
        return -1; /* the samples stay unreleased, and failed forbids emitting them again */
    c->tail = c->snap_head; /* only now may producers reuse the slots */
    (void)pal_mutex_unlock(&c->mu);
    return 0;
}
