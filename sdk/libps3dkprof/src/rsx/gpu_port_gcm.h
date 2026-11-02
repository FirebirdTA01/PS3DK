/* P3.3 R3: the GCM device binding of the prg_port DEVICE operations (binding contract r2, codex GO 17:08).  The
 * locks, the clock and the hold stay the runtime's; this file binds only reserve_pair, flush, read/write_label,
 * read/write_timer and sync.  NOT wired into the runtime in this slice, and not part of any archive.  ILP32 only
 * (LP64 is unsupported by this binding).  A1 (report visible before its label) and A2 (coherent report read) remain
 * assumptions; the encoding below is the CHOSEN one (the cellGcm QUERY_GET form), not a hardware-confirmed one.
 *
 * Contract.  gpp_prepare validates and caches every device address BEFORE the core may poison anything through it,
 * and makes no device write; a failed prepare leaves the port zeroed (not ready), and every op of a not-ready port
 * refuses without device access.  The ops then use only the cached addresses; that they never see a bad mapping
 * holds only after a successful prepare and while the app keeps the lent mapping alive.  The app lends an EXCLUSIVE
 * report range and label, against every other context and library.  Releasing the lent resources must follow the
 * runtime's quiescence/retirement contract (when the wiring comes); gpp_release alone does not make the GPU quiescent.
 *
 * reserve_pair reserves exactly 6 words itself (not ps3tc_gcm_reserve): the context's begin/current/end are
 * validated as integers (non-NULL, 4-byte aligned, ordered) before any arithmetic; with too little room the context's
 * callback is called ONCE for 6 words, then the context is re-read and revalidated.  A refusal emits no profiler
 * pair and leaves current unadvanced by it; callback side effects (published app commands, a rewritten context)
 * cannot be rolled back.  begin/current/end pointing into one live command buffer owned by the calling ctx owner
 * thread is the caller's contract.  The callback may block without a bound and may trap (as the app's own emission). */
#ifndef GPU_PORT_GCM_H
#define GPU_PORT_GCM_H

#include <stdint.h>

#include "prg.h"

#define gpp_prepare ps3dk_prof__gpp_prepare
#define gpp_release ps3dk_prof__gpp_release
#define gpp_bind ps3dk_prof__gpp_bind
#define gpp_dev_gcm ps3dk_prof__gpp_dev_gcm

#ifdef PS3DK_PROF_PAL_LV2
#include <cell/gcm.h>
typedef CellGcmContextData gpp_ctx;
#else
/* the host stand-in: the same four fields and callback shape as the SDK's context */
typedef struct gpp_ctx gpp_ctx;
struct gpp_ctx {
    uint32_t *begin, *end, *current;
    int32_t (*callback)(gpp_ctx *context, uint32_t count);
};
#endif

#define GPP_REPORTS 2048u         /* local report indices 0..2047 */
#define GPP_CACHE_MAX PRG_SLOTS_MAX /* report addresses the port caches */
#define GPP_LABEL_USER_FIRST 64u
#define GPP_LABEL_LAST 255u
#define GPP_REPORT_STRIDE 16u     /* {u64 timer; u32 value; u32 zero} */
#define GPP_PAIR_WORDS 6u

/* the device accessors: the GCM ones on target (gpp_dev_gcm), fakes in host tests */
typedef struct gpp_dev {
    void *(*report_addr)(uint32_t index); /* the LOCAL report entry `index` */
    void *(*label_addr)(uint32_t index);  /* label `index` (already checked <= 255) */
    void (*flush)(gpp_ctx *ctx);
} gpp_dev;

typedef struct gpp_port {
    uint32_t ready; /* published last by gpp_prepare; 0: every op refuses */
    uint32_t report_first, report_count, label;
    gpp_ctx *ctx;
    const gpp_dev *dev;
    volatile uint32_t *label_word;
    volatile uint64_t *timer[GPP_CACHE_MAX];
} gpp_port;

/* 0, or PRG_EINVAL with *pc zeroed.  Refused: NULL arguments; an invalid context; report_count 0 or above
 * GPP_CACHE_MAX; a range past GPP_REPORTS; a label outside 64..255 or reserved by our libraries (65..68, 255); any
 * mapping NULL, misaligned, off the 16-byte stride or overflowing.  No device write in any case. */
int gpp_prepare(gpp_port *pc, const gpp_dev *dev, gpp_ctx *ctx, uint32_t report_first, uint32_t report_count,
                uint32_t label);
void gpp_release(gpp_port *pc);
/* fills the device ops of *p (ctx = pc); the runtime's fields (mftb, now_us, locks, emittable) are left untouched */
void gpp_bind(prg_port *p, gpp_port *pc);

#ifdef PS3DK_PROF_PAL_LV2
extern const gpp_dev gpp_dev_gcm;
#endif

#endif
