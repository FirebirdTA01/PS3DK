/* Private post-shutdown coordinator; not installed or wired into startup. */
#ifndef PS3DK_PROF_ARCS_EXPORT_H
#define PS3DK_PROF_ARCS_EXPORT_H
#include "prof_arcs_stream.h"
enum { PS3DK_PROF_STREAM_TIMING=-5 };
struct ps3dk_prof_arc_output {
    ps3prof_arcs_sink write_all;
    int (*flush)(void *);
    int (*close)(void *);
    void *ctx;
};
/* Uses only the runtime's immutable finalization getter, never caller-claimed
 * timing success. Single owner; callbacks may not mutate the stream, arrays or
 * finalization. Scratch/source obey stream_replay's contract.
 * Preflight refusal performs no I/O and takes NO ownership: the caller still
 * owns and must close its output. Once export_attempted is set, this function
 * owns output close, exactly once even on failure; flush occurs only after a
 * complete trailer. No retry. The input source remains caller-owned.
 * 0 means serialization + output flush/close succeeded, not complete data,
 * verified companion pairing or publication at a final filename. */
int ps3dk_prof__stream_export(struct ps3dk_prof_arc_stream *, uint64_t snapshot_id,
                            ps3dk_prof_stream_read, void *read_ctx,
                            const struct ps3dk_prof_arc_output *,
                            void *scratch, size_t scratch_bytes);
#endif
