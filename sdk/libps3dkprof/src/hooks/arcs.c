#include "arcs.h"
#include <string.h>
int ps3dk_prof__arcs_init(struct ps3dk_prof_arc_collector *c,
                        struct ps3prof_arc_row *rows, uint32_t capacity,
                        uint32_t *index, uint32_t index_capacity) {
    if (!c) return PS3DK_PROF_ARC_INVALID;
    if (c->initialized) return PS3DK_PROF_ARC_STATE;
    if (!rows || !index || capacity < 1 || capacity > 65536 ||
        index_capacity < capacity * 2 || index_capacity > 131072 ||
        (index_capacity & (index_capacity - 1))) return PS3DK_PROF_ARC_INVALID;
    memset(rows, 0, (size_t)capacity * sizeof *rows);
    memset(index, 0, (size_t)index_capacity * sizeof *index);
    *c = (struct ps3dk_prof_arc_collector){
        .rows=rows, .index=index, .capacity=capacity,
        .index_capacity=index_capacity, .initialized=1
    };
    return PS3DK_PROF_ARC_OK;
}
int ps3dk_prof__arcs_record(struct ps3dk_prof_arc_collector *c,
                          uint64_t caller, uint64_t callee, uint32_t flags) {
    struct ps3prof_arc_row *row = NULL;
    uint32_t bucket = 0;
    int refuse = 0;
    if (!c || !c->initialized) return PS3DK_PROF_ARC_STATE;
    if (c->limit_flags) return PS3DK_PROF_ARC_LIMIT;
    if (c->attempted == UINT64_MAX) goto limit;
    if (!callee || (callee & 3) || flags > 1 ||
        (flags ? caller != 0 : (!caller || (caller & 3)))) {
        refuse = 1;
    } else {
        /* Mix full-width PCs, then bounded linear probing at <=50% occupancy.
         * Full key comparison, not the hash, determines identity. */
        uint64_t hash = (caller >> 2) ^ (callee * UINT64_C(0x9e3779b97f4a7c15));
        hash ^= hash >> 30; hash *= UINT64_C(0xbf58476d1ce4e5b9);
        hash ^= hash >> 27; hash *= UINT64_C(0x94d049bb133111eb);
        hash ^= hash >> 31;
        bucket = (uint32_t)hash & (c->index_capacity - 1);
        for (uint32_t probe = 0; probe < c->index_capacity; ++probe) {
            uint32_t entry = c->index[bucket];
            if (!entry) break;
            row = &c->rows[entry - 1];
            if (row->caller_return_pc == caller && row->callee_hook_pc == callee && row->flags == flags)
                break;
            row = NULL;
            bucket = (bucket + 1) & (c->index_capacity - 1);
        }
        if (!row && c->row_count == c->capacity) refuse = 1;
    }
    if (refuse) {
        if (c->refused == UINT64_MAX) goto limit;
        ++c->attempted; ++c->refused;
        return PS3DK_PROF_ARC_REFUSED;
    }
    if (c->recorded == UINT64_MAX || (row && row->count == UINT64_MAX)) goto limit;
    /* No observable partial event: all overflow checks precede every write.
     * The sole owner may expose these rows only after its acknowledgement. */
    if (!row) {
        row = &c->rows[c->row_count];
        *row = (struct ps3prof_arc_row){
            .caller_return_pc=caller, .callee_hook_pc=callee, .count=0, .flags=flags
        };
        c->index[bucket] = ++c->row_count;
    }
    ++row->count; ++c->recorded; ++c->attempted;
    return PS3DK_PROF_ARC_OK;
limit:
    c->limit_flags = 1;
    return PS3DK_PROF_ARC_LIMIT;
}
