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
    uint32_t size;              /* sizeof(ps3dk_prof_auto_options) = 52; 36 and 32, the earlier structs (without the
                                 * ring fields; 32 also without range_pairs), are still accepted */
    uint32_t report_first;      /* local reports, first index 0..2047 */
    uint32_t report_count;      /* even, 18..1024, wholly within 0..2047 */
    uint32_t operation_label;   /* 64 or 69..254; distinct from clock_label */
    uint32_t clock_label;
    uint32_t metadata_records;  /* 1..1024 operation snapshots */
    uint32_t frame_records;     /* 1..256 coverage snapshots */
    uint32_t max_bytes;         /* actual checked layout must fit; <= 2 MiB */
    uint32_t range_pairs;       /* report pairs kept for AUTO ranges; 0 the default (an eighth of the pool, at most
                                 * 64: 63 with the default 1024 reports; with ring blocks, of the legacy pairs they
                                 * leave). A range holds its pair until the drainer has
                                 * read it, so size for the ranges outstanding over the RSX and drainer lag (about
                                 * three frames), not for one frame. At most what leaves 64 pairs for draws and FRAME
                                 * marks after the whole-CALL reserve (32, a quarter of a small pool) and this one */
    /* Report ring (owner-buffer draws recycle report blocks through MAIN memory). RESERVED: all four must be 0 in
     * this release, which refuses any other value; 0 keeps the bounded local report pool. */
    uint32_t ring_blocks;       /* 2 or 3 local blocks of 126 pairs (the hardware-tested copy) at the top of the
                                 * range; the rest stay legacy and must be nonempty, so 3 needs over 378 pairs and 4
                                 * (504 pairs, the whole 1024-report maximum) is always refused */
    uint32_t ring_label;        /* 0: 245; otherwise 64 or 69..254, distinct from both labels above */
    uint32_t ring_bytes;        /* 0: 16 MiB; whole 1 MiB pages up to 256 MiB of MAIN extents */
    uint32_t ring_records;      /* 0: 16384; 1..65536 operation snapshots, replacing metadata_records */
} ps3dk_prof_auto_options;

/* Defaults: reports 1024..2047, operation label 247, clock label 246. All are PROFILER-owned in
 * <cell/gcm/ps3tc_gcm_labels.h> (labels 240..247, reports 0..511 and 1024..2047), so they never collide with the
 * application's labels (64, 69..239, 248..254) or reports (512..1023); the attach refuses defaults that would. */
#define PS3DK_PROF_AUTO_OPTIONS_INIT {52u,1024u,1024u,247u,246u,1024u,256u,2097152u,0u,0u,0u,0u,0u}

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
 *                                      without a pair from the range reserve (range_pairs, default 63) is refused and
 *                                      counted (per frame in its coverage row when the frame has one). Both are int
 *                                      expressions: BEGIN 1 when the range was admitted, END 1 when it was
 *                                      committed, 0 when refused; always 1 without PS3_PROF_AUTO (nothing is
 *                                      refused there, so BEGIN/END pairing logic is the same in both builds). A refused BEGIN still keeps the nesting balanced: issue its
 *                                      END whatever BEGIN returned. name is a string literal (the runtime keeps one
 *                                      descriptor per name/file/line site, 256 sites);
 *  a capture window opened mid-frame (an immediate SECTION or WHOLE_RUN, from any thread): the owner's first draw
 *                                      or range in it opens that frame's coverage row from that moment, with no
 *                                      FRAME marks and always OPEN (its ranges and draws are recorded; the frame
 *                                      gets no whole-frame verdict). Before the owner's first frame there is no
 *                                      frame, so ranges are refused (counted only in the run total);
 *  PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED(name)  the same, and its covered draws get per-draw pairs while
 *  PS3DK_PROF_AUTO_TIMED_RANGES_ONLY() limits per-draw timing to timed ranges (draws outside them are covered but
 *                                      untimed, reason "outside timed range"); without it every draw is timed. */
