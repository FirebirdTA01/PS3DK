/* The SPU writer API over the producer core (design I3 r2 section 2).  SPU builds only. */
#include "prof_spu_lane.h"

#include <stddef.h>

#if defined(__SPU__)
_Static_assert(sizeof(struct ps3dk_prof_spu_desc) == 16, "a descriptor record is 16 bytes on SPU");
#define WRITER_PORT NULL /* the SPU core binds its port at compile time */
#else /* PS3DK_PROF_SPU_HOST_TEST: the writer's checks over the host port table */
extern const psp_port *ps3dk_prof__spu_host_port;
#define WRITER_PORT ps3dk_prof__spu_host_port
#endif
_Static_assert(_Alignof(ps3dk_prof_spu_writer) == PS3DK_PROF_SPU_WRITER_ALIGN, "the writer holds two 128-byte lines");
_Static_assert(PS3DK_PROF_SPU_STAGING_BYTES(4) == (4u + 2u * PSP_HEAD) * PSP_SLOT, "staging = (ls_cap + 2 heads) slots");

static int map_rc(int rc)
{
    switch (rc) {
    case PSP_OK: return PS3DK_PROF_SPU_OK;
    case PSP_EBUSY: return PS3DK_PROF_SPU_EBUSY;
    case PSP_EAGAIN: return PS3DK_PROF_SPU_EAGAIN;
    case PSP_ECLOSED: return PS3DK_PROF_SPU_ECLOSED;
    case PSP_ENOTSUP: return PS3DK_PROF_SPU_ENOTSUP;
    case PSP_EINVAL: return PS3DK_PROF_SPU_EINVAL;
    case PSP_EDEPTH: return PS3DK_PROF_SPU_EDEPTH;
    default: return PS3DK_PROF_SPU_EFAULT; /* PSP_EFAULT and anything unexpected */
    }
}

/* the image's descriptor section: 0 when it is missing or malformed */
static uint32_t section_count(void)
{
    uint32_t a = (uint32_t)(uintptr_t)__start_ps3dkprof_spudesc, b = (uint32_t)(uintptr_t)__stop_ps3dkprof_spudesc;
    if (!a || (a & 15u) || b < a || ((b - a) & 15u))
        return 0;
    return (b - a) >> 4 <= 4096u ? (b - a) >> 4 : 0;
}

int ps3dk_prof_spu_attach(ps3dk_prof_spu_writer *w, uint64_t ea_ring, uint32_t ring_slots, uint64_t ea_ctl,
                          uint32_t tag, void *staging, uint32_t ls_cap, uint32_t flags)
{
    uint32_t count;
    int rc;
    /* every check before w is written or a channel command is issued */
    if (!w || ((uintptr_t)w & (PS3DK_PROF_SPU_WRITER_ALIGN - 1u)) || !staging || ((uintptr_t)staging & 15u) ||
        tag > 31u || ls_cap < 4u || ls_cap > 4096u || (ls_cap & 1u) || ring_slots < 8u || ring_slots > (1u << 20) ||
        (ring_slots & (ring_slots - 1u)) || (ea_ring & 127u) || (ea_ctl & 127u) || (flags & ~PS3DK_PROF_SPU_OWN_DEC))
        return PS3DK_PROF_SPU_EINVAL;
    {   /* the writer and the staging buffer are distinct objects (the ordering evidence relies on it); the
         * distances are differences of the two addresses, so no address + size is ever formed */
        uintptr_t wa = (uintptr_t)w, sa = (uintptr_t)staging;
        if (sa >= wa ? sa - wa < sizeof *w : wa - sa < PS3DK_PROF_SPU_STAGING_BYTES(ls_cap))
            return PS3DK_PROF_SPU_EINVAL;
    }
    count = section_count();
    if (!count)
        return PS3DK_PROF_SPU_EINVAL;
    /* observe-only: the core reads the decrementer; OWN_DEC only permits a later explicit dec_write */
    rc = psp_spu_init(&w->s, WRITER_PORT, ea_ring, ring_slots, ea_ctl, tag, (uint8_t *)staging, ls_cap,
                      (flags & PS3DK_PROF_SPU_OWN_DEC) != 0);
    w->desc_count = rc == PSP_OK ? count : 0u;
    w->bad_index = 0;
    return map_rc(rc);
}

/* the defined failure for an invalid index: nothing recorded, counted, and the writer latched terminal exactly as
 * the core's fault() does (FAULT flag, terminal, flush phase idle), so no falsely identified event is ever emitted */
static int index_ok(ps3dk_prof_spu_writer *w, uint32_t i)
{
    if (i < w->desc_count)
        return 1;
    if (w->bad_index != UINT32_MAX)
        w->bad_index++;
    w->s.flags |= PSP_F_FAULT;
    w->s.terminal = 1;
    w->s.phase = PSP_FL_IDLE;
    return 0;
}

void ps3dk_prof_spu_begin(ps3dk_prof_spu_writer *w, uint32_t desc_index)
{
    if (index_ok(w, desc_index))
        psp_spu_begin(&w->s, (uint16_t)desc_index);
}

void ps3dk_prof_spu_end(ps3dk_prof_spu_writer *w, uint32_t desc_index)
{
    if (index_ok(w, desc_index))
        psp_spu_end(&w->s, (uint16_t)desc_index);
}

void ps3dk_prof_spu_mark(ps3dk_prof_spu_writer *w, uint32_t desc_index)
{
    if (index_ok(w, desc_index))
        psp_spu_mark(&w->s, (uint16_t)desc_index);
}

int ps3dk_prof_spu_flush(ps3dk_prof_spu_writer *w)
{
    int rc = psp_spu_flush(&w->s);
    return rc == 7 /* the core's PSP_ENOSPC: counters exhausted, events become losses */ ? PS3DK_PROF_SPU_EEXHAUSTED
                                                                                          : map_rc(rc);
}

int ps3dk_prof_spu_fini(ps3dk_prof_spu_writer *w) { return map_rc(psp_spu_fini(&w->s)); }

int ps3dk_prof_spu_dec_write(ps3dk_prof_spu_writer *w, uint32_t v) { return map_rc(psp_spu_dec_write(&w->s, v)); }
