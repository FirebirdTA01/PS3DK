/* GPU (RSX) timestamp domains inside the runtime (P3.3 R2, host PAL only): the port adapters over the R1 core
 * (prg.c), the table-slot pin protocol, owner entries and their declaration evidence, the drain/seal steps, and the
 * API of prof_gpu.h.  Design: p3.3-r2 935936fb + deltas 2400e50a, 0e23d13e and the codex 12:53 rulings.
 *
 * Locks: the EXTENSION mutex (spu_mutex) serializes every drain/finalization, attach, release, the stats copy and the
 * GPU ledger; it is held across sink callbacks (the drainer emits under it).  Each table slot's snap mutex is taken
 * only for the core's snapshot copies, never across a sink or port callback.  thread_mutex is taken alone (gpu_frame's
 * validate-and-install, and the drainer's free scan that calls p_gpu_slot_freed); it is never nested with the other
 * two.  Hot calls (begin/end/mark/frame/calibrate) never take the extension mutex: their domain lifetime is proven by
 * the PIN (gpu_internal.h).
 *
 * Faults: after ext_fault no GPU call takes the extension mutex again (ESTOPPED); a domain whose finalization never
 * ran keeps its ctx, reports and label BORROWED until process end, and a faulted table slot is never reused. */
#include "prof_internal.h"

#ifdef PS3DK_PROF_WITH_SPU

#ifndef PS3DK_PROF_PAL_HOST
#error "the GPU domain extension is host-PAL only in this checkpoint"
#endif

#include <string.h>

#include "gpu_internal.h"
#include "prof_gpu.h"
#ifdef PS3DK_PROF_TEST_HOOKS
#include "prof_test_hooks.h"
#include "psp_ppu.h" /* the limits accessor */
#endif

/* the report area and the user label range (named here; pinned against our GCM layer in R3) */
#define GPU_REPORTS 2048u
#define GPU_LABEL_FIRST 64u
#define GPU_LABEL_LAST 255u
/* R4a: the runtime's ranges are the GCM port's (one set of numbers) */
_Static_assert(GPU_REPORTS == GPP_REPORTS, "report area");
_Static_assert(GPU_LABEL_FIRST == GPP_LABEL_USER_FIRST && GPU_LABEL_LAST == GPP_LABEL_LAST, "label range");
_Static_assert(PRG_SLOTS_MAX <= GPP_CACHE_MAX, "gpu_report_slots_max fits the port's address cache");

#define LD_SC_(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define ST_SC_(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define CAS_SC_(p, e, v) __atomic_compare_exchange_n((p), (e), (v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

/* ---- the port: the device ops forward to the emulator; the locks and the hold are the runtime's ---- */

static int gp_reserve(void *c, uint32_t r, uint32_t l, uint32_t v)
{
    const gpu_slot *g = c;
    return g->dev->reserve_pair(g->dev->ctx, r, l, v);
}
static void gp_flush(void *c)
{
    const gpu_slot *g = c;
    g->dev->flush(g->dev->ctx);
}
static uint32_t gp_rlabel(void *c, uint32_t l)
{
    const gpu_slot *g = c;
    return g->dev->read_label(g->dev->ctx, l);
}
static void gp_wlabel(void *c, uint32_t l, uint32_t v)
{
    const gpu_slot *g = c;
    g->dev->write_label(g->dev->ctx, l, v);
}
static uint64_t gp_rtimer(void *c, uint32_t r)
{
    const gpu_slot *g = c;
    return g->dev->read_timer(g->dev->ctx, r);
}
static void gp_wtimer(void *c, uint32_t r, uint64_t v)
{
    const gpu_slot *g = c;
    g->dev->write_timer(g->dev->ctx, r, v);
}
static void gp_sync(void *c)
{
    const gpu_slot *g = c;
    g->dev->sync(g->dev->ctx);
}
static uint64_t gp_mftb(void *c)
{
    (void)c;
    return p_tb(); /* the file's PPU timebase (CLOCKS), not the device's */
}
static int gp_now(void *c, uint64_t *us)
{
    (void)c;
    return pal_mono_now_us(us) == 0 ? 0 : -1;
}

static int gp_slock(void *c)
{
    gpu_slot *g = c;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.gpu_snap_nolock)
        return 0;
    if (ps3dk_prof_test_hooks.gpu_snap_lock_fail_at &&
        ++ps3dk_prof_test_hooks.gpu_snap_ops == ps3dk_prof_test_hooks.gpu_snap_lock_fail_at)
        return -1;
#endif
    return pal_mutex_lock(&g->snap) == 0 ? 0 : -1;
}
static int gp_sunlock(void *c)
{
    gpu_slot *g = c;
    int rc;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.gpu_snap_nolock)
        return 0;
#endif
    rc = pal_mutex_unlock(&g->snap);
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at &&
        ++ps3dk_prof_test_hooks.gpu_snap_ops == ps3dk_prof_test_hooks.gpu_snap_unlock_fail_at)
        rc = -1; /* the seam: really unlocked, then the error */
#endif
    return rc == 0 ? 0 : -1;
}

