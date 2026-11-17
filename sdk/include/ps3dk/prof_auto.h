/* Opt-in automatic RSX observation. PS3_PROF_AUTO selects its startup and GCM
 * provider; application code does not need to call attach. These optional
 * owner APIs change defaults or explicitly close an associated coverage frame.
 * Draw/clear marker brackets describe reached-marker elapsed intervals. They
 * do not measure isolated execution time, GPU utilization or starvation. */
#ifndef PS3DK_PROF_AUTO_H
#define PS3DK_PROF_AUTO_H
#include <stdint.h>
#include <ps3dk/prof.h>
#include <ps3dk/prof_gpu.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3dk_prof_auto_options {
    uint32_t size;              /* sizeof(ps3dk_prof_auto_options) = 36 (32: the earlier struct, without range_pairs) */
    uint32_t report_first;      /* local reports, first index 0..2047 */
    uint32_t report_count;      /* even, 18..1024, wholly within 0..2047 */
    uint32_t operation_label;   /* 64 or 69..254; distinct from clock_label */
    uint32_t clock_label;
    uint32_t metadata_records;  /* 1..1024 operation snapshots */
    uint32_t frame_records;     /* 1..256 coverage snapshots */
    uint32_t max_bytes;         /* actual checked layout must fit; <= 2 MiB */
    uint32_t range_pairs;       /* report pairs kept for AUTO ranges; 0 the default (16, an eighth of a small pool);
                                 * at most what leaves 64 pairs for draws and FRAME marks after the whole-CALL reserve
                                 * (32, a quarter of a small pool) and this one; larger is refused */
} ps3dk_prof_auto_options;

/* Defaults: reports 1024..2047, operation label 247, clock label 246. All are PROFILER-owned in
 * <cell/gcm/ps3tc_gcm_labels.h> (labels 240..247, reports 0..511 and 1024..2047), so they never collide with the
 * application's labels (64, 69..239, 248..254) or reports (512..1023); the attach refuses defaults that would. */
#define PS3DK_PROF_AUTO_OPTIONS_INIT {36u,1024u,1024u,247u,246u,1024u,256u,2097152u,0u}

/* Extra command words the automatic profiler writes inside a CALL segment around each covered draw or clear
 * (one 2-word timestamp report before it, one after it), whether the segment is written through
 * cellGcmSegmentBegin's context or, after cellGcmSegmentBeginExternal, through the application's own context.
 * The profiler checks them against the context's end before the draw and never calls the context's callback:
 * a draw without that room is emitted untimed and counted as excluded for room. A writer that admits its
 * reservations without callbacks adds this to each covered draw's bound. Zero when PS3_PROF_AUTO is off. */
#if defined(PS3TC_PROF_AUTO) && PS3TC_PROF_AUTO
#define PS3DK_PROF_AUTO_SEGMENT_DRAW_WORDS 4u
#else
#define PS3DK_PROF_AUTO_SEGMENT_DRAW_WORDS 0u
#endif

/* Sealing the capture on an exit path that skips atexit handlers (_exit, a quit-guard deadline): call
 * PS3DK_PROF_AUTO_SEAL() on the owner thread (the thread that initialised the profiler) just before it. It is
 * ps3dk_prof_shutdown(): EPERM on another thread, EBUSY inside a profiler callback, EALREADY when already shut down,
 * so calling it on every exit path is safe; it never waits on the RSX (pending draws are recorded incomplete, pending
 * clock anchors get one last poll). Zero and no call when PS3_PROF_AUTO is off. */
