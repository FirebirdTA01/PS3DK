/* Private integration contract; compiled only with PS3DK_PROF_WITH_ARCS.
 * No installed ABI or compiler entry points are enabled by this header. */
#ifndef PS3DK_PROF_ARC_RUNTIME_H
#define PS3DK_PROF_ARC_RUNTIME_H
#include "arcs.h"
struct ps3dk_prof_arc_view {
    uint32_t generation;
    struct ps3prof_arc_thread thread;
};
struct ps3dk_prof_arc_options {
    uint32_t max_arcs_per_thread;
    /* Stable borrowed view, outside runtime locks, old slot still held.
     * Slow consumers lower available thread capacity until they return.
     * Copy/encode before returning0; nonzero is terminal, never retried. */
    int (*consume)(void *, const struct ps3dk_prof_arc_view *);
    /* Exact same frozen counter later written into the timing trailer.
     * A nonzero value precludes complete combined counts. Return0 accepts
     * delivery, not completeness. Called once even if all slots retired.
     * 0 confirms arc delivery only. The combined result is valid only if the
     * timing capture also finalizes successfully; the exporter must check both. */
    int (*finish)(void *, uint64_t unregistered_refusals);
    void *ctx;
};
int ps3dk_prof__configure_arcs(const struct ps3dk_prof_arc_options *);
int ps3dk_prof__record_arc(uint64_t caller, uint64_t callee, uint32_t flags);
#endif