/* the extension mutex through the port: only prg_detach's finalization uses it (drains and the seal run held) */
static int gp_elock(void *c)
{
    prof *p = ((gpu_slot *)c)->p;
    if (LD_ACQ(&p->ext_fault))
        return -1;
    if (p_ext_lock_at(p) != 0) {
        ST_REL(&p->ext_fault, 1u);
        ST_RLX(&p->lock_failed, 1);
        return -1;
    }
    if (LD_ACQ(&p->ext_fault)) {
        (void)p_ext_unlock_site(p, U_GPU_DETACH);
        return -1;
    }
    return 0;
}
static int gp_eunlock(void *c) { return p_ext_unlock_site(((gpu_slot *)c)->p, U_GPU_DETACH); }

/* The HOLD (codex 12:53 rulings).  Only the drainer holds the declaration state, so anything else holds (the
 * finalization of an already drained detach reads nothing).  Sink failed: DISCARD (emit, output-free).  Otherwise
 * the descriptor must have been declared and the owner entry evidenced for its current tag; an ORPHAN entry stays
 * held (the seal turns it into the terminal LOSS 4), never rewritten to NONE. */
static int gp_emittable(void *c, const prg_meta *m)
{
    gpu_slot *g = c;
    prof *p = g->p;
    gpu_owner *o;
    const ext_acc *a;
    uint32_t ev, tag;
    if (!g_tls.in_drain)
        return 0;
    if (p->snk.failed)
        return 1;
    if (m->desc >= p->dr.desc_emitted)
        return 0;
    if (!m->owner_ref)
        return 1;
    o = &g->own[m->owner_ref - 1];
    tag = o->tag; /* the lease: an entry named by a published, undrained ticket is not reinstalled */
    ev = LD_ACQ(&o->evidence);
    if (ev == GPU_EV(GPU_EV_EVIDENCED, tag))
        return 1;
    if (ev == GPU_EV(GPU_EV_ORPHAN, tag)) {
        if (o->orphan_counted_tag != tag) {
            o->orphan_counted_tag = tag;
            g->orphaned++;
        }
        return 0;
    }
    a = &p->decl_acc[o->slot];
    if (ev == GPU_EV(GPU_EV_NONE, tag) && a->valid && a->incarnation == o->incarnation &&
        a->generation == o->generation) {
        ST_REL(&o->evidence, GPU_EV(GPU_EV_EVIDENCED, tag));
        return 1;
    }
    return 0;
}

static int gp_chunk(void *c, uint32_t type, const uint8_t *payload, size_t len)
{
    return p_ext_emit((prof *)c, type, payload, len);
}

/* ---- the drainer side ---- */

#ifdef PS3DK_PROF_TEST_HOOKS
static uint32_t g_snap_inits, g_snap_destroys; /* test accounting (process-wide) */
#endif

static void unpin(gpu_slot *g) { (void)__atomic_fetch_sub(&g->users, 1u, __ATOMIC_SEQ_CST); }

#ifdef PS3DK_PROF_TEST_HOOKS
#define SNAP_COUNT(x) ((x)++)
#else
#define SNAP_COUNT(x) ((void)0)
#endif

/* every initialized GPU snap mutex, exactly once (a later failed-init path, after p_gpu_init succeeded) */
void p_gpu_destroy(prof *p)
{
    uint32_t i;
    for (i = p->gpu_max; i--;)
        if (p->gpu[i].snap_ok) {
            (void)pal_mutex_destroy(&p->gpu[i].snap);
            p->gpu[i].snap_ok = 0;
            SNAP_COUNT(g_snap_destroys);
        }
}

int p_gpu_init(prof *p)
{
    uint32_t i;
    for (i = 0; i < p->gpu_max; i++) {
        int fail = pal_mutex_init(&p->gpu[i].snap) != 0;
#ifdef PS3DK_PROF_TEST_HOOKS
        if (ps3dk_prof_test_hooks.gpu_mutex_init_fail_at == i + 1) {
            if (!fail)
                (void)pal_mutex_destroy(&p->gpu[i].snap);
            fail = 1;
        }
#endif
        if (fail) {
            p_gpu_destroy(p); /* the ones before i, in reverse */
            return -1;
        }
        p->gpu[i].snap_ok = 1;
        SNAP_COUNT(g_snap_inits);
    }
    return 0;
}

void p_gpu_step(prof *p)
{
    uint32_t i;
    for (i = 0; i < p->gpu_max; i++)
        if (LD_ACQ(&p->gpu[i].state) == GPU_S_LIVE)
            prg_drain_held(&p->gpu[i].dom);
}

/* The detach acknowledgement, in two halves around the drainer's closing extension unlock (codex 16:11).
 * COLLECT, still holding the extension mutex: the exact domains this turn finalized are recorded (at most gpu_max,
 * the caller's array), each per-turn marker is cleared under the lock, and each slot is PINNED (users++), so neither
 * a release nor an attach can recycle it.  PUBLISH, after the unlock: only those recorded slots are touched, the
 * outcome is stored, and the unpin is the last access.  No other domain field is read outside its protection.  (The
 * drainer's pins add at most gpu_max to a count the API bounds at GPU_USERS_MAX = 2^31: no u32 overflow.) */
