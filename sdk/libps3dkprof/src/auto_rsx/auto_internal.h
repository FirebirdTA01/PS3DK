#ifndef PS3DK_PROF_AUTO_INTERNAL_H
#define PS3DK_PROF_AUTO_INTERNAL_H
#include "../core/prof_internal.h"
#include "auto_config.h"
#include "auto_writer.h"
#include "auto_queue.h"
#include "auto_completion.h"
#include "auto_frames.h"
#include "auto_clock.h"
#include "auto_ring.h"
#include "auto_ring_commands.h"
#include "auto_ring_layout.h"
#include "../rsx/gpu_port_gcm.h"
#include <ps3dk/prof_auto.h>

/* labels whose complete-run cadence one coverage row tracks separately (label_one); later labels are not credited */
#define AUTO_LABEL_TRACK 8u
#if !defined(PS3DK_PROF_WITH_SPU) || !defined(PS3DK_PROF_WITH_FIFO)
#error "automatic RSX observation requires the extension and FIFO provider"
#endif

#define p_auto_init ps3dk_prof__auto_init
#define p_auto_drain ps3dk_prof__auto_drain
#define p_auto_seal_claim ps3dk_prof__auto_seal_claim
#define p_auto_owner_retire ps3dk_prof__auto_owner_retire
#define p_auto_frame ps3dk_prof__auto_frame

/* Supplied by the mandatory selector link step, before linking. The external
 * execution manifest binds these identities to the final ELF and SELF. */
extern const uint8_t ps3dk_prof__auto_mechanism[32];
extern const uint8_t ps3dk_prof__auto_build_binding[32];
extern const char ps3dk_prof__auto_build_id[];
extern const char ps3dk_prof__auto_app[];
/* Start-up only, before profiler initialization: the build-option ring defaults (PS3_PROF_AUTO_RING). */
int ps3dk_prof__auto_build_ring(const ps3dk_prof_auto_ring_options *ring);

/* Up to 32 distinct segments per capture; their built operations share one pool at least as large as the report-
 * pair pool (report_count <= 1024, 16 for clocks: at most 504 pairs), so report pairs, not the pool, bound how many
 * segment draws can be timed in flight. */
enum { AUTO_SEGMENTS_OPEN=32, AUTO_SEGMENT_POOL=512, AUTO_SEGMENT_NONE=0xffffffffu };
/* Report pairs kept for whole-CALL brackets: covered draws and FRAME marks stop taking pairs when only this many
 * are free (fewer with a small pool: a quarter of it). */
enum { AUTO_CALL_PAIRS=32 };
/* Report pairs kept for AUTO ranges by default: an eighth of the pool, at most 64 (63 for the default 504-pair pool).
 * A range's pair stays held until the drainer has read its completed reports, so the reserve bounds the ranges
 * outstanding across the RSX's lag behind the PPU and the drainer's lag behind the RSX, not one frame's count: about
 * three frames of 19 ranges fit. Per-draw timing keeps its own pairs whatever the timing mode (a timed range times its
 * draws), so the reserve does not depend on PS3DK_PROF_AUTO_TIMED_RANGES_ONLY. Also the nesting limit. */
enum { AUTO_RANGE_PAIRS=64, AUTO_RANGE_DEPTH=4 };
/* Distinct PS3DK_PROF_AUTO_RANGE_BEGIN sites (name, file, line) the runtime keeps a descriptor for. */
enum { AUTO_RANGE_SITES=256 };
/* One covered operation built inside a CALL segment: its report pair is owned
 * from the build; its record is committed at cellGcmSegmentCall (or as an
 * INCOMPLETE record when the segment is rebuilt, never called or called
 * outside the attached ring). */
typedef struct auto_segment_op { auto_wire_op op; uint32_t pair, next; } auto_segment_op;
/* ctx is the segment's identity: its embedded context, which cellGcmSegmentCall passes. writer is the context
 * that emits into it: ctx itself for Begin/End, or, for BeginExternal, the application's context bound by the
 * first covered emitter whose current and end lie inside [lo, hi), the claimed buffer. */
