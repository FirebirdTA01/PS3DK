#ifndef PS3DK_PROF_AUTO_WIRE_H
#define PS3DK_PROF_AUTO_WIRE_H
#include <stddef.h>
#include <stdint.h>

#define auto_encode_config ps3dk_prof__auto_encode_config
#define auto_encode_ops ps3dk_prof__auto_encode_ops
#define auto_encode_frame ps3dk_prof__auto_encode_frame
#define auto_encode_status ps3dk_prof__auto_encode_status
#define auto_encode_clock ps3dk_prof__auto_encode_clock

enum { C_AUTO_CONFIG=34, C_AUTO_OPS=35, C_AUTO_COVERAGE=36, C_AUTO_STATUS=37, C_AUTO_CLOCK=38 };
enum { AUTO_REFUSED=1, AUTO_CONFIG_BYTES=160, AUTO_OP_BYTES=128, AUTO_FRAME_BYTES=384,
       AUTO_STATUS_BYTES=128, AUTO_CLOCK_BYTES=96, AUTO_CHUNK_MAX=1048576 };
enum { AUTO_GCM_INIT_FAILED=1, AUTO_NO_INDEPENDENT_DRAIN, AUTO_LEASE_UNVERIFIED,
       AUTO_PRIOR_WRITES_UNKNOWN, AUTO_RESOURCE_CONFLICT, AUTO_NO_OWNER,
       AUTO_ALLOCATION_FAILED, AUTO_UNSUPPORTED_CONTEXT, AUTO_STOPPED, AUTO_NO_GCM_INIT };
enum { AUTO_FINAL=1, AUTO_LOWER_BOUND=2, AUTO_SATURATED=4, AUTO_OWNER_UNACK=8,
       AUTO_STATUS_CONFLICT=16, AUTO_STATUS_RESET=32, AUTO_STATUS_SEGMENT_QUARANTINE=64,
       AUTO_STATUS_FRAME_MARK_MISSED=128 /* at least one FRAME mark was refused */,
       AUTO_STATUS_FRAMES_FLIP=256 /* some frames were delimited by flips */,
       AUTO_STATUS_FRAMES_EXPLICIT=512 /* some frames were delimited by ps3dk_prof_frame */ };
/* COVERAGE row flag: this frame was opened by a flip on the owner thread. */
enum { AUTO_FRAME_ROW_FLIP=32,
       AUTO_FRAME_ROW_PRESENT=64 /* this writer observed presentation in this row (block at 352) */ };
/* CONFIG masks of this writer: ARRAY|INDEX|CLEAR|FRAME covered, plus PRESENT=16
 * (flip-wait scopes and polling cadence, 384-byte COVERAGE rows; not an OPS
 * kind); unsafe, immediate and direct/uninstrumented forms unsupported (CALL
 * segments built through the segment API are covered). A reader that knows
 * only 7/15 or 15/11 refuses 31/11 at CONFIG. */
enum { AUTO_KIND_MASK=31, AUTO_FORM_MASK=11 };
/* Polling-cadence gap bound: 1 ms in PPU ticks (see auto_runtime.c). */
enum { AUTO_CADENCE_BOUND_US=1000 };
/* OPS: kind 4 FRAME marks a frame boundary (args[0] 1 begin, 2 end); flag 64
 * SEGMENT marks an operation built inside a CALL segment (u32@92 segment
 * ordinal, u32@116 call ordinal, both nonzero). */
enum { AUTO_KIND_FRAME=4, AUTO_FRAME_BEGIN=1, AUTO_FRAME_END=2, AUTO_OP_SEGMENT=64 };

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
    uint64_t counts[3][12]; /* visited, observed, ten disjoint exclusions */
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
#endif