uint32_t p_gpu_ack_collect(prof *p, uint32_t *idx)
{
    uint32_t i, n = 0;
    for (i = 0; i < p->gpu_max; i++) {
        prg_dom *d = &p->gpu[i].dom;
        if (d->detach_turn) {
            d->detach_turn = 0;
            (void)__atomic_fetch_add(&p->gpu[i].users, 1u, __ATOMIC_SEQ_CST);
            idx[n++] = i;
        }
    }
    return n;
}

void p_gpu_ack_publish(prof *p, const uint32_t *idx, uint32_t n, int ok)
{
    uint32_t k;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (n && ps3dk_prof_test_hooks.gpu_before_ack)
        ps3dk_prof_test_hooks.gpu_before_ack(); /* between the real closing unlock and the publication */
#endif
    for (k = 0; k < n; k++) {
        gpu_slot *g = &p->gpu[idx[k]];
        ST_REL(&g->dom.detach_ack, ok ? 1u : 2u);
#ifdef PS3DK_PROF_TEST_HOOKS
        if (ps3dk_prof_test_hooks.gpu_after_ack_store)
            ps3dk_prof_test_hooks.gpu_after_ack_store(); /* published, still pinned */
#endif
        unpin(g); /* the last access */
    }
}

void p_gpu_seal(prof *p)
{
    uint32_t i;
    for (i = 0; i < p->gpu_max; i++)
        if (LD_ACQ(&p->gpu[i].state) == GPU_S_LIVE)
            prg_seal_held(&p->gpu[i].dom); /* idempotent: the second call finds a terminal state */
}

/* the free scan: the drainer, under thread_mutex, before decl_acc[slot] is cleared.  Every installed entry naming
 * this exact incarnation that has no evidence for its current tag gets it: EVIDENCED when its THREADS identity was
 * accepted, else ORPHAN.  thread_mutex orders this against every install. */
void p_gpu_slot_freed(prof *p, uint32_t slot)
{
    const struct slot *s = &p->slots[slot];
    const ext_acc *a = &p->decl_acc[slot];
    uint32_t i, j;
    for (i = 0; i < p->gpu_max; i++)
        for (j = 0; j < GPU_OWNER_REFS; j++) {
            gpu_owner *o = &p->gpu[i].own[j];
            if (!o->installed || o->slot != slot || o->incarnation != s->incarnation ||
                o->generation != s->generation || LD_ACQ(&o->evidence) != GPU_EV(GPU_EV_NONE, o->tag))
                continue;
            ST_REL(&o->evidence, (a->valid && a->incarnation == o->incarnation && a->generation == o->generation)
                                     ? GPU_EV(GPU_EV_EVIDENCED, o->tag)
                                     : GPU_EV(GPU_EV_ORPHAN, o->tag));
        }
}

/* ---- the API prologue: TLS, state, PIN, then (and only then) domain fields ---- */

static int map(int rc)
{
    switch (rc) {
    case PRG_OK: return PS3DK_PROF_OK;
    case PRG_EINVAL: return PS3DK_PROF_EINVAL;
    case PRG_EBUSY: return PS3DK_PROF_EBUSY;
    case PRG_ETIMEDOUT: return PS3DK_PROF_ETIMEDOUT;
    case PRG_ECLOCK: return PS3DK_PROF_ECLOCK;
    case PRG_EOVERFLOW: return PS3DK_PROF_ESEQLIMIT;
    default: return PS3DK_PROF_ESTOPPED; /* ESTOPPED, EFAULT, EANOMALY: the reason is in the stats */
    }
}

/* a bounded SC CAS increment (refused at GPU_USERS_MAX without incrementing), then the SC id check */
static gpu_slot *pin(prof *p, uint32_t h, int *rc)
{
    uint32_t i;
    for (i = 0; i < p->gpu_max; i++) {
        gpu_slot *g = &p->gpu[i];
        uint32_t u = LD_SC_(&g->users);
        do {
            if (u >= GPU_USERS_MAX) {
                *rc = PS3DK_PROF_EBUSY;
                return NULL;
            }
        } while (!CAS_SC_(&g->users, &u, u + 1u));
#ifdef PS3DK_PROF_TEST_HOOKS
        if (ps3dk_prof_test_hooks.gpu_after_pin_inc)
            ps3dk_prof_test_hooks.gpu_after_pin_inc();
#endif
        if (h && LD_SC_(&g->id) == h)
            return g;
        unpin(g);
    }
    *rc = PS3DK_PROF_EINVAL; /* stale, never issued, or INERT (0) */
    return NULL;
}

