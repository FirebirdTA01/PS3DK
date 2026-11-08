/* The SPU lane extension inside the runtime (P3.2 slice I2, contract delta r2 241f83f9): the psp_mem / psp_out
 * adapters, the lane-memory slot allocator, owner coverage, step 3b, the seal, and the lane API.
 *
 * Locking: every psp call runs under spu_mutex, which is never held with thread_mutex.  ext_fault (atomic) is
 * checked before taking spu_mutex and again after acquiring it; every failed unlock is terminal (ext_fault +
 * lock_failed), on every path.  Residual: a real failed unlock may leave the mutex locked, and a later acquirer may
 * then block forever; no recovery or shutdown bound is promised for that case.
 *
 * Two memory adapters: the host one emulates the line premises (psp_host_emul.h; not target evidence); the LV2 one
 * (R4b, both PPU data models) uses plain aligned accesses and real barriers.  Its correctness rests on the OPEN target
 * assumptions T1-T6 of p3.3-r4b r2 (line-B putlluc indivisible and coherent as seen by PPU loads, ordered loads, ring
 * data visible before its publication, single-copy aligned line-A stores whose stores cancel an SPU reservation,
 * MFC coherence); the build gates check only the code shapes they require. */
/* configuration refusals, before any include, so each fails with its own message (R4b) */
#ifdef PS3DK_PROF_WITH_SPU
#if defined(PS3DK_PROF_PAL_HOST) && defined(PS3DK_PROF_PAL_LV2)
#error "the SPU lane extension: PS3DK_PROF_PAL_HOST and PS3DK_PROF_PAL_LV2 together"
#elif !defined(PS3DK_PROF_PAL_HOST) && !defined(PS3DK_PROF_PAL_LV2)
#error "the SPU lane extension: no PAL selected"
#endif
#ifdef PS3DK_PROF_PAL_LV2
#ifdef PS3DK_PROF_TEST_HOOKS
#error "the SPU lane extension target never carries test hooks"
#endif
#ifdef GPP_HOST_DEVMEM
#error "GPP_HOST_DEVMEM is a host-only test seam"
#endif
#endif
#endif
#include "prof_internal.h"

#ifdef PS3DK_PROF_WITH_SPU

#include <string.h>

#include "prof_spu.h"
#ifdef PS3DK_PROF_PAL_HOST
#include "psp_host_emul.h"
#endif
#ifdef PS3DK_PROF_PAL_LV2
/* both PPU data models: the pointer, uintptr_t and size_t widths are the ones __LP64__ selects (an EA is the
 * process address as an unsigned 64-bit value either way; the size checks rely on the size_t width) */
#ifdef __LP64__
#define PS3DK_PROF__PTR_BYTES 8
#else
#define PS3DK_PROF__PTR_BYTES 4
#endif
_Static_assert(sizeof(void *) == PS3DK_PROF__PTR_BYTES && sizeof(uintptr_t) == sizeof(void *) &&
                   sizeof(size_t) == sizeof(void *),
               "LV2 extension: the data model matches __LP64__");
#endif
#include "psp_elf.h"
#include "psp_ppu.h"
#ifdef PS3DK_PROF_TEST_HOOKS
#include "prof_test_hooks.h"
#endif


/* ---- the psp_mem adapter: plain process memory; EAs are addresses; host emulation of the line premises ---- */

static uint8_t *at(uint64_t ea) { return (uint8_t *)(uintptr_t)ea; }

#ifdef PS3DK_PROF_PAL_HOST