typedef struct auto_segment {
    void *ctx, *writer;
    uintptr_t lo, hi;
    uint32_t ordinal, open, count, external;
    uint32_t head, tail;   /* this build's operations in the pool, in emission order */
    /* This build's history, kept apart from count (which a CALL or a drop clears) and reset only when a new build
     * of the part starts (Begin/BeginExternal): visits are its covered emitters (timed, refused, or moved here
     * from another part), refused those that are not in the pool; both saturate. called: the build was CALLed
     * (or its CALL refused) at least once; reports: a timestamp report was written into its buffer, so a later
     * CALL replays report writes. */
    uint32_t visits, refused, called, reports;
    /* Of the refused: untimed_refused were counted in a coverage frame with an untimed-only exclusion (room, report
     * capacity, metadata capacity, reentry, unsupported arguments), all in untimed_frame unless untimed_mixed;
     * other_refused were refused otherwise or counted in no frame. Reset with the build. */
    uint32_t untimed_refused, other_refused, untimed_mixed;
    uint64_t untimed_frame;
    /* A whole-CALL bracket between cellGcmSegmentCall's pre-CALL hook and its CALL hook: its pair (the begin
     * report is in the ring before the CALL) and the PPU time before that report. */
    uint32_t bracket, bracket_pair;
    uint64_t bracket_tb;
    uint32_t fp;   /* the last fragment-program offset set while building this part (0 none) */
} auto_segment;
/* One open AUTO range (owner only): refused ranges are kept too, so END stays balanced. */
typedef struct auto_range {
    uint32_t ok, pair, desc, timed, tag;
    uint64_t frame, tb;
} auto_range;

/* The MAIN ring's page allocation and its IO mapping: sys_memory_allocate with 1 MiB pages and
 * cellGcmMapMainMemory on LV2 (gpp_ring_memory_gcm), ps3dk_prof_test_hooks.auto_ring_memory in host tests.
 * Both return 0 on success. Nothing is ever freed or unmapped: a mapping that was attempted, successful or not,
 * may be GPU visible, and queue emptiness does not prove the RSX quiescent. */
typedef struct auto_ring_memory {
    int (*allocate)(uint32_t bytes, uintptr_t *ea);
    int (*map)(uintptr_t ea, uint32_t bytes, uint32_t *offset);
} auto_ring_memory;
/* Why recycling stopped (first reason kept): the owner context's callback is not the native FIFO wrap, its end is
 * not the exact one the tail reservation set, the flush could not be emitted (unknown M2MF state or no room), a
 * context reset, or the ring label did not read back zero after its startup reset. A refused or uncertain block is quarantined, never reused.
 * The first three are found before any flush word is stored: they halt new admission only, and blocks sealed earlier
 * still drain and retire. A context reset or a label anomaly stops the ring on both sides. */
enum { AUTO_RING_REFUSED_CALLBACK=1, AUTO_RING_REFUSED_END, AUTO_RING_REFUSED_EMIT, AUTO_RING_REFUSED_RESET,
       AUTO_RING_REFUSED_LABEL };

/* One allocation before observation; variable queues follow this control block
 * at 128-byte boundaries. Prepared ports make no device writes at attachment. */