/* 0 with the slot pinned and in_gpu set; every later exit goes through done() */
static int prologue(uint32_t h, prof **pp, gpu_slot **gp)
{
    prof *p;
    gpu_slot *g;
    int rc = PS3DK_PROF_EINVAL;
    if (g_tls.in_drain || g_tls.in_gpu)
        return PS3DK_PROF_EBUSY;
    p = p_ready() ? g_prof : NULL;
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (!p->gpu_max)
        return PS3DK_PROF_ENOTSUP;
    if (LD_ACQ(&p->ext_fault))
        return PS3DK_PROF_ESTOPPED;
    g = pin(p, h, &rc);
    if (!g)
        return rc;
    if (!pal_owner_is_self(&g->owner)) {
        unpin(g);
        return PS3DK_PROF_EPERM;
    }
    g_tls.in_gpu = 1;
    *pp = p;
    *gp = g;
    return PS3DK_PROF_OK;
}

static int done(gpu_slot *g, int rc)
{
    g_tls.in_gpu = 0;
    unpin(g);
    return rc;
}

/* the producer calls' common checks after the prologue: the ctx, then the shutdown claim (admitted-before-stop
 * calls that passed it proceed; the core's SC handshake decides their side of the frozen frontier) */
static int producer_checks(prof *p, gpu_slot *g, struct CellGcmContextData *ctx)
{
    if (ctx != g->ctx) {
        g->refused_ctx++;
        return PS3DK_PROF_EINVAL;
    }
    if (LD_ACQ(&p->shutdown_once))
        return PS3DK_PROF_ESTOPPED;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.gpu_after_prologue)
        ps3dk_prof_test_hooks.gpu_after_prologue();
#endif
    return PS3DK_PROF_OK;
}

/* the current owner entry's lease covers every ticket submitted while it is current */
static void note_ticket(gpu_slot *g)
{
    if (g->cur_ref)
        g->own[g->cur_ref - 1].last_ticket = g->dom.p_ticket;
}

/* the extension mutex for attach / release / stats, with the I2 fault rules; 0 with it held */
static int ext_enter(prof *p)
{
    if (LD_ACQ(&p->ext_fault))
        return PS3DK_PROF_ESTOPPED;
    if (p_ext_lock_at(p) != 0) {
        ST_REL(&p->ext_fault, 1u);
        ST_RLX(&p->lock_failed, 1);
        return PS3DK_PROF_ESTOPPED;
    }
    if (LD_ACQ(&p->ext_fault)) {
        (void)p_ext_unlock_site(p, U_ANY);
        return PS3DK_PROF_ESTOPPED;
    }
    return PS3DK_PROF_OK;
}

/* ---- the API ---- */