#if defined(PS3TC_PROF_AUTO) && PS3TC_PROF_AUTO
void ps3dk_prof_auto_tag(uint32_t tag);
void ps3dk_prof_auto_timing(uint32_t mode);
int ps3dk_prof_auto_range_begin(ps3dk_prof_desc *desc, uint32_t timed);
/* BEGIN keyed by its call site: the runtime owns the descriptor (name and file must outlive the capture) */
int ps3dk_prof_auto_range_begin_at(const char *name, const char *file, uint32_t line, uint32_t timed);
int ps3dk_prof_auto_range_end(void);
#define PS3DK_PROF_AUTO_TAG(t) ps3dk_prof_auto_tag((uint32_t)(t))
#define PS3DK_PROF_AUTO_RANGE_BEGIN(name) ps3dk_prof_auto_range_begin_at((name), __FILE__, (uint32_t)__LINE__, 0u)
#define PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED(name) ps3dk_prof_auto_range_begin_at((name), __FILE__, (uint32_t)__LINE__, 1u)
#define PS3DK_PROF_AUTO_RANGE_END() ps3dk_prof_auto_range_end()
#define PS3DK_PROF_AUTO_TIMED_RANGES_ONLY() ps3dk_prof_auto_timing(1u)
#else
/* a call rather than a bare constant, so a range macro used as a statement raises no unused-value warning */
static inline int ps3dk_prof__auto_off(void) { return 1; }
static inline int ps3dk_prof__auto_off_ok(void) { return 0; }
#define PS3DK_PROF_AUTO_TAG(t) ((void)sizeof(t))
#define PS3DK_PROF_AUTO_RANGE_BEGIN(name) ((void)sizeof(name), ps3dk_prof__auto_off())
#define PS3DK_PROF_AUTO_RANGE_BEGIN_TIMED(name) ((void)sizeof(name), ps3dk_prof__auto_off())
#define PS3DK_PROF_AUTO_RANGE_END() ps3dk_prof__auto_off()
#define PS3DK_PROF_AUTO_TIMED_RANGES_ONLY() ((void)0)
#endif

#if defined(PS3TC_PROF_AUTO) && PS3TC_PROF_AUTO
int ps3dk_prof_shutdown(void);
/* ps3dk_prof_shutdown, closing an open capture window with reason SEAL */
#define PS3DK_PROF_AUTO_SEAL() ps3dk_prof_capture_seal()
#else
#define PS3DK_PROF_AUTO_SEAL() 0
#endif

/* Options before cellGcmInit: validated and copied now (the caller's struct need not outlive the call; the earlier
 * 32- and 36-byte structs are accepted as attach accepts them, 32 taking the default range reserve; ring fields are
 * validated as attach validates them), then used by the intercepted cellGcmInit in place
 * of the defaults, so commands written right after init cannot close the window for them. Only an init on the same
 * thread uses them: another thread's init takes the defaults, and the capture then carries the sticky STATUS flag
 * PREINIT_FOREIGN (2048; the reader prints "pre-init options ignored"). The first successful init consumes them; a later call
 * replaces an earlier one. EINVAL for options the attach would refuse, EBUSY once an intercepted cellGcmInit ran,
 * ENOTINIT before profiler start-up. PS3DK_PROF_AUTO_OPTIONS_SET(o) is 0 and calls nothing without PS3_PROF_AUTO.
 * The override below is unchanged: after any command write it still refuses (PRIOR_WRITES_UNKNOWN). */
int ps3dk_prof_auto_options_set(const ps3dk_prof_auto_options *options);
#if defined(PS3TC_PROF_AUTO) && PS3TC_PROF_AUTO
#define PS3DK_PROF_AUTO_OPTIONS_SET(o) ps3dk_prof_auto_options_set(o)
#else
#define PS3DK_PROF_AUTO_OPTIONS_SET(o) ((void)sizeof(o), ps3dk_prof__auto_off_ok())
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
