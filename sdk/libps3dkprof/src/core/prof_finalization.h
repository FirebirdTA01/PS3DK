/* The runtime's private finalization record: in every build, the common prefix that the live link's END reports
 * (live/live_api.c); with PS3DK_PROF_WITH_ARCS, the candidate metadata for the later paired exporter as well.
 * No wire changes. */
#ifndef PS3DK_PROF_FINALIZATION_H
#define PS3DK_PROF_FINALIZATION_H
#include <stdint.h>
struct ps3dk_prof_final_text { const char *data; uint32_t length; };
/* The common prefix is in every build (the live link's END reads only it); the arc-only tail exists only with
 * PS3DK_PROF_WITH_ARCS, so every producer and consumer of the record is compiled with the same setting. */
struct ps3dk_prof_finalization {
    uint64_t accepted_bytes;
    int shutdown_result;
    uint8_t trailer_written, io_finalized, join_failed;
#ifdef PS3DK_PROF_WITH_ARCS
    uint8_t abi, consumer_mode;
    uint64_t completed_chunks, last_chunk_sequence;
    uint32_t prefix_crc32;
    uint64_t unregistered_refusals, session_id, freq_hz, tb_at_init;
    struct ps3dk_prof_final_text app, build_id, sdk_version, compiler_identity, flags, workload_tag;
#endif
};
/* NULL until the winning shutdown has finished sink close and join handling.
 * The returned record and its text remain immutable for the runtime lifetime.
 * accepted_bytes/CRC describe positively acknowledged bytes only. A failed
 * callback may have unreported side effects; io_finalized=0 forbids publication.
 * io_finalized means completed I/O, not complete/qualified observations; a consumer also checks
 * shutdown_result. */
const struct ps3dk_prof_finalization *ps3dk_prof__finalization(void);

/* 1 once a shutdown has won its claim (acquire), before it takes any lock or sets the stop latch; 0 before, or when
 * the profiler is not ready.  A snapshot: it does not arbitrate a start racing a shutdown, which the caller
 * serializes (init, live_start, shutdown and live_stop are never concurrent). */
int ps3dk_prof__shutdown_claimed(void);
#endif