int ps3dk_prof_gpu_attach(struct CellGcmContextData *ctx, uint32_t report_first, uint32_t report_count,
                          uint32_t label, uint32_t *gpu)
{
    prof *p;
    gpu_slot *g = NULL;
    const prg_port *dev = NULL;
    const gpp_dev *gdev = NULL;
    prg_port port;
    prg_out out;
    uint32_t i, id;
    int rc;
    if (g_tls.in_drain || g_tls.in_gpu)
        return PS3DK_PROF_EBUSY;
    p = p_ready() ? g_prof : NULL;
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (!p->gpu_max)
        return PS3DK_PROF_ENOTSUP;
#ifdef PS3DK_PROF_TEST_HOOKS
    dev = (const prg_port *)ps3dk_prof_test_hooks.gpu_port;
    gdev = (const gpp_dev *)ps3dk_prof_test_hooks.gpu_gpp_dev;
#endif
    if (dev && gdev)
        return PS3DK_PROF_EINVAL; /* two device sources: a test-configuration error, nothing changes */
    if (!dev && !gdev)
        return PS3DK_PROF_ENOTSUP; /* no device binding (host: a test hook; target: R4b) */
    if (!ctx || !gpu)
        return PS3DK_PROF_EINVAL;
    if (LD_ACQ(&p->ext_fault) || LD_ACQ(&p->shutdown_once))
        return PS3DK_PROF_ESTOPPED;
    if (report_count < 2 || report_count > p->gpu_slots_max || report_first > GPU_REPORTS - report_count ||
        label < GPU_LABEL_FIRST || label > GPU_LABEL_LAST)
        return PS3DK_PROF_EINVAL;
    rc = ext_enter(p);
    if (rc != PS3DK_PROF_OK)
        return rc;
    g_tls.in_gpu = 1;
    if (p->ext_sealed) {
        rc = PS3DK_PROF_ESTOPPED;
        goto out;
    }
    for (i = 0; i < p->gpu_max; i++) { /* no overlap with any domain that may still use its resources */
        const gpu_slot *o = &p->gpu[i];
        if (LD_ACQ(&o->state) == GPU_S_FREE)
            continue;
        if (o->ctx == (void *)ctx || o->label == label ||
            (report_first < o->report_first + o->report_count && o->report_first < report_first + report_count)) {
            rc = PS3DK_PROF_EINVAL;
            goto out;
        }
    }
    if (p->gpu_lifetime >= GPU_LIFETIME_MAX) {
        rc = PS3DK_PROF_EFULL;
        goto out;
    }
    for (i = 0; i < p->gpu_max && !g; i++)
        if (LD_ACQ(&p->gpu[i].state) == GPU_S_FREE && LD_SC_(&p->gpu[i].users) == 0)
            g = &p->gpu[i];
    if (!g) {
        rc = PS3DK_PROF_EFULL;
        goto out;
    }
    if (pal_owner_get(&g->owner) != 0) {
        rc = PS3DK_PROF_ETHREAD;
        goto out;
    }
    /* the GCM port: (re)prepared only here, the slot FREE with users 0 under the extension mutex, before prg_attach
     * can write through it; in_gpu is set, so an accessor re-entering the API gets EBUSY.  A failure leaves the port
     * zeroed and changes nothing else that is visible (id, lifetime, ledger, output, slot availability). */
    if (gdev) {
        if (gpp_prepare(&g->gpp, gdev, (gpp_ctx *)(void *)ctx, report_first, report_count, label) != PRG_OK) {
            rc = PS3DK_PROF_EINVAL;
            goto out;
        }
        memset(&g->devport, 0, sizeof g->devport);
        gpp_bind(&g->devport, &g->gpp); /* device ops only; their ctx is &g->gpp, never g */
        dev = &g->devport;
    }
    /* every domain field before the id is published (the pinning words and the owner entries are untouched) */
    id = GPU_ID_BASE + p->gpu_lifetime;
    g->dev = dev;
    g->ctx = ctx;
    g->label = label;
    g->report_first = report_first;
    g->report_count = report_count;
    g->cur_ref = 0;
    g->owner_busy = g->refused_scope = g->refused_ctx = 0;
    g->orphaned = 0;
    port = (prg_port){g, gp_reserve, gp_flush, gp_rlabel, gp_wlabel, gp_rtimer, gp_wtimer, gp_sync, gp_mftb, gp_now,
                      gp_slock, gp_sunlock, gp_elock, gp_eunlock, gp_emittable};
    out = (prg_out){p, gp_chunk};
    rc = map(prg_attach(&g->dom, id, report_first, report_count, label, &port, &out, p->gpu_ledger, g->meta, g->stage,
                        g->scratch, report_count));
    if (rc != PS3DK_PROF_OK)
        goto out;
    g->dom.drainer_finalizes = 1; /* the drainer is the sole encoder: a detach is finalized by it, never here */
    p->gpu_lifetime++;
    ST_REL(&g->state, (uint32_t)GPU_S_LIVE);
    ST_SC_(&g->id, id);
    g_tls.in_gpu = 0;
    if (p_ext_unlock_site(p, U_GPU_ATTACH) != 0) {
        ST_REL(&g->state, (uint32_t)GPU_S_QUARANTINED); /* attached, never usable: borrowed to process end */
        *gpu = id;
        return PS3DK_PROF_ESTOPPED;
    }
    *gpu = id;
    return PS3DK_PROF_OK;
out:
    g_tls.in_gpu = 0;
    return p_ext_unlock_site(p, U_GPU_ATTACH) == 0 ? rc : PS3DK_PROF_ESTOPPED;
}

int ps3dk_prof_gpu_begin(uint32_t h, struct CellGcmContextData *ctx, ps3dk_prof_desc *desc, ps3dk_prof_gpu_zone *out)
{
    prof *p;
    gpu_slot *g;
    prg_token t;
    uint32_t idx, scope;
    int rc;
    if (!out)
        return (g_tls.in_drain || g_tls.in_gpu) ? PS3DK_PROF_EBUSY : PS3DK_PROF_EINVAL; /* nothing written */
    memset(out, 0, sizeof *out);                                                         /* INERT */
    rc = prologue(h, &p, &g);
    if (rc != PS3DK_PROF_OK)
        return rc;
    rc = producer_checks(p, g, ctx);
    if (rc != PS3DK_PROF_OK)
        return done(g, rc);
    rc = p_gpu_desc(p, desc, &idx, &scope);
    if (rc == PS3DK_PROF_OK && scope != 2)
        rc = PS3DK_PROF_EINVAL;
    if (rc != PS3DK_PROF_OK) {
        g->refused_scope += rc == PS3DK_PROF_EINVAL;
        return done(g, rc);
    }
    rc = map(prg_begin(&g->dom, idx, &t));
    note_ticket(g);
    if (rc == PS3DK_PROF_OK)
        *out = (ps3dk_prof_gpu_zone){h, t.depth, t.begin_seq, t.desc, 0u};
    return done(g, rc);
}

int ps3dk_prof_gpu_end(uint32_t h, struct CellGcmContextData *ctx, ps3dk_prof_gpu_zone *zone)
{
    prof *p;
    gpu_slot *g;
    prg_token t;
    int rc;
    if (!zone)
        return (g_tls.in_drain || g_tls.in_gpu) ? PS3DK_PROF_EBUSY : PS3DK_PROF_EINVAL;
    rc = prologue(h, &p, &g);
    if (rc != PS3DK_PROF_OK)
        return rc;
    rc = producer_checks(p, g, ctx);
    if (rc != PS3DK_PROF_OK)
        return done(g, rc);
    if (zone->gpu != h)
        return done(g, PS3DK_PROF_EINVAL); /* INERT, or another domain's zone */
    t = (prg_token){h, zone->depth, zone->begin, zone->desc, 0u};
    rc = map(prg_end(&g->dom, &t, zone->desc));
    note_ticket(g);
    return done(g, rc);
}