static uint64_t m_ld64(void *c, uint64_t ea)
{
    uint64_t v;
    (void)c;
    psp_emul_line_lock(ea);
    v = psp_rd64(at(ea));
    psp_emul_line_unlock(ea);
    return v;
}
static void m_ld_line(void *c, uint64_t ea, uint8_t out[128])
{
    (void)c;
    psp_emul_line_lock(ea);
    memcpy(out, at(ea), 128);
    psp_emul_line_unlock(ea);
}
static void m_ld_slots(void *c, uint64_t ea, uint8_t *out, size_t n)
{
    (void)c;
    memcpy(out, at(ea), n); /* ordered after a line-B load that saw the publication (the line mutex edge) */
}
static void line_a_store(uint64_t ea, const void *v, size_t n)
{
    uint64_t line = ea & ~(uint64_t)127;
    if (!psp_emul_drop_line_a)
        psp_emul_line_lock(line);
    memcpy(at(ea), v, n);
    psp_emul_version_bump(line);
    if (!psp_emul_drop_line_a)
        psp_emul_line_unlock(line);
}
static void m_st64(void *c, uint64_t ea, uint64_t v)
{
    uint8_t b[8];
    (void)c;
    psp_be64(b, v);
    line_a_store(ea, b, 8);
}
static void m_st16(void *c, uint64_t ea, uint16_t v)
{
    uint8_t b[2];
    (void)c;
    psp_be16(b, v);
    line_a_store(ea, b, 2);
}
/* lwsync / sync / isync: on the host the happens-before edges come from the emulation's line mutexes (above), so the
 * barrier hooks order nothing here; the target's barriers are the target-port gate.  (A C fence would also be
 * unsupported under TSan.) */
static void m_fence(void *c) { (void)c; }
static uint64_t m_mftb(void *c)
{
    (void)c;
    return p_tb();
}
#define M_LWSYNC m_fence
#define M_SYNC m_fence
#define M_ISYNC m_fence

#else /* PS3DK_PROF_PAL_LV2 */

/* The EA is the process address as an unsigned 64-bit value (zero-extended in ILP32; at() above).  Every line-A
 * store is ONE naturally aligned store (assumption T5); native big-endian order equals the psp_be64 / psp_rd64 byte
 * order. */
static uint64_t m_ld64(void *c, uint64_t ea)
{
    (void)c;
    return *(const volatile uint64_t *)(const void *)at(ea);
}
/* 16 aligned doubleword loads of the line; a torn copy is the drainer's seqlock's concern under T1 (not detected in
 * general: the WEAK-model counterexample stands).  The private output buffer is filled by an endian-preserving copy. */
static void m_ld_line(void *c, uint64_t ea, uint8_t out[128])
{
    const volatile uint64_t *src = (const volatile uint64_t *)(const void *)at(ea);
    unsigned i;
    (void)c;
#pragma GCC unroll 16 /* straight-line: the linked-image gate counts the 16 source loads per path */
    for (i = 0; i < 16; i++) {
        uint64_t v = src[i];
        memcpy(out + 8 * i, &v, 8);
    }
}
static void m_ld_slots(void *c, uint64_t ea, uint8_t *out, size_t n)
{
    (void)c;
    memcpy(out, at(ea), n); /* ordered by the caller's lwsync after the accepting recheck */
}
static void m_st64(void *c, uint64_t ea, uint64_t v)
{
    (void)c;
    *(volatile uint64_t *)(void *)at(ea) = v;
}
static void m_st16(void *c, uint64_t ea, uint16_t v)
{
    (void)c;
    *(volatile uint16_t *)(void *)at(ea) = v;
}
static void m_lwsync(void *c)
{
    (void)c;
    __asm__ volatile("lwsync" ::: "memory");
}
static void m_sync(void *c)
{
    (void)c;
    __asm__ volatile("sync" ::: "memory");
}
static void m_isync(void *c)
{
    (void)c;
    __asm__ volatile("isync" ::: "memory");
}
static uint64_t m_mftb(void *c)
{
    (void)c;
    return p_tb();
}
#define M_LWSYNC m_lwsync
#define M_SYNC m_sync
#define M_ISYNC m_isync

#endif

/* the slot allocator over the init block: psp_lane_create asks for the ring, then the control block, of one lane */
static size_t slot_bytes(const prof *p) { return (size_t)p->spu->cfg.ring_slots * PSP_SLOT + PSP_CTL_BYTES; }

/* Lane slot i's address, computed out of line ONCE per allocation (after the free-slot scan, from its final index):
 * the call's result is the one value m_alloc zeroes and returns, so the linked code shows the zeroed region and the
 * returned EA as the same value in both data models (the image checker's G4a binding; in LP64 an inline sum would
 * be rebuilt from the scan's index on each path).  Behaviour is unchanged. */
static __attribute__((noinline)) uint8_t *slot_at(prof *p, uint32_t i)
{
    return p->spu_slots + (size_t)i * slot_bytes(p);
}