typedef struct auto_capture {
    pal_owner owner;
    uint32_t owner_slot, owner_incarnation, owner_generation, lease_ticket;
    uint32_t fifo_instance;
    gpp_ctx *context;
    gpp_port operations_port, clock_port;
    prg_port operations_device, clock_device;
    auto_queue operations;
    auto_publication publication;
    auto_completion completion; /* drainer only */
    auto_frames coverage;
    auto_queue_entry *active; /* owner only, between canonical begin/end */
    uintptr_t expected_end;
    uint32_t iso_active;
    uint32_t open_ended;     /* the active draw is immediate (SetDrawBegin): its words end at SetDrawEnd */     /* RSX frame capture ISOLATED: 4 words of WAIT_FOR_IDLE in the active draw (0 otherwise) */
    uint32_t device_initialized, saturated, owner_done;
    /* owner only: open segment contexts (cellGcmSegmentBegin..End) and the
     * covered visits on contexts other than the attached ring */
    auto_segment segments[AUTO_SEGMENTS_OPEN];
    auto_segment_op segment_pool[AUTO_SEGMENT_POOL];
    uint32_t segment_free, segment_pending; /* free list head; the operation between a segment begin and end */
    /* owner only: once report pairs or metadata run out in a coverage frame, every later covered draw of that
     * frame is excluded with that column too, so the untimed draws are exactly the frame's tail */
    uint32_t pool_tail_column;
    uint32_t segments_used, segment_ordinal, call_ordinal, segment_quarantine;
    uint32_t segment_replay; /* sticky: a build with report writes was CALLed again (STATUS SEGMENT_REPLAY) */
    auto_segment *segment_active; /* between a segment emitter's begin and end */
    void *segment_active_writer;  /* that emitter's context: kept here, not on the segment, which a part close clears */
    uint64_t segment_visits, foreign_visits, segment_table_full, frame_marks_missed;
    uint64_t call_brackets, call_brackets_refused; /* whole-CALL brackets begun; CALLs that needed one and got none */
    uint32_t admit_column; /* the column segment_admit counted its last refusal in (0: counted in no frame) */
    /* owner only: the current tag (PS3DK_PROF_AUTO_TAG), the attached ring's last fragment-program offset, the timing
     * mode (0 every covered draw, 1 only inside timed ranges), the open ranges and the depth beyond the stack */
    uint32_t tag, fp_ring, timing, timed_open, range_depth, range_excess;
    auto_range ranges[AUTO_RANGE_DEPTH];
    uint64_t ranges_done, ranges_refused;
    /* owner only: frame source. A flip opens frames until an explicit
     * ps3dk_prof_frame is seen; from then on explicit calls drive and flips are
     * ignored (counted). Both kinds are reported in STATUS. */
    uint32_t in_flip_frame, explicit_seen;
    uint64_t frames_by_flip, frames_by_explicit, flips_ignored;
    /* owner only: presentation observation. A cadence run is a sequence of
     * consecutive pending flip-status polls whose gaps are each within
     * cadence_bound ticks (polling cadence, never waiting evidence). A
     * flip-wait scope is one cellGcmWaitFlipComplete call (waiting evidence). */
    uint32_t cadence_open, flip_wait_open;
    /* the open run or scope crossed a frame boundary: it belongs to no frame */
    uint32_t cadence_spanning, flip_wait_spanning;
    uint64_t cadence_last, cadence_ticks, cadence_bound, flip_wait_since;
    /* owner only: label-poll cadence, the same rules for cellGcmIsLabelPassed reads of one label (label_at) */
    uint32_t label_open, label_spanning, label_at;
    uint64_t label_last, label_ticks;
    /* owner only: the open row's per-label totals of complete runs (label_row = its frame + 1; at most
     * AUTO_LABEL_TRACK labels, later ones are not credited) */
    uint64_t label_row, label_sum[AUTO_LABEL_TRACK];
    uint32_t label_n, label_id[AUTO_LABEL_TRACK];
    uint64_t admitted, unassociated, internal_suppressed, conflicts;
    auto_wire_frame *frames;
    uint32_t frame_capacity;
    auto_clock clocks;
    uint32_t clock_put_frontier; /* owner-only snapshot around original PUT */
    uint64_t allocated;
    uint32_t owner_declared, domain_declared, epoch_declared, final_declared;
    /* REPORT_RING, attached when ring_plan.blocks is nonzero. ring_ea is the page allocation: [0, bytes) IO mapped at
     * ring_offset (the MAIN extents), then the CPU-only metadata entries and descriptors (ring_plan). */
    auto_ring_plan ring_plan;
    auto_ring ring;
    uintptr_t ring_ea;
    uint32_t ring_offset, ring_label;
    gpp_port ring_port;
    prg_port ring_device;
    /* owner only: the physical tail reserved while a block fills, the application M2MF shadow and its scan cursor
     * (the next owner word not yet scanned; profiler-injected flush packets are never scanned) */
    auto_ring_tail tail;
    auto_ring_shadow shadow;
    uintptr_t scan;
    /* atomic: owner work requested by a foreign closer (the sealing drainer); the owner settles its open block at
     * its next hook. Foreign closers and the drainer never emit. */
    uint32_t ring_flush_request;
    uint32_t ring_refusal;       /* AUTO_RING_REFUSED_*, the first one */
    uint64_t ring_flushes, ring_quarantined, ring_injected_words;
    /* The ring's own PUT frontier: the owner commits 2g after a block's complete flush, publish_before snapshots
     * it before PUT (ring_put_snapshot, owner only) and publish_after releases it; the drainer acquires it. */
    auto_publication ring_publication;
    uint32_t ring_put_snapshot;
    /* owner stores (release), drainer loads: copied bytes, flushes that found the application's M2MF state untouched
     * or restored it, application words decoded (STATUS ring tail) */
    uint64_t ring_copied, ring_untouched, ring_restored, ring_decoded;
    /* owner stores once, drainer loads: the decoder's first rejected header word, its byte offset from the context
     * begin, then (release) its AUTO_RING_REJECT_* reason (STATUS ring tail u32 at 192..200) */
    uint32_t ring_reject[3];
} auto_capture;

void p_auto_init(prof *p); /* before READY/start_gate publication */
void p_auto_drain(prof *p, int seal); /* sole encoder, core drain serialization */
void p_auto_seal_claim(void); /* before quiescence and every sink check */
void p_auto_owner_retire(void); /* before the core owner's final acknowledgement */
void p_auto_frame(uint64_t frame,uint64_t tb); /* registered GCM owner only */
#endif