int ps3dk_prof_gpu_mark(uint32_t h, struct CellGcmContextData *ctx, ps3dk_prof_desc *desc)
{
    prof *p;
    gpu_slot *g;
    uint32_t idx, scope;
    int rc = prologue(h, &p, &g);
    if (rc != PS3DK_PROF_OK)
        return rc;
    rc = producer_checks(p, g, ctx);
    if (rc != PS3DK_PROF_OK)
        return done(g, rc);
    rc = p_gpu_desc(p, desc, &idx, &scope);
    if (rc == PS3DK_PROF_OK && scope != 2)
        rc = PS3DK_PROF_EINVAL;
    if (rc != PS3DK_PROF_OK) {
        g->refused_scope += rc == PS3DK_PROF_EINVAL;
        return done(g, rc);
    }
    rc = map(prg_mark(&g->dom, idx));
    note_ticket(g);
    return done(g, rc);
}

/* an entry this domain's producer may (re)install: never installed, a released domain's (decided from dom_id
 * alone, before any producer-private field), or its own, not current, with every ticket naming it drained; and a
 * tag still below the maximum */
static int reclaimable(const gpu_slot *g, const gpu_owner *o)
{
    if (o->tag + 1u >= GPU_TAG_MAX)
        return 0; /* retired from installation: tags are never wrapped or reused */
    if (!o->installed || o->dom_id != g->dom.domain_id)
        return 1;
    return !o->cur && o->last_ticket <= LD_ACQ(&g->dom.drained_pub);
}

int ps3dk_prof_gpu_frame(uint32_t h, ps3dk_prof_token tok)
{
    prof *p;
    gpu_slot *g;
    uint32_t i, j, ref = 0;
    int installed = 0;
    int rc = prologue(h, &p, &g);
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (LD_ACQ(&p->shutdown_once) || LD_RLX(&p->lock_failed))
        return done(g, PS3DK_PROF_ESTOPPED);
    if (tok.reserved || (!tok.owner && tok.frame))
        return done(g, PS3DK_PROF_EINVAL);
    if (tok.owner) {
        const struct slot *s = NULL;
        gpu_owner *cur = g->cur_ref ? &g->own[g->cur_ref - 1] : NULL;
        if (p_lock(p, &p->thread_mutex) != 0)
            return done(g, PS3DK_PROF_ESTOPPED); /* lock_failed is latched: the terminal PAL fault */
        /* validate and install in ONE thread_mutex section: a retirement is wholly before or wholly after */
        for (i = 0; i < p->nslots && !s; i++)
            if (p->slots[i].state == SLOT_LIVE && p->slots[i].incarnation == tok.owner)
                s = &p->slots[i];
        if (!s || tok.frame >= LD_ACQ(&s->cb->current_frame)) {
            p_unlock(p, &p->thread_mutex);
            /* a failed closing unlock is terminal on every path: ESTOPPED, nothing changed */
            return done(g, LD_RLX(&p->lock_failed) ? PS3DK_PROF_ESTOPPED : PS3DK_PROF_EINVAL);
        }
#ifdef PS3DK_PROF_TEST_HOOKS
        if (ps3dk_prof_test_hooks.gpu_in_frame_install)
            ps3dk_prof_test_hooks.gpu_in_frame_install();
#endif
        if (cur && cur->incarnation == tok.owner && cur->generation == s->generation) {
            ref = g->cur_ref; /* the same owner: its entry stays current */
            installed = 0;
        } else {
            for (j = 0; j < GPU_OWNER_REFS && !ref; j++)
                if (reclaimable(g, &g->own[j]))
                    ref = j + 1;
            if (!ref) {
                p_unlock(p, &p->thread_mutex);
                if (LD_RLX(&p->lock_failed))
                    return done(g, PS3DK_PROF_ESTOPPED); /* terminal; the current owner and every lease unchanged */
                g->owner_busy++;
                return done(g, PS3DK_PROF_EBUSY); /* the current owner is unchanged */
            }
            {
                gpu_owner *o = &g->own[ref - 1];
                o->installed = 1;
                o->dom_id = g->dom.domain_id;
                o->incarnation = tok.owner;
                o->slot = (uint32_t)(s - p->slots);
                o->generation = s->generation;
                o->tag++;
                ST_REL(&o->evidence, GPU_EV(GPU_EV_NONE, o->tag)); /* the only reset: inside the install */
                o->cur = 1;
                o->last_ticket = g->dom.p_ticket;
            }
            installed = 1;
        }
        p_unlock(p, &p->thread_mutex);
        if (LD_RLX(&p->lock_failed)) {
            /* the thread_mutex unlock failed (the runtime's terminal PAL fault, failing the capture closed at
             * shutdown): ESTOPPED; the core's owner is unchanged; a fresh entry is not current (reclaimable once
             * drained: it names no ticket); the current entry keeps its lease */
            if (installed)
                g->own[ref - 1].cur = 0;
            return done(g, PS3DK_PROF_ESTOPPED);
        }
    }
    rc = map(prg_frame(&g->dom, tok.owner, tok.frame, (uint8_t)ref));
    if (rc == PS3DK_PROF_OK) {
        if (g->cur_ref && g->cur_ref != ref)
            g->own[g->cur_ref - 1].cur = 0; /* still leased by its outstanding tickets */
        g->cur_ref = ref;
    } else if (installed) {
        g->own[ref - 1].cur = 0; /* installed but not current: reclaimable once drained (it names no ticket) */
    }
    return done(g, rc);
}

