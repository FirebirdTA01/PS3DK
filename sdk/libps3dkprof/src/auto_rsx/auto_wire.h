#ifndef PS3DK_PROF_AUTO_WIRE_H
#define PS3DK_PROF_AUTO_WIRE_H
#include <stddef.h>
#include <stdint.h>

#define auto_encode_config ps3dk_prof__auto_encode_config
#define auto_encode_ops ps3dk_prof__auto_encode_ops
#define auto_encode_frame ps3dk_prof__auto_encode_frame
#define auto_encode_status ps3dk_prof__auto_encode_status
#define auto_encode_clock ps3dk_prof__auto_encode_clock
#define auto_encode_ops_ring ps3dk_prof__auto_encode_ops_ring
#define auto_encode_frame_ring ps3dk_prof__auto_encode_frame_ring
#define auto_encode_status_ring ps3dk_prof__auto_encode_status_ring

enum { C_AUTO_CONFIG=34, C_AUTO_OPS=35, C_AUTO_COVERAGE=36, C_AUTO_STATUS=37, C_AUTO_CLOCK=38 };
enum { AUTO_REFUSED=1, AUTO_CONFIG_BYTES=176, AUTO_OP_BYTES=160, AUTO_FRAME_BYTES=480,
       AUTO_STATUS_BYTES=128, AUTO_CLOCK_BYTES=96, AUTO_CHUNK_MAX=1048576 };
enum { AUTO_CONFIG_RING_BYTES=216, AUTO_OP_RING_BYTES=176, AUTO_FRAME_RING_BYTES=528,
       AUTO_STATUS_RING_BYTES=192, AUTO_KIND_RING_MASK=2047, AUTO_OP_RING=128 };
enum { AUTO_GCM_INIT_FAILED=1, AUTO_NO_INDEPENDENT_DRAIN, AUTO_LEASE_UNVERIFIED,
       AUTO_PRIOR_WRITES_UNKNOWN, AUTO_RESOURCE_CONFLICT, AUTO_NO_OWNER,
       AUTO_ALLOCATION_FAILED, AUTO_UNSUPPORTED_CONTEXT, AUTO_STOPPED, AUTO_NO_GCM_INIT };
enum { AUTO_FINAL=1, AUTO_LOWER_BOUND=2, AUTO_SATURATED=4, AUTO_OWNER_UNACK=8,
       AUTO_STATUS_CONFLICT=16, AUTO_STATUS_RESET=32, AUTO_STATUS_SEGMENT_QUARANTINE=64,
       AUTO_STATUS_FRAME_MARK_MISSED=128 /* at least one FRAME mark was refused */,
       AUTO_STATUS_FRAMES_FLIP=256 /* some frames were delimited by flips */,
       AUTO_STATUS_FRAMES_EXPLICIT=512 /* some frames were delimited by ps3dk_prof_frame */,
       /* a CALL segment whose build carried timed draws was called again without a rebuild: its embedded
        * report writes ran again and may have overwritten other records' timestamps (sticky; every per-frame
        * verdict is UNKNOWN) */
       AUTO_STATUS_SEGMENT_REPLAY=1024 };
/* COVERAGE row flag: this frame was opened by a flip on the owner thread. */
enum { AUTO_FRAME_ROW_FLIP=32,
       AUTO_FRAME_ROW_PRESENT=64 /* this writer observed presentation in this row (block at 352) */ };
/* CONFIG masks of this writer: ARRAY|INDEX|CLEAR|FRAME covered, plus PRESENT=16
 * (flip-wait scopes and polling cadence, 384-byte COVERAGE rows; not an OPS
 * kind) and SEGMENT_BUILD=32 (segment records name their building frame in a
 * 144-byte OPS record; COVERAGE u32@380 counts segment draws a CALL executed
 * without a record; not an OPS kind); unsafe, immediate and
 * direct/uninstrumented forms unsupported (CALL segments built through the
 * segment API are covered), and CLOCK_BRACKET=64 (a clock anchor k releases the
 * label at 2k-1 before its timestamp and 2k after it; the drainer brackets the
 * timestamp between the last poll below 2k-1 and the first at or above 2k:
 * label_first/label_second carry those observed values), and CALL_BRACKET=128
 * (OPS kind 5 CALL: one report pair around a whole cellGcmSegmentCall whose
 * build was not fully timed, see below), and LABEL_POLL=256 (416-byte COVERAGE
 * rows: a label-poll cadence block at 384, see auto_wire_frame), and TAG=512
 * (160-byte OPS records: u32@144 tag current at the record's claim, u32@148 the
 * fragment-program offset last set on its context, u32@152 a RANGE's nesting
 * depth, u32@156 zero; OPS kind 6 RANGE; 480-byte COVERAGE rows with the
 * timed-range block at 416 and the flip-wait scope block at 448). A reader that knows only 7/15, 15/11, 31/11,
 * 63/11, 127/11, 255/11 or 511/11 refuses 1023/11 at CONFIG. */