static int m_alloc(void *c, size_t size, uint64_t *ea)
{
    prof *p = c;
    size_t ring = (size_t)p->spu->cfg.ring_slots * PSP_SLOT;
    uint8_t *slot;
    uint32_t i;
    if (LD_ACQ(&p->ext_fault))
        return -1; /* frozen: an uncertain lifetime is never reused */
    if (size == ring) {
        for (i = 0; i < p->spu->cfg.lanes_max && p->ext_slot_used[i]; i++)
            ;
        if (i == p->spu->cfg.lanes_max)
            return -1;
        p->ext_slot_used[i] = 1;
        p->ext_pending_slot = i;
        slot = slot_at(p, i); /* once: the zeroed slot IS the EA returned below */
        memset(slot, 0, slot_bytes(p));
#ifdef PS3DK_PROF_PAL_LV2
        __asm__ volatile("sync" ::: "memory"); /* the zeroed ring + control block are performed before the lane's EA
                                                * can leave this thread (r4b D2) */
#endif
        *ea = (uint64_t)(uintptr_t)slot;
        return 0;
    }
    if (size == PSP_CTL_BYTES) {
        *ea = (uint64_t)(uintptr_t)(p->spu_slots + (size_t)p->ext_pending_slot * slot_bytes(p) + ring);
        return 0;
    }
    return -1;
}

static void m_free(void *c, uint64_t ea, size_t size)
{
    prof *p = c;
    if (size != (size_t)p->spu->cfg.ring_slots * PSP_SLOT || LD_ACQ(&p->ext_fault))
        return; /* the control block goes with its ring; a faulted extension never returns a slot */
    p->ext_slot_used[(size_t)(at(ea) - p->spu_slots) / slot_bytes(p)] = 0;
}

static int o_chunk(void *c, uint32_t type, const uint8_t *payload, size_t len)
{
    return p_ext_emit((prof *)c, type, payload, len);
}

/* the exact accepted identity (delta r2 section 2): valid, incarnation and generation all match */
static int covered(void *c, uint32_t inc, uint32_t slot, uint32_t gen)
{
    const prof *p = c;
    const ext_acc *a;
    if (slot >= p->nslots)
        return 0;
    a = &p->decl_acc[slot];
    return a->valid && a->incarnation == inc && a->generation == gen;
}

static psp_mem g_mem;
static psp_out g_out;

int p_ext_init(prof *p)
{
    if (pal_mutex_init(&p->spu_mutex) != 0)
        return -1;
    p->spu_mutex_ok = 1;
    if (p->spu) { /* SPU lanes enabled (a GPU-only extension has no PSP state) */
        g_mem = (psp_mem){p, m_ld64, m_ld_line, m_ld_slots, m_st64, m_st16, M_LWSYNC, M_SYNC, M_ISYNC, m_mftb, m_alloc,
                          m_free};
        g_out = (psp_out){p, o_chunk};
        p->spu->mem = &g_mem;
        p->spu->out = &g_out;
        p->spu->covered = covered;
        p->spu->covered_ctx = p;
    }
    if (p->gpu_max && p_gpu_init(p) != 0) { /* it destroyed its own mutexes; unwind the extension mutex */
        (void)pal_mutex_destroy(&p->spu_mutex);
        p->spu_mutex_ok = 0;
        return -1;
    }
    return 0;
}

/* ---- the drainer side: accepted THREADS identities, freed slots, step 3b and the seal ---- */

void p_ext_threads_accepted(prof *p, const tdecl *t, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        ext_acc *a = &p->decl_acc[t[i].slot];
        a->valid = 1;
        a->incarnation = t[i].incarnation;
        a->generation = t[i].generation;
    }
}

void p_ext_slot_freed(prof *p, uint32_t slot)
{
    if (p->gpu_max)
        p_gpu_slot_freed(p, slot); /* the GPU owner evidence, BEFORE the accepted identity is cleared */
    p->decl_acc[slot].valid = 0;
}

static int ext_lock(prof *p)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.spu_lock_fail_at &&
        ++ps3dk_prof_test_hooks.spu_ops == ps3dk_prof_test_hooks.spu_lock_fail_at)
        return -1;
    if (pal_mutex_trylock(&p->spu_mutex) == 0)
        return 0;
    __atomic_fetch_add(&ps3dk_prof_test_hooks.ext_lock_contended, 1u, __ATOMIC_SEQ_CST); /* S2: found it held */