int ps3dk_prof_gpu_calibrate(uint32_t h, struct CellGcmContextData *ctx, uint64_t timeout_us)
{
    prof *p;
    gpu_slot *g;
    int rc = prologue(h, &p, &g);
    if (rc != PS3DK_PROF_OK)
        return rc;
    rc = producer_checks(p, g, ctx);
    if (rc != PS3DK_PROF_OK)
        return done(g, rc);
    rc = map(prg_calibrate(&g->dom, timeout_us));
    note_ticket(g);
    return done(g, rc);
}

/* one inline drain when the caller is the designated APP drain thread; otherwise the drain thread (or the
 * designated APP thread's own drains) progress it, and the caller only sleeps */
static void poll_inline(void *c)
{
    prof *p = c;
    if (p->cfg.mode == PS3DK_PROF_MODE_APP && pal_owner_is_self(&p->owner))
        p_ext_inline_drain(p);
    else
        pal_sleep_us(200);
}

/* The caller only REQUESTS the detach and waits: the DRAINER (prg_drain_held, the sole encoder of the file, with its
 * in_drain guard) finalizes it, and the drainer acknowledges the finalizing turn only after its closing extension
 * unlock (p_gpu_ack).  OK: retired and that turn completed (FINAL accepted or, with a failed sink, refused: see the
 * stats' final_written); ESTOPPED: the turn's closing unlock failed, or the seal won; ETIMEDOUT while pending.  The
 * caller holds no lock while it waits; its inline drain (designated APP thread) takes the extension mutex itself. */
int ps3dk_prof_gpu_detach(uint32_t h, struct CellGcmContextData *ctx, uint64_t timeout_us)
{
    prof *p;
    gpu_slot *g;
    int rc = prologue(h, &p, &g);
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (ctx != g->ctx) {
        g->refused_ctx++;
        return done(g, PS3DK_PROF_EINVAL);
    }
    rc = map(prg_detach_wait(&g->dom, timeout_us, poll_inline, p));
    if (LD_ACQ(&p->ext_fault))
        rc = PS3DK_PROF_ESTOPPED;
    return done(g, rc);
}

int ps3dk_prof_gpu_release(uint32_t h, uint32_t *released_or_null)
{
    prof *p;
    gpu_slot *g;
    uint32_t rel = 0;
    int rc;
    if (released_or_null)
        *released_or_null = 0;
    rc = prologue(h, &p, &g);
    if (rc != PS3DK_PROF_OK)
        return rc;
    rc = ext_enter(p);
    if (rc != PS3DK_PROF_OK)
        return done(g, rc); /* no provable lock: the resources stay borrowed */
    if (LD_ACQ(&g->dom.state) == PRG_DETACH_FINAL && LD_ACQ(&g->dom.detach_ack) == 0) {
        rc = PS3DK_PROF_EBUSY; /* the detach's completion is pending: release would bypass the outcome it awaits */
    } else if (prg_release(&g->dom) == PRG_OK) {
        ST_SC_(&g->id, 0u);                          /* stale callers now miss */
        ST_REL(&g->state, (uint32_t)GPU_S_QUARANTINED); /* not FREE until the unlock is proven */
        rel = 1;
        rc = PS3DK_PROF_OK;
    } else {
        rc = PS3DK_PROF_EBUSY; /* not retired, or a call still in flight */
    }
    if (p_ext_unlock_site(p, U_GPU_RELEASE) != 0) {
        rc = PS3DK_PROF_ESTOPPED; /* the release took effect (rel); the slot stays QUARANTINED */
    } else if (rel) {
        ST_REL(&g->state, (uint32_t)GPU_S_FREE);
    }
    if (released_or_null)
        *released_or_null = rel;
    return done(g, rc);
}