enum { AUTO_KIND_MASK=1023, AUTO_FORM_MASK=11 };
/* Polling-cadence gap bound: 1 ms in PPU ticks (see auto_runtime.c). */
enum { AUTO_CADENCE_BOUND_US=1000 };
/* OPS: kind 4 FRAME marks a frame boundary (args[0] 1 begin, 2 end); flag 64
 * SEGMENT marks an operation built inside a CALL segment (u32@92 segment
 * ordinal, u32@116 call ordinal, both nonzero). */
enum { AUTO_KIND_FRAME=4, AUTO_FRAME_BEGIN=1, AUTO_FRAME_END=2, AUTO_OP_SEGMENT=64 };
/* REPORT_RING (2047/11): OPS flag 128 (AUTO_OP_RING) marks an owner ARRAY/INDEX/CLEAR timed through a recycled local
 * block and read from its MAIN copy; its serials are 2g-1/2g on CONFIG ring_label and u64@120 (published) is the ring
 * label value observed when it was read. Never SEGMENT, never kinds 4..6. */
/* OPS kind 5 CALL (CALL_BRACKET): the begin report precedes a CALL in the ring,
 * the end report follows its retirement label, and the operation-label release
 * of that CALL completes it with the build's records. TOKEN is the calling
 * frame; not SEGMENT; args[0] the build's covered draws, args[1] those this
 * CALL executed without a record of their own (counted into that frame's
 * segment_unrepresented), args[2] the segment ordinal, args[3] the call
 * ordinal, args[4] how many of args[1] were refused while building with an
 * untimed-only exclusion counted in the build-tail frame (BUILD_KNOWN only when
 * args[4] is nonzero; the tail is zero otherwise), args[5..6] zero. */
enum { AUTO_KIND_CALL=5 };
/* OPS kind 6 RANGE (TAG): an AUTO-owned range, a timestamp report at
 * PS3DK_PROF_AUTO_RANGE_BEGIN and one at PS3DK_PROF_AUTO_RANGE_END in the ring,
 * completed by an operation-label release after the end report. TOKEN is the
 * frame of both (a range whose END falls in another frame is refused); args[0]
 * the name descriptor index + 1, args[1] 1 for a timed range, args[2..6] zero;
 * u32@152 its nesting depth 1..4; the tag is the one current at BEGIN. */
enum { AUTO_KIND_RANGE=6, AUTO_RANGE_TIMED=1 };
/* OPS build tail (SEGMENT records only, zero otherwise): u64@128 build frame,
 * u32@136 build owner, u16@140 build flags (BUILD_KNOWN=1: the draw was
 * admitted while a coverage frame was open; without it owner and frame are
 * zero), u16@142 zero. */
enum { AUTO_OP_BUILD_KNOWN=1 };

/* Native writer inputs. No native struct is copied to the wire. Semantic
 * validation and immutable snapshot ownership belong to the runtime/reader;
 * these encoders bound destination writes and encode every reserved byte. */
typedef struct auto_wire_config {
    uint32_t instance, domain, epoch, owner, generation;
    uint32_t report_first, report_count, op_label, clock_label, pairs, metadata, frames;
    uint64_t frequency, allocated, ceiling;
    uint32_t reason;
    int32_t application_rc;
    uint8_t mechanism[32], binding[32];
    uint32_t call_pairs, range_pairs; /* TAG: u32@160 whole-CALL reserve, u32@164 range reserve; u64@168 zero */
    /* REPORT_RING (2047/11), u32 fields at 176..208, u32@212=1 (owner draws only).
     * pairs still names the entire local pool; legacy_pairs excludes the ring
     * blocks. Metadata and raw-ring storage are independently bounded. */
    uint32_t ring_bytes,ring_blocks,ring_pairs,ring_first,ring_label;
    uint32_t ring_stride,ring_slots,metadata_stride,legacy_pairs;
} auto_wire_config;

typedef struct auto_wire_op {
    uint64_t id;
    uint32_t frame_owner;
    uint16_t kind, flags;
    uint64_t frame, tb_before, tb_after, raw_begin, raw_end, caller;
    uint32_t args[7]; /* words, start, count, offset, index type, mode, clear mask */
    uint32_t report_begin, report_end, serial_begin, serial_end, generation;
    uint64_t published;
    uint32_t segment, call; /* SEGMENT only; zero otherwise */
    uint64_t build_frame;   /* SEGMENT only: the coverage frame that admitted this draw */
    uint32_t build_owner;
    uint16_t build_flags;   /* AUTO_OP_BUILD_KNOWN */
    uint32_t tag;           /* TAG: the tag current at the claim (a segment draw: at its build) */
    uint32_t fp_offset;     /* TAG: the last cellGcmSetFragmentProgram offset on the record's context, 0 if none */
    uint32_t depth;         /* RANGE only: nesting depth 1..4 */
    /* REPORT_RING tail at 160: zero for legacy local records. The serials of
     * a ring record are 2*generation-1 / 2*generation on CONFIG's ring_label;
     * report indices name its reusable local block, raw values its MAIN copy.
     * state: 0 untouched app M2MF, 1 restored app state, 2 uncertain. */
    uint32_t ring_generation,ring_slot;
    uint16_t ring_ordinal,ring_local;
    uint32_t ring_state;
} auto_wire_op;