#endif
    return pal_mutex_lock(&p->spu_mutex) == 0 ? 0 : -1;
}


/* 0, or -1 after setting the terminal fault (every failed unlock is terminal) */
static int ext_unlock_at(prof *p, uint32_t site)
{
    int rc;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (site && ps3dk_prof_test_hooks.spu_unlock_fail_site == site && ps3dk_prof_test_hooks.ext_before_failing_unlock)
        ps3dk_prof_test_hooks.ext_before_failing_unlock(); /* ordering schedules: work published before this unlock */
#endif
    rc = pal_mutex_unlock(&p->spu_mutex);
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.spu_unlock_fail_at &&
        ++ps3dk_prof_test_hooks.spu_ops == ps3dk_prof_test_hooks.spu_unlock_fail_at)
        rc = -1; /* the hook performed the REAL unlock above, then reports failure: fault outcome (a) */
    if (site && ps3dk_prof_test_hooks.spu_unlock_fail_site == site) {
        ps3dk_prof_test_hooks.spu_unlock_fail_site = 0;
        rc = -1;
    }
#else
    (void)site;
#endif
    if (rc == 0)
        return 0;
    ST_REL(&p->ext_fault, 1u);
    ST_RLX(&p->lock_failed, 1);
    return -1;
}

static int ext_unlock(prof *p) { return ext_unlock_at(p, U_ANY); }

/* the drainer and the seal: a known or new fault stops extension output and fails the capture closed */
static void drainer_run(prof *p, int seal)
{
    if (LD_ACQ(&p->ext_fault)) {
        p_ext_sink_fail(p, -3);
        return;
    }
    if (ext_lock(p) != 0) {
        ST_REL(&p->ext_fault, 1u);
        ST_RLX(&p->lock_failed, 1);
        p_ext_sink_fail(p, -3);
        return;
    }
    if (LD_ACQ(&p->ext_fault)) {
        (void)ext_unlock(p);
        p_ext_sink_fail(p, -3);
        return;
    }
    if (seal) {
#ifdef PS3DK_PROF_TEST_HOOKS
        if (ps3dk_prof_test_hooks.spu_before_seal)
            ps3dk_prof_test_hooks.spu_before_seal();
#endif
        if (p->spu)
            psp_seal(p->spu);
        /* corruption psp detected in a publication fails the RUNTIME sink at once, before any GPU output: no
         * trailer, no further output (sink_fail keeps an earlier first error); the output-free lifecycle continues */
        if (p->spu && p->spu->corrupt)
            p_ext_sink_fail(p, -2);
        if (p->gpu_max)
            p_gpu_seal(p);
        p->ext_sealed = 1;
    } else {
        if (p->spu)
            psp_drain(p->spu);
        if (p->spu && p->spu->corrupt)
            p_ext_sink_fail(p, -2); /* as above: before any GPU output */
        if (p->gpu_max)
            p_gpu_step(p);
    }
    {
        uint32_t acks[4], nack = p->gpu_max ? p_gpu_ack_collect(p, acks) : 0; /* under the lock: pinned */
        int ok = ext_unlock_at(p, seal ? U_SEAL : U_STEP3B) == 0;
        if (!ok)
            p_ext_sink_fail(p, -3);
        /* a detach finalized in this turn completes only now, with the unlock's outcome; only the pinned slots */
        p_gpu_ack_publish(p, acks, nack, ok);
    }
}

void p_ext_step3b(prof *p) { drainer_run(p, 0); }
void p_ext_seal(prof *p) { drainer_run(p, 1); }

/* ---- the API ---- */

static int map_rc(int rc)
{
    switch (rc) {
    case PSP_P_OK: return PS3DK_PROF_OK;
    case PSP_P_ENOSPC: return PS3DK_PROF_EFULL;
    case PSP_P_ENOMEM: return PS3DK_PROF_ENOMEM;
    case PSP_P_ESTOPPED: return PS3DK_PROF_ESTOPPED;
    case PSP_P_ETIMEDOUT: return PS3DK_PROF_ETIMEDOUT;
    case PSP_P_EBUSY: return PS3DK_PROF_EBUSY;
    default: return PS3DK_PROF_EINVAL;
    }
}