int ps3dk_prof_gpu_get_stats(uint32_t h, ps3dk_prof_gpu_stats *st)
{
    prof *p;
    gpu_slot *g;
    ps3dk_prof_gpu_stats o;
    const prg_dom *d;
    int rc;
    if (!st)
        return (g_tls.in_drain || g_tls.in_gpu) ? PS3DK_PROF_EBUSY : PS3DK_PROF_EINVAL;
    rc = prologue(h, &p, &g);
    if (rc != PS3DK_PROF_OK)
        return rc;
    d = &g->dom;
    memset(&o, 0, sizeof o);
    /* the producer's own counters: this IS the ctx owner thread */
    o.refused_api = d->st.refused_api;
    o.refused_mark_depth = d->st.refused_mark_depth;
    o.refused_suppress_overflow = d->st.refused_suppress_overflow;
    o.refused_scope = g->refused_scope;
    o.refused_ctx = g->refused_ctx;
    o.owner_busy = g->owner_busy;
    o.lost_ring = d->st.lost_ring;
    o.lost_fifo = d->st.lost_fifo;
    o.lost_records = d->st.lost_records;
    o.brackets = d->st.brackets;
    o.cal_bad = d->st.cal_bad;
    rc = ext_enter(p);
    if (rc != PS3DK_PROF_OK)
        return done(g, rc);
    o.events = d->events_accepted;
    o.lost = d->lost_accepted;
    o.held = d->st.held;
    o.attribution_orphaned = g->orphaned;
    o.brackets_dropped = d->st.brackets_dropped;
    o.retired = LD_ACQ(&d->retired);
    o.final_written = d->final_written;
    o.final_state = d->final_state;
    o.fault = LD_ACQ(&d->fault);
    o.anomaly = LD_ACQ(&d->anomaly);
    o.loss_exhausted = d->loss_exhausted;
    o.overflow = LD_ACQ(&d->overflow);
    if (p_ext_unlock_site(p, U_GPU_STATS) != 0)
        return done(g, PS3DK_PROF_ESTOPPED);
    *st = o;
    return done(g, PS3DK_PROF_OK);
}

#ifdef PS3DK_PROF_TEST_HOOKS
/* test accessors (never in an SDK archive) */
uint32_t ps3dk_prof__test_gpu_users(unsigned i);
void ps3dk_prof__test_gpu_set_users(unsigned i, uint32_t v);
uint32_t ps3dk_prof__test_gpu_state(unsigned i);
int ps3dk_prof__test_gpu_limits(uint32_t out[8]);
uint64_t ps3dk_prof__test_ext_subtotal(void);
void ps3dk_prof__test_gpu_set_tags(unsigned i, uint32_t tag);
uint32_t ps3dk_prof__test_gpu_tag(unsigned i, unsigned j);
/* every owner entry of table slot i: its install tag (tests set it near the maximum) */
void ps3dk_prof__test_gpu_set_tags(unsigned i, uint32_t tag)
{
    for (unsigned j = 0; j < GPU_OWNER_REFS; j++)
        g_prof->gpu[i].own[j].tag = tag;
}
uint32_t ps3dk_prof__test_gpu_tag(unsigned i, unsigned j) { return g_prof->gpu[i].own[j].tag; }
uint32_t ps3dk_prof__test_gpu_users(unsigned i) { return g_prof->gpu ? LD_SC_(&g_prof->gpu[i].users) : 0; }
/* R4a: is table slot i's GCM port prepared (its readiness word)? */
uint32_t ps3dk_prof__test_gpu_gpp_ready(unsigned i);
uint32_t ps3dk_prof__test_gpu_gpp_ready(unsigned i) { return g_prof->gpu ? LD_ACQ(&g_prof->gpu[i].gpp.ready) : 0; }
/* R4a: exhaust (1) or restore (0) the GPU ledger's LOSS credit, so prg_attach's own credit check refuses.  Only while
 * no drain can run (the test's APP-mode designated thread calls it between API calls). */
void ps3dk_prof__test_gpu_ledger_exhaust(int on);
void ps3dk_prof__test_gpu_ledger_exhaust(int on)
{
    static uint32_t saved;
    prg_ledger *l = g_prof->gpu_ledger;
    if (on) {
        saved = l->loss_used;
        l->loss_used = l->loss_max - l->loss_reserved;
    } else {
        l->loss_used = saved;
    }
}
void ps3dk_prof__test_gpu_set_users(unsigned i, uint32_t v) { ST_SC_(&g_prof->gpu[i].users, v); }
uint32_t ps3dk_prof__test_gpu_state(unsigned i) { return g_prof->gpu ? LD_ACQ(&g_prof->gpu[i].state) : 0; }
/* {spu loss_chunks, spu brackets_file, spu epochs, gpu loss_max, gpu br_epoch_max, gpu br_file_max, gpu_max, slots} */
int ps3dk_prof__test_gpu_limits(uint32_t out[8])
{
    prof *p = g_prof;
    memset(out, 0, 8 * sizeof *out);
    if (p->spu) {
        out[0] = p->spu->lim.loss_chunks;
        out[1] = p->spu->lim.brackets_file;
        out[2] = p->spu->lim.epochs;
    }
    if (p->gpu_ledger) {
        out[3] = p->gpu_ledger->loss_max;
        out[4] = p->gpu_ledger->br_epoch_max;
        out[5] = p->gpu_ledger->br_file_max;
    }
    out[6] = p->gpu_max;
    out[7] = p->gpu_slots_max;
    return 0;
}
uint64_t ps3dk_prof__test_ext_subtotal(void) { return g_prof->spu_bytes; }
void ps3dk_prof__test_gpu_mutex_counts(uint32_t *inits, uint32_t *destroys);
void ps3dk_prof__test_gpu_mutex_counts(uint32_t *inits, uint32_t *destroys)
{
    *inits = g_snap_inits;
    *destroys = g_snap_destroys;
}
#endif

#endif