typedef struct auto_wire_frame {
    uint16_t flags;
    uint32_t instance, owner;
    uint64_t frame, tb_begin, tb_end, unsupported, lost;
    uint32_t flip_waits;    /* u32@12: flip-wait scopes completed inside this frame */
    uint64_t flip_wait;     /* u64@56: their elapsed owner PPU ticks, including scheduling and polling */
    /* presentation block at 352 (PRESENT rows): polling cadence is named
     * evidence of how often the owner looked, never of waiting */
    uint32_t cadence_runs;      /* u32@352: pending-poll runs closed by a done poll, every gap within the bound */
    uint32_t cadence_broken;    /* u32@356: runs ended by a gap over the bound or by a flip-wait scope */
    uint32_t cadence_spanning;  /* u32@360: runs that crossed a boundary of this frame (charged to neither) */
    uint32_t flip_wait_spanning;/* u32@364: flip-wait scopes that crossed a boundary of this frame */
    uint64_t cadence;           /* u64@368: summed bounded gaps of the complete runs */
    uint32_t flip_wait_timeouts;/* u32@376: flip-wait scopes that returned the timeout error */
    /* u32@380: covered segment draws that CALLs in this frame executed without a committed record carrying
     * the CALL's token (re-CALL, untracked segment, draws refused or quarantined while building, a refused CALL,
     * a pair lost at commit). A lower bound: an untracked segment counts 1. Saturates (row SATURATED). */
    uint32_t segment_unrepresented;
    /* label-poll block at 384 (LABEL_POLL): cellGcmIsLabelPassed / cellGcmIsLabelValuePassed reads on the owner
     * thread, with the same run rules as flip-status polling cadence (a run of not-yet-passed polls of one label
     * closed by a passed poll, every gap within the bound). Polling cadence, never evidence of waiting. */
    uint32_t label_runs;        /* u32@384: complete runs */
    uint32_t label_broken;      /* u32@388: runs ended by a gap over the bound, another label or a flip-wait scope */
    uint32_t label_spanning;    /* u32@392: runs that crossed a boundary of this frame (charged to neither) */
    uint32_t label_index;       /* u32@396: the complete runs' label + 1; 0 none, 0xffffffff several */
    uint64_t label_cadence;     /* u64@400: summed bounded gaps of the complete runs, over every label */
    uint64_t label_one;         /* u64@408: the largest total of any ONE label's complete runs (never a sum) */
    /* timed-range block at 416 (TAG): u64@416/424/432 covered ARRAY/INDEX/CLEAR visits left untimed because timing
     * was limited to timed ranges and none was open (counted in visited, not observed, not in the ten exclusions);
     * u32@440 AUTO ranges refused in this frame (depth, no pair, no room, a segment build, another frame); u32@444
     * zero */
    uint64_t outside_timed[3];
    uint32_t ranges_refused;
    /* flip-wait scope block at 448 (TAG): the owner PPU ticks [begin, end] of the first two flip-wait scopes counted
     * at 12 (u64@448/456 and u64@464/472); zero for scopes not recorded. With more than two scopes in the frame the
     * reader cannot place the others and does not use the block. */
    uint64_t flip_wait_at[2][2];
    uint64_t counts[3][12]; /* visited, observed, ten disjoint exclusions */
    uint64_t ring_full[3],ring_state[3]; /* REPORT_RING: at 480 and 504, disjoint extra exclusions */
} auto_wire_frame;

typedef struct auto_wire_clock {
    uint16_t flags;
    uint32_t instance, epoch, id, role, slot, serial, polls;
    uint64_t tb_before, tb_after, raw, repeated;
    uint32_t label_first, label_second;
    uint64_t frame;
    uint32_t owner;
    uint64_t elapsed;
} auto_wire_clock;

/* Return encoded byte length, or zero without changing dst on insufficient
 * capacity, null input or an impossible counted chunk. Caller owns snapshots. */
size_t auto_encode_config(uint8_t *, size_t, const auto_wire_config *);
size_t auto_encode_ops(uint8_t *, size_t, uint32_t, const auto_wire_op *, uint32_t);
size_t auto_encode_frame(uint8_t *, size_t, const auto_wire_frame *);
size_t auto_encode_status(uint8_t *, size_t, uint32_t, uint16_t, const uint64_t [15]);
size_t auto_encode_clock(uint8_t *, size_t, const auto_wire_clock *);
size_t auto_encode_ops_ring(uint8_t *, size_t, uint32_t, const auto_wire_op *, uint32_t);
size_t auto_encode_frame_ring(uint8_t *, size_t, const auto_wire_frame *);
/* Tail u64[8]: issued blocks, retired blocks, published label, held extents,
 * copied bytes, untouched-state copies, restored-state copies, decoded words. */
size_t auto_encode_status_ring(uint8_t *, size_t, uint32_t, uint16_t, const uint64_t [15], const uint64_t [8]);
#endif
