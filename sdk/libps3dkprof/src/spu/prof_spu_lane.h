/* The SPU side of a profiler lane (design I3 r2 section 2): INTERNAL, not installed; SPU builds only.
 *
 * The PPU creates a lane (ps3dk_prof_spu_lane_create) and passes its ea_ring / ring_slots / ea_ctl to the SPU program
 * (for example as thread arguments); the SPU attaches a caller-owned writer to it.
 *   - The tag is the caller's (0..31); the library issues DMA only on it and restores the tag mask it found.
 *   - Observe-only by default: the decrementer is only read.  It is written only by an explicit
 *     ps3dk_prof_spu_dec_write on a writer attached with PS3DK_PROF_SPU_OWN_DEC.
 *   - Nothing flushes on its own: events past ls_cap are counted losses until the program flushes.
 *   - The writer (128-aligned) and the staging buffer (16-aligned, PS3DK_PROF_SPU_STAGING_BYTES) are caller memory,
 *     two objects that must not overlap (attach refuses an overlap);
 *     the library allocates nothing and has no static state.  One writer per SPU thread, not reentrant (never from
 *     an SPU event/interrupt handler).
 * Zones: PS3DK_PROF_SPU_ZONE_BEGIN(w, "name") ... PS3DK_PROF_SPU_ZONE_END(w, z), PS3DK_PROF_SPU_MARK(w, "name").
 * Each use defines one 16-byte descriptor record in section ps3dkprof_spudesc; its index is integer LS arithmetic
 * against __start_ps3dkprof_spudesc, checked by the same wrapper the direct calls use. */
#ifndef PS3DK_PROF_SPU_LANE_H
#define PS3DK_PROF_SPU_LANE_H

#include <stdint.h>

#define PS3DK_PROF_SPU_OK 0
#define PS3DK_PROF_SPU_EBUSY (-2)    /* an event the library must not consume is enabled; retry after disabling it */
#define PS3DK_PROF_SPU_EINVAL (-5)   /* an argument, alignment, flag or the descriptor section is invalid */
#define PS3DK_PROF_SPU_ECLOSED (-15) /* the lane was already attached once, or this writer is closed */
#define PS3DK_PROF_SPU_ENOTSUP (-17) /* dec_write without OWN_DEC; also every macro in an off build */
#define PS3DK_PROF_SPU_EAGAIN (-20)  /* dec_write: earlier events are not all published yet; flush and retry */
#define PS3DK_PROF_SPU_EFAULT (-21)  /* terminal: an MFC status was wrong, or an invalid descriptor index was used */
#define PS3DK_PROF_SPU_EEXHAUSTED (-22) /* flush: the generation/write counters are spent; only fini's CLOSED remains */
#define PS3DK_PROF_SPU_EDEPTH (-23) /* dec_write inside a zone (or with suppressed BEGINs): refused before any flush,
                                    * write or publication; NOT retryable by flushing (end the zones first) */

#define PS3DK_PROF_SPU_OWN_DEC 1u
#define PS3DK_PROF_SPU_STAGING_BYTES(ls_cap) (((uint32_t)(ls_cap) + 8u) * 8u)
#define PS3DK_PROF_SPU_WRITER_ALIGN 128u

#if defined(PS3DK_PROF) && PS3DK_PROF

#if !defined(__SPU__) && !defined(PS3DK_PROF_SPU_HOST_TEST)
#error "prof_spu_lane.h is the SPU side; PPU code uses prof_spu.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include "psp_desc.h"
#include "psp_spu.h"

/* caller-allocatable: declare it with __attribute__((aligned(PS3DK_PROF_SPU_WRITER_ALIGN))) or in 128-aligned memory */
typedef struct ps3dk_prof_spu_writer {
    psp_spu s;
    uint32_t desc_count; /* records in the image's descriptor section, set by a successful attach */
    uint32_t bad_index;  /* invalid descriptor indices seen (the first one latched EFAULT) */
} ps3dk_prof_spu_writer;

/* EINVAL before any write to w or any channel command: NULL or misaligned w/staging, tag > 31, ls_cap not even in
 * 4..4096, ring_slots not a power of two in 8..2^20, ea_ring/ea_ctl not 128-aligned, unknown flags, a staging
 * buffer overlapping the writer, or a descriptor
 * section that is missing, misaligned, not a multiple of 16 bytes or above 4096 records.  Otherwise the writer is
 * initialised: OK; EBUSY (blocking events enabled: nothing published, attach may be retried on the same writer);
 * ECLOSED (the lane already had a producer: not retryable); EFAULT (an MFC status was wrong: not retryable). */
int ps3dk_prof_spu_attach(ps3dk_prof_spu_writer *w, uint64_t ea_ring, uint32_t ring_slots, uint64_t ea_ctl,
                          uint32_t tag, void *staging, uint32_t ls_cap, uint32_t flags);
/* desc_index is checked against the attached count BEFORE narrowing: an index >= desc_count records nothing,
 * counts bad_index and latches EFAULT (later events are not admitted; flush/fini return EFAULT). */
