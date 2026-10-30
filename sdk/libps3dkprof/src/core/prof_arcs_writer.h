#ifndef PS3PROF_ARCS_WRITER_H
#define PS3PROF_ARCS_WRITER_H
#include <stddef.h>
#include <stdint.h>

/* Internal serializer, deliberately independent of hooks, allocation and PAL.
 * The caller owns a stable, acknowledged snapshot for the whole call. This
 * does not provide the later producer/drain publication handshake. */
struct ps3prof_arc_row {
    uint64_t caller_return_pc, callee_hook_pc, count;
    uint32_t flags;
};
struct ps3prof_arc_thread {
    uint32_t incarnation, slot;
    uint64_t os_thread_id;
    const char *name;
    const struct ps3prof_arc_row *rows;
    uint32_t row_count;
    uint8_t state, limit_flags;
    uint64_t attempted, recorded, refused, lost;
};
struct ps3prof_arc_snapshot {
    const char *app;
    const unsigned char *build_id;
    uint16_t build_id_len;
    const char *sdk_version, *compiler_identity, *flags, *workload_tag;
    uint8_t abi, mode;
    uint16_t snapshot_flags;
    uint64_t session_id, snapshot_id, timing_prefix_bytes, timing_chunk_seq;
    uint32_t timing_prefix_crc32, max_threads, max_arcs_per_thread;
    uint64_t freq_hz, tb_at_init;
    const struct ps3prof_arc_thread *threads;
    uint32_t thread_count;
    uint64_t sink_error_count;
    int32_t first_sink_error;
};
enum { PS3PROF_ARCS_OK=0, PS3PROF_ARCS_INVALID=-1, PS3PROF_ARCS_SINK=-2 };
/* Callback returns zero only after consuming all bytes. Any nonzero result
 * permanently stops this serialization; the caller retains partial output as
 * incomplete evidence. Never retry a partially written chunk in place. */
typedef int (*ps3prof_arcs_sink)(void *, const void *, size_t);
int ps3prof_arcs_write(const struct ps3prof_arc_snapshot *, ps3prof_arcs_sink, void *);
#endif
