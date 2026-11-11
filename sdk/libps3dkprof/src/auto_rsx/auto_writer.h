#ifndef PS3DK_PROF_AUTO_WRITER_H
#define PS3DK_PROF_AUTO_WRITER_H
#include "auto_config.h"

#define auto_writer_init ps3dk_prof__auto_writer_init
#define auto_writer_begin ps3dk_prof__auto_writer_begin
#define auto_writer_ops ps3dk_prof__auto_writer_ops
#define auto_writer_frame ps3dk_prof__auto_writer_frame
#define auto_writer_clock ps3dk_prof__auto_writer_clock
#define auto_writer_status ps3dk_prof__auto_writer_status

/* The adapter must return zero only after accepting the complete chunk. A
 * failed/short write is terminal and is never retried. The capture drainer is
 * the sole caller; owner data is passed only as an immutable/acquired snapshot.
 * This writer neither polls the GPU nor acknowledges a queue entry itself. */
typedef int (*auto_chunk_sink)(void *, uint32_t type, const uint8_t *, size_t);
typedef struct auto_writer {
    const auto_config_store *config;
    uint8_t *scratch;
    size_t capacity;
    auto_chunk_sink sink;
    void *user;
    uint32_t config_sent, closed, failed, status_seen;
    uint16_t sticky;
    uint64_t operations_written, frames_written, clocks_written;
    uint64_t publication_max;
    uint64_t prior[15];
} auto_writer;

int auto_writer_init(auto_writer *, const auto_config_store *, uint8_t *, size_t, auto_chunk_sink, void *);
/* Returns ready-for-data. Zero means not frozen, refused/closed, or failed.
 * A refused frozen config emits CONFIG followed by its zero terminal STATUS,
 * without requiring invented owner/domain declarations. No OPS/CLOCK follow. */
int auto_writer_begin(auto_writer *);
int auto_writer_ops(auto_writer *, const auto_wire_op *, uint32_t count);
int auto_writer_frame(auto_writer *, const auto_wire_frame *);
int auto_writer_clock(auto_writer *, const auto_wire_clock *);
int auto_writer_status(auto_writer *, uint16_t flags, const uint64_t counters[15]);
#endif