/* the common entry: state checks before any lock, then the mutex, then the fault check again */
static int enter(prof **pp)
{
    prof *p = p_ready() ? g_prof : NULL;
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (!p->spu)
        return PS3DK_PROF_ENOTSUP; /* no SPU lanes (the extension off, or GPU domains only) */
    if (g_tls.in_drain || g_tls.in_gpu)
        return PS3DK_PROF_EBUSY; /* a sink callback, a drain, or a GPU port callback: refused before locking */
    if (LD_ACQ(&p->ext_fault))
        return PS3DK_PROF_ESTOPPED;
    if (ext_lock(p) != 0)
        return PS3DK_PROF_EBUSY; /* nothing changed */
    if (LD_ACQ(&p->ext_fault)) {
        (void)ext_unlock(p);
        return PS3DK_PROF_ESTOPPED;
    }
    *pp = p;
    return PS3DK_PROF_OK;
}

static int leave(prof *p, int rc)
{
    return ext_unlock(p) == 0 ? rc : PS3DK_PROF_ESTOPPED;
}

/* the admission decision (delta r2 section 3): holding spu_mutex, ext_fault, sealed and the shutdown claim are clear;
 * the mutex stays held through the mutation */
static int admitted(prof *p)
{
    if (LD_ACQ(&p->shutdown_once) || p->ext_sealed)
        return 0;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.spu_admission_pause)
        ps3dk_prof_test_hooks.spu_admission_pause();
#endif
    return 1;
}

int ps3dk_prof_spu_image_register_table(const uint8_t *build_id, uint32_t build_len, const ps3dk_prof_spu_desc *descs,
                                        uint32_t n, uint32_t *image)
{
    prof *p = NULL;
    uint32_t idx;
    int rc = enter(&p);
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (!build_id || !descs || !image)
        return leave(p, PS3DK_PROF_EINVAL);
    if (!admitted(p))
        return leave(p, PS3DK_PROF_ESTOPPED);
    rc = map_rc(psp_image_add(p->spu, build_id, build_len, descs, n, &idx)); /* the same type: no cast */
    if (rc == PS3DK_PROF_OK) {
        int u = leave(p, PS3DK_PROF_OK);
        *image = idx; /* the mutation happened: reported even when the unlock then failed */
        return u;
    }
    return leave(p, rc);
}

/* the ELF descriptor source, with the test hook that fails a chosen get (the copy pass's late-failure cases) */
static int elf_src_get(void *ctx, uint32_t i, psp_desc_rec *out)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    static uint32_t calls;
    if (ps3dk_prof_test_hooks.spu_src_fail_at) {
        if (++calls == ps3dk_prof_test_hooks.spu_src_fail_at) {
            calls = 0;
            ps3dk_prof_test_hooks.spu_src_fail_at = 0;
            return -1;
        }
    } else {
        calls = 0;
    }
#endif
    return psp_elf_get(ctx, i, out);
}

#ifdef PS3DK_PROF_TEST_HOOKS
/* TEST ONLY: the image arena's committed state, for the rollback assertions (no lock: tests call them quiescent) */
size_t ps3dk_prof__test_spu_arena_used(void);
size_t ps3dk_prof__test_spu_arena_len(void);
unsigned ps3dk_prof__test_spu_nimages(void);
int ps3dk_prof__test_spu_image_slot(unsigned i, void *out, size_t n);
size_t ps3dk_prof__test_spu_arena_used(void) { return p_ready() && g_prof->spu ? g_prof->spu->arena_used : 0; }
size_t ps3dk_prof__test_spu_arena_len(void) { return p_ready() && g_prof->spu ? g_prof->spu->arena_len : 0; }
unsigned ps3dk_prof__test_spu_nimages(void) { return p_ready() && g_prof->spu ? g_prof->spu->nimages : 0; }
int ps3dk_prof__test_spu_image_slot(unsigned i, void *out, size_t n)
{
    if (!p_ready() || !g_prof->spu || i >= 32 || n != sizeof(psp_image))
        return -1;
    memcpy(out, &g_prof->spu->images[i], n);
    return 0;
}
#endif