/* Attribution and AUTO-owned ranges (owner thread; inert elsewhere and without PS3_PROF_AUTO):
 *  PS3DK_PROF_AUTO_TAG(t)              every later operation records tag t (a segment draw: the tag at its build;
 *                                      a whole-CALL record: the tag at the CALL);
 *  PS3DK_PROF_AUTO_RANGE_BEGIN(name)   a range timed by its own report pair in the ring: the RSX time between BEGIN
 *  PS3DK_PROF_AUTO_RANGE_END()         and END, independent of per-draw timing. Nesting to depth 4; deeper, a
 *                                      range inside a segment build, one whose END falls in another frame, or one
 *                                      without a pair from the range reserve (16) is refused and counted;
 *  PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED(name)  the same, and its covered draws get per-draw pairs while
 *  PS3DK_PROF_AUTO_TIMED_RANGES_ONLY() limits per-draw timing to timed ranges (draws outside them are covered but
 *                                      untimed, reason "outside timed range"); without it every draw is timed. */
#if defined(PS3TC_PROF_AUTO) && PS3TC_PROF_AUTO
void ps3dk_prof_auto_tag(uint32_t tag);
void ps3dk_prof_auto_timing(uint32_t mode);
int ps3dk_prof_auto_range_begin(ps3dk_prof_desc *desc, uint32_t timed);
int ps3dk_prof_auto_range_end(void);
#define PS3DK_PROF_AUTO__RANGE(name, timed) \
    do { static ps3dk_prof_desc ps3dk_prof__r = {(name), "", __FILE__, (uint32_t)__LINE__, 2u, 0u, 0u}; \
         (void)ps3dk_prof_auto_range_begin(&ps3dk_prof__r, (timed)); } while (0)
#define PS3DK_PROF_AUTO_TAG(t) ps3dk_prof_auto_tag((uint32_t)(t))
#define PS3DK_PROF_AUTO_RANGE_BEGIN(name) PS3DK_PROF_AUTO__RANGE(name, 0u)
#define PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED(name) PS3DK_PROF_AUTO__RANGE(name, 1u)
#define PS3DK_PROF_AUTO_RANGE_END() ((void)ps3dk_prof_auto_range_end())
#define PS3DK_PROF_AUTO_TIMED_RANGES_ONLY() ps3dk_prof_auto_timing(1u)
#else
#define PS3DK_PROF_AUTO_TAG(t) ((void)sizeof(t))
#define PS3DK_PROF_AUTO_RANGE_BEGIN(name) ((void)sizeof(name))
#define PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED(name) ((void)sizeof(name))
#define PS3DK_PROF_AUTO_RANGE_END() ((void)0)
#define PS3DK_PROF_AUTO_TIMED_RANGES_ONLY() ((void)0)
#endif

#if defined(PS3TC_PROF_AUTO) && PS3TC_PROF_AUTO
int ps3dk_prof_shutdown(void);
/* ps3dk_prof_shutdown, closing an open capture window with reason SEAL */
#define PS3DK_PROF_AUTO_SEAL() ps3dk_prof_capture_seal()
#else
#define PS3DK_PROF_AUTO_SEAL() 0
#endif

/* Optional override, on the registered context owner before its first covered
 * operation, clock or frame observation. NULL options select the defaults.
 * The common SDK registry reserves the whole range and both labels; public
 * SDK conflicts stop observation while preserving the application command.
 * Raw/private command writers must honor these reservations and are outside
 * observable coverage. Invalid overrides leave the existing default intact.
 * No second instance, replacement CONFIG or serial reset is permitted.
 * Both outputs are zero on refusal. No allocation, marker or flush on a
 * refused option validation. Shutdown owns finalization and report quarantine;
 * there is no detach that can free still-referenced reports. */
int ps3dk_prof_auto_attach(ps3dk_prof_gcm_ctx *context, const ps3dk_prof_auto_options *options,
                          uint32_t *instance, uint32_t *domain);

/* Closes only this owner's active matching frame. It does not wait for the
 * RSX or prove GPU completion. Missing/foreign/duplicate closure refuses;
 * Reserved token fields must be zero. Opening a successor with ps3dk_prof_frame leaves an unclosed predecessor
 * explicitly open/unknown. A token's owner==0 is NONE; frame id zero is valid. */
int ps3dk_prof_auto_frame_end(uint32_t instance, ps3dk_prof_token frame);

#ifdef __cplusplus
}
#endif
#endif
