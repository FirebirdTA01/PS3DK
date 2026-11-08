/* Internal, single-owner arc accumulator. No TLS, allocation, PAL or hooks.
 * The caller supplies disjoint storage and a zero-initialized collector.
 * After init, only record() may modify it. Storage lives through the eventual
 * owner-acknowledged snapshot; a drainer must never read a live collector.
 */
#ifndef PS3DK_PROF_ARC_COLLECTOR_H
#define PS3DK_PROF_ARC_COLLECTOR_H
#include "../core/prof_arcs_writer.h"

enum {
    PS3DK_PROF_ARC_OK = 0,
    PS3DK_PROF_ARC_REFUSED = 1,
    PS3DK_PROF_ARC_LIMIT = 2,
    PS3DK_PROF_ARC_INVALID = -1,
    PS3DK_PROF_ARC_STATE = -2
};
struct ps3dk_prof_arc_collector {
    struct ps3prof_arc_row *rows;
    uint32_t *index; /* row index+1, zero means empty */
    uint32_t capacity, index_capacity, row_count, initialized;
    uint64_t attempted, recorded, refused;
    uint8_t limit_flags; /* bit0 COUNTER_LIMIT, compatible with the writer */
};
/* capacity:1..65536; index_capacity is a power of two in [2*capacity,131072].
 * Invalid/repeated init leaves collector and supplied storage unchanged.
 */
int ps3dk_prof__arcs_init(struct ps3dk_prof_arc_collector *,
                        struct ps3prof_arc_row *, uint32_t capacity,
                        uint32_t *index, uint32_t index_capacity);
/* All observable states satisfy attempted=recorded+refused and
 * sum(rows.count)=recorded. UINT64_MAX is representable. Before an event
 * would overflow any counter, latch COUNTER_LIMIT and freeze ALL counters,
 * rows and indices together. No later event can unfreeze this collector.
 * Invalid PCs/flags and excess distinct edges are refused (and counted).
 */
int ps3dk_prof__arcs_record(struct ps3dk_prof_arc_collector *,
                          uint64_t caller_return_pc, uint64_t callee_hook_pc,
                          uint32_t flags);
#endif