/* raw SPU ELF file bytes (not a sys_spu_image_t), alive and unchanged for the call (design I3b r2) */
int ps3dk_prof_spu_image_register_elf(const void *elf, uint64_t size, uint32_t *image)
{
    prof *p = p_ready() ? g_prof : NULL;
    psp_elf_view v; /* the whole parse context: on the stack, no allocation */
    psp_desc_src src;
    uint32_t idx;
    int rc;
    /* the cheap refusals first, lock-free and only on atomics (ext_sealed is read under spu_mutex only) */
    if (!p)
        return PS3DK_PROF_ENOTINIT;
    if (!p->spu)
        return PS3DK_PROF_ENOTSUP;
    if (g_tls.in_drain || g_tls.in_gpu)
        return PS3DK_PROF_EBUSY;
    if (LD_ACQ(&p->ext_fault) || LD_ACQ(&p->shutdown_once))
        return PS3DK_PROF_ESTOPPED;
    if (!elf || !image)
        return PS3DK_PROF_EINVAL;
    if (psp_elf_parse(elf, size, &v) != 0)
        return PS3DK_PROF_EINVAL;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.spu_elf_after_parse)
        ps3dk_prof_test_hooks.spu_elf_after_parse();
#endif
    rc = enter(&p); /* again: ready, SPU lanes, in_drain, ext_fault, then the mutex */
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (!admitted(p))
        return leave(p, PS3DK_PROF_ESTOPPED);
    src.ctx = &v;
    src.get = elf_src_get;
    rc = map_rc(psp_image_add_src(p->spu, v.build_id, v.build_len, &src, v.n, &idx));
    if (rc == PS3DK_PROF_OK) {
        int u = leave(p, PS3DK_PROF_OK);
        *image = idx; /* the mutation happened: reported even when the unlock then failed (as register_table) */
        return u;
    }
    return leave(p, rc);
}

int ps3dk_prof_spu_lane_create(const char *name, uint32_t image, int owner_self, ps3dk_prof_spu_lane *out)
{
    prof *p = NULL;
    tls_state *t = &g_tls;
    uint32_t idx, inc = 0, slot = 0, gen = 0;
    int rc;
    if (!out || (owner_self != 0 && owner_self != 1))
        return p_ready() ? PS3DK_PROF_EINVAL : PS3DK_PROF_ENOTINIT;
    if (owner_self) { /* the caller's own live incarnation: it cannot retire while this call runs */
        if (!t->slot1 || t->acked || t->retired)
            return p_ready() ? PS3DK_PROF_EINVAL : PS3DK_PROF_ENOTINIT;
        inc = t->incarnation;
        slot = t->slot1 - 1;
        gen = t->generation;
    }
    rc = enter(&p);
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (!admitted(p))
        return leave(p, PS3DK_PROF_ESTOPPED);
    rc = map_rc(psp_lane_create(p->spu, name, image, inc, slot, gen, &idx));
    if (rc == PS3DK_PROF_OK) {
        const psp_lane *l = &p->spu->lanes[idx];
        ps3dk_prof_spu_lane o = {l->lane_id, p->spu->cfg.ring_slots, l->ea_ring, l->ea_ctl};
        int u = leave(p, PS3DK_PROF_OK);
        *out = o; /* with ESTOPPED after a failed unlock: diagnostic ownership only, never drained */
        return u;
    }
    return leave(p, rc);
}

static int find(const prof *p, uint32_t handle, uint32_t *idx)
{
    uint32_t i;
    for (i = 0; i < p->spu->cfg.lanes_max; i++)
        if (p->spu->lanes[i].state != PSP_L_FREE && p->spu->lanes[i].lane_id == handle) {
            *idx = i;
            return 1;
        }
    return 0;
}

static void to_public(const psp_lane_stats *s, ps3dk_prof_spu_lane_stats *o)
{
    memset(o, 0, sizeof *o);
    o->final_written = (uint32_t)s->final_written;
    o->fin_state = s->fin_state;
    o->next_seq = s->next_seq;
    o->events = s->events;
    o->lost = s->lost;
    o->have_post_join = (uint32_t)s->have_post_join;
    o->post_state = s->post_join.state;
    o->post_next_seq = s->post_join.next_seq;
    o->post_older_lost = s->post_join.older_lost;
    o->post_refused = s->post_join.refused;
    o->stale = s->stale;
    o->cancelled = s->cancelled;
    o->unstable = s->unstable;
}

