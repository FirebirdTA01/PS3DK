#ifndef PS3DK_PROF_AUTO_INTERNAL_H
#define PS3DK_PROF_AUTO_INTERNAL_H
#include "../core/prof_internal.h"
#include "auto_config.h"
#include "auto_writer.h"
#include "auto_queue.h"
#include "auto_completion.h"
#include "auto_frames.h"
#include "auto_clock.h"
#include "../rsx/gpu_port_gcm.h"
#include <ps3dk/prof_auto.h>
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

enum { AUTO_SEGMENTS_OPEN=8, AUTO_SEGMENT_OPS=32 };
/* One covered operation built inside a CALL segment: its report pair is owned
 * from the build; its record is committed at cellGcmSegmentCall (or as an
 * INCOMPLETE record when the segment is rebuilt, never called or called
 * outside the attached ring). */
typedef struct auto_segment_op { auto_wire_op op; uint32_t pair; } auto_segment_op;
/* ctx is the segment's identity: its embedded context, which cellGcmSegmentCall passes. writer is the context
 * that emits into it: ctx itself for Begin/End, or, for BeginExternal, the application's context bound by the
 * first covered emitter whose current and end lie inside [lo, hi), the claimed buffer. */
typedef struct auto_segment {
    void *ctx, *writer;
    uintptr_t lo, hi;
    uint32_t ordinal, open, count, external;
    auto_segment_op ops[AUTO_SEGMENT_OPS];
} auto_segment;

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
    uint32_t device_initialized, saturated, owner_done;
    /* owner only: open segment contexts (cellGcmSegmentBegin..End) and the
     * covered visits on contexts other than the attached ring */
    auto_segment segments[AUTO_SEGMENTS_OPEN];
    uint32_t segments_used, segment_ordinal, call_ordinal, segment_quarantine;
    auto_segment *segment_active; /* between a segment emitter's begin and end */
    uint64_t segment_visits, foreign_visits, segment_table_full, frame_marks_missed;
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
    uint64_t admitted, unassociated, internal_suppressed, conflicts;
    auto_wire_frame *frames;
    uint32_t frame_capacity;
    auto_clock clocks;
    uint32_t clock_put_frontier; /* owner-only snapshot around original PUT */
    uint64_t allocated;
    uint32_t owner_declared, domain_declared, epoch_declared, final_declared;
} auto_capture;

void p_auto_init(prof *p); /* before READY/start_gate publication */
void p_auto_drain(prof *p, int seal); /* sole encoder, core drain serialization */
void p_auto_seal_claim(void); /* before quiescence and every sink check */
void p_auto_owner_retire(void); /* before the core owner's final acknowledgement */
void p_auto_frame(uint64_t frame,uint64_t tb); /* registered GCM owner only */
#endif
