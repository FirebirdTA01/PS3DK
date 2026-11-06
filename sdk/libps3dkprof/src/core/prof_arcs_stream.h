/* Private FINAL-only body encoder. Caller owns zeroed, disjoint state/arrays
 * for the whole session. One drainer calls it; this is not a live snapshot or
 * file API. No installed archive enables it yet. */
#ifndef PS3DK_PROF_ARCS_STREAM_H
#define PS3DK_PROF_ARCS_STREAM_H
#include "../hooks/arc_runtime.h"
enum {
    PS3DK_PROF_STREAM_OK=0, PS3DK_PROF_STREAM_INVALID=-1,
    PS3DK_PROF_STREAM_STATE=-2, PS3DK_PROF_STREAM_LIMIT=-3,
    PS3DK_PROF_STREAM_IO=-4
};
struct ps3dk_prof_stream_slot { uint32_t generation, incarnation, state; };
struct ps3dk_prof_stream_final {
    uint32_t incarnation;
    uint8_t state, limit_flags;
    uint64_t attempted, recorded, refused, lost;
};
struct ps3dk_prof_arc_stream {
    struct ps3dk_prof_stream_slot *slots;
    struct ps3dk_prof_stream_final *finals;
    ps3prof_arcs_sink write_all;
    void *ctx;
    uint32_t max_threads, max_arcs_per_thread, final_count, end_flags;
    uint64_t max_body_bytes, body_bytes, next_sequence, unregistered_refusals;
    uint32_t body_crc32;
    int replay_attempted, export_attempted;
    int initialized, finished, busy, error;
};
/* slots/finals each have max_threads elements, initially zeroed. A zero byte
 * budget permits an empty stream only. Invalid/repeated init changes nothing. */
int ps3dk_prof__stream_init(struct ps3dk_prof_arc_stream *,
                          struct ps3dk_prof_stream_slot *, struct ps3dk_prof_stream_final *,
                          uint32_t max_threads, uint32_t max_arcs_per_thread,
                          uint64_t max_body_bytes, ps3prof_arcs_sink write_all, void *ctx);
/* Stable views from the runtime only. Global incarnation uniqueness comes
 * from its allocator and row-key uniqueness from its collector; per-slot reuse
 * is checked here. The production reader independently rejects duplicates.
 * The callback must
 * consume all bytes before returning0. Nonzero may mean partial output: no
 * retry, no later writer calls. No caller-owned view pointer survives accept.
 * After init, public fields above are read-only to callers. */
int ps3dk_prof__stream_accept(struct ps3dk_prof_arc_stream *, const struct ps3dk_prof_arc_view *);
/* Freezes input and the existing timing counter; does NOT emit a TRAILER or
 * certify timing finalization. Prefix binding/publication is a later layer. */
int ps3dk_prof__stream_finish(struct ps3dk_prof_arc_stream *, uint64_t unregistered_refusals);
/* Post-finish, single-owner replay of THIS capture's immutable spool, positioned
 * at byte zero. Positive read counts must be <= requested, zero means EOF,
 * negative means terminal failure. Adapters own retries. Scratch (1..65536
 * bytes) must be disjoint from the stream, arrays and callback-owned storage.
 * A valid attempt is consumed before I/O, including on failure or reentry.
 * Success confirms exact body delivery/CRC/EOF only: no trailer, flush, close,
 * timing-finalization or combined-publication guarantee. Failure can leave
 * partial output; never append a final trailer or retry that output in place. */
typedef ptrdiff_t (*ps3dk_prof_stream_read)(void *, void *, size_t);
int ps3dk_prof__stream_replay(struct ps3dk_prof_arc_stream *,
                            ps3dk_prof_stream_read, void *, ps3prof_arcs_sink, void *,
                            void *scratch, size_t scratch_bytes);
#endif
