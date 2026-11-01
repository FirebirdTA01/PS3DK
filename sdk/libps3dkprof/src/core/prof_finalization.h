/* Private candidate metadata for the later paired exporter. No wire changes. */
#ifndef PS3DK_PROF_FINALIZATION_H
#define PS3DK_PROF_FINALIZATION_H
#include <stdint.h>
struct ps3dk_prof_final_text { const char *data; uint32_t length; };
struct ps3dk_prof_finalization {
    uint64_t accepted_bytes, completed_chunks, last_chunk_sequence;
    uint32_t prefix_crc32;
    uint64_t unregistered_refusals, session_id, freq_hz, tb_at_init;
    uint8_t abi, consumer_mode, trailer_written, io_finalized, join_failed;
    int shutdown_result;
    struct ps3dk_prof_final_text app, build_id, sdk_version, compiler_identity, flags, workload_tag;
};
/* NULL until the winning shutdown has finished sink close and join handling.
 * The returned record and its text remain immutable for the runtime lifetime.
 * accepted_bytes/CRC describe positively acknowledged bytes only. A failed
 * callback may have unreported side effects; io_finalized=0 forbids publication.
 * io_finalized means completed I/O, not complete/qualified observations. */
const struct ps3dk_prof_finalization *ps3dk_prof__finalization(void);
#endif