int ps3dk_prof_spu_lane_get_stats(uint32_t handle, ps3dk_prof_spu_lane_stats *st)
{
    prof *p = NULL;
    uint32_t idx;
    psp_lane_stats s;
    ps3dk_prof_spu_lane_stats o;
    int rc;
    if (!st)
        return p_ready() ? PS3DK_PROF_EINVAL : PS3DK_PROF_ENOTINIT;
    rc = enter(&p);
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (!find(p, handle, &idx))
        return leave(p, PS3DK_PROF_EINVAL); /* stale */
    psp_lane_stats_get(p->spu, idx, 0, &s);
    to_public(&s, &o);
    rc = leave(p, PS3DK_PROF_OK);
    if (rc == PS3DK_PROF_OK)
        *st = o; /* a read: its outputs are written only on success */
    return rc;
}

/* one drain on the designated APP thread, under sole-drainer ownership exactly as ps3dk_prof_drain takes it */
static void inline_drain(prof *p)
{
    uint32_t z = 0;
    if (p->cfg.mode != PS3DK_PROF_MODE_APP || !pal_owner_is_self(&p->owner))
        return;
    if (!CAS_ACQREL(&p->drain_busy, &z, 1u))
        return;
    if (!LD_ACQ(&p->stop_latch)) {
        g_tls.in_drain = 1;
        (void)p_drain_once(p);
        g_tls.in_drain = 0;
    }
    ST_REL(&p->drain_busy, 0u);
}

int ps3dk_prof_spu_lane_end(uint32_t handle, uint64_t timeout_us, ps3dk_prof_spu_lane_stats *final_or_null)
{
    prof *p = NULL;
    uint32_t idx;
    uint64_t now, deadline;
    int rc = enter(&p);
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (!find(p, handle, &idx))
        return leave(p, PS3DK_PROF_EINVAL); /* stale or already ended */
    psp_lane_end_request(p->spu, idx);
    rc = leave(p, PS3DK_PROF_OK);
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (pal_mono_now_us(&now) != 0)
        return PS3DK_PROF_ECLOCK; /* the request stays in place */
    deadline = timeout_us > UINT64_MAX - now ? UINT64_MAX : now + timeout_us;
    inline_drain(p);
    for (;;) {
        rc = enter(&p);
        if (rc != PS3DK_PROF_OK)
            return rc;
        if (!find(p, handle, &idx))
            return leave(p, PS3DK_PROF_EINVAL); /* another caller ended it meanwhile */
        if (p->spu->lanes[idx].released) {
            ps3dk_prof_spu_lane_stats o;
            psp_lane_stats s;
            int prc;
            psp_lane_stats_get(p->spu, idx, 1, &s); /* post-join counters, read before the slot is released */
            to_public(&s, &o);
            prc = map_rc(psp_lane_end_poll(p->spu, idx, UINT32_MAX)); /* frees: OK, or ESTOPPED when sealed */
            /* a failed final unlock is terminal (ESTOPPED), but the release already happened: the output describes it */
            rc = ext_unlock_at(p, U_END_FINAL) == 0 ? prc : PS3DK_PROF_ESTOPPED;
            if (final_or_null)
                *final_or_null = o; /* the slot was released: its counters are reported */
            return rc;
        }
        rc = leave(p, PS3DK_PROF_OK);
        if (rc != PS3DK_PROF_OK)
            return rc;
        if (pal_mono_now_us(&now) != 0)
            return PS3DK_PROF_ECLOCK;
        if (now >= deadline)
            return PS3DK_PROF_ETIMEDOUT; /* still charged; call again */
        pal_sleep_us(deadline - now < 1000 ? deadline - now : 1000); /* never holding spu_mutex */
    }
}

/* ---- shared with the GPU domain runtime (src/rsx/gpu_runtime.c) ---- */

int p_ext_lock_at(prof *p) { return ext_lock(p); }
int p_ext_unlock_site(prof *p, uint32_t site) { return ext_unlock_at(p, site); }
void p_ext_inline_drain(prof *p) { inline_drain(p); }

#endif