void ps3dk_prof_spu_begin(ps3dk_prof_spu_writer *w, uint32_t desc_index);
void ps3dk_prof_spu_end(ps3dk_prof_spu_writer *w, uint32_t desc_index);
void ps3dk_prof_spu_mark(ps3dk_prof_spu_writer *w, uint32_t desc_index);
int ps3dk_prof_spu_flush(ps3dk_prof_spu_writer *w);
int ps3dk_prof_spu_fini(ps3dk_prof_spu_writer *w);
/* EDEPTH first (a zone is open or a BEGIN was suppressed: nothing happens), then ENOTSUP without OWN_DEC; then a
 * flush, the preflight (EAGAIN: earlier events not all published, retry after a flush), the write and its EPOCH. */
int ps3dk_prof_spu_dec_write(ps3dk_prof_spu_writer *w, uint32_t v);

extern const char __start_ps3dkprof_spudesc[] __attribute__((weak));
extern const char __stop_ps3dkprof_spudesc[] __attribute__((weak));

#ifdef __cplusplus
}
#endif

/* the record's index: unsigned LS-address difference, never a pointer subtraction between distinct objects */
#define PS3DK_PROF__SPU_INDEX(rec)                                                                                    \
    (((uint32_t)(uintptr_t)(rec) - (uint32_t)(uintptr_t)__start_ps3dkprof_spudesc) >> 4)

#define PS3DK_PROF__SPU_RECORD(var, name)                                                                             \
    static const struct ps3dk_prof_spu_desc var                                                                       \
        __attribute__((section("ps3dkprof_spudesc"), aligned(16), used)) = {                                          \
            (uint32_t)(uintptr_t)&var, (uint32_t)__LINE__, (name), __FILE__}

/* a zone: BEGIN yields the index END uses (a uint32_t expression statement) */
#define PS3DK_PROF_SPU_ZONE_BEGIN(w, name)                                                                            \
    __extension__({                                                                                                   \
        PS3DK_PROF__SPU_RECORD(ps3dk_prof__rec, name);                                                                \
        uint32_t ps3dk_prof__i = PS3DK_PROF__SPU_INDEX(&ps3dk_prof__rec);                                             \
        ps3dk_prof_spu_begin((w), ps3dk_prof__i);                                                                     \
        ps3dk_prof__i;                                                                                                \
    })
#define PS3DK_PROF_SPU_ZONE_END(w, z) ps3dk_prof_spu_end((w), (z))
#define PS3DK_PROF_SPU_MARK(w, name)                                                                                  \
    do {                                                                                                              \
        PS3DK_PROF__SPU_RECORD(ps3dk_prof__rec, name);                                                                \
        ps3dk_prof_spu_mark((w), PS3DK_PROF__SPU_INDEX(&ps3dk_prof__rec));                                            \
    } while (0)

#else /* off build: nothing runs and no argument is evaluated */

typedef struct ps3dk_prof_spu_writer {
    uint8_t unused;
} ps3dk_prof_spu_writer;
#define PS3DK_PROF_SPU_ATTACH(w, er, rs, ec, tag, st, cap, fl)                                                        \
    ((void)sizeof(w), (void)sizeof(er), (void)sizeof(rs), (void)sizeof(ec), (void)sizeof(tag), (void)sizeof(st),      \
     (void)sizeof(cap), (void)sizeof(fl), PS3DK_PROF_SPU_ENOTSUP)
#define PS3DK_PROF_SPU_ZONE_BEGIN(w, name) ((void)sizeof(w), (void)sizeof(name), 0u)
#define PS3DK_PROF_SPU_ZONE_END(w, z) ((void)sizeof(w), (void)sizeof(z))
#define PS3DK_PROF_SPU_MARK(w, name) ((void)sizeof(w), (void)sizeof(name))
#define PS3DK_PROF_SPU_FLUSH(w) ((void)sizeof(w), PS3DK_PROF_SPU_ENOTSUP)
#define PS3DK_PROF_SPU_FINI(w) ((void)sizeof(w), PS3DK_PROF_SPU_ENOTSUP)
#define PS3DK_PROF_SPU_DEC_WRITE(w, v) ((void)sizeof(w), (void)sizeof(v), PS3DK_PROF_SPU_ENOTSUP)

#endif

#if defined(PS3DK_PROF) && PS3DK_PROF
#define PS3DK_PROF_SPU_ATTACH(w, er, rs, ec, tag, st, cap, fl) ps3dk_prof_spu_attach((w), (er), (rs), (ec), (tag), (st), (cap), (fl))
#define PS3DK_PROF_SPU_FLUSH(w) ps3dk_prof_spu_flush((w))
#define PS3DK_PROF_SPU_FINI(w) ps3dk_prof_spu_fini((w))
#define PS3DK_PROF_SPU_DEC_WRITE(w, v) ps3dk_prof_spu_dec_write((w), (v))
#endif

#endif
