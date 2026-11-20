/* Test-first API contract. RED includes this file; normal tests include the
 * production header. This is not linked into the SDK. */
#ifndef TEST_ROLL_CONTRACT_H
#define TEST_ROLL_CONTRACT_H
#include <stddef.h>
#include <stdint.h>
enum { PFR_OK = 0, PFR_INVALID = -1, PFR_IO = -2, PFR_CORRUPT = -3,
       PFR_SPACE = -4, PFR_FAILED = -5, PFR_END = -6 };
typedef struct pfr_store pfr_store;
typedef struct {
    void *ctx;
    int64_t (*read_at)(void *, uint64_t, void *, size_t);
    int64_t (*write_at)(void *, uint64_t, const void *, size_t);
    int (*resize)(void *, uint64_t);
    int (*size)(void *, uint64_t *);
    int (*sync)(void *);
} pfr_io;
typedef struct {
    uint64_t max_bytes;
    uint32_t retained_slots, checkpoint_bytes, payload_bytes;
    uint8_t session[16];
} pfr_config;
typedef struct {
    uint64_t file_bytes;
    uint32_t slot_bytes;
    size_t scratch_bytes;
} pfr_layout;
typedef struct {
    uint64_t generation, first_segment, next_segment, first_chunk, next_chunk;
    uint64_t evicted_segments, evicted_chunks, evicted_payload_bytes;
    uint32_t retained_segments, recovered;
} pfr_view;
typedef struct {
    uint64_t segment, first_chunk, next_chunk;
    const uint8_t *checkpoint, *payload;
    uint32_t checkpoint_bytes, payload_bytes;
} pfr_segment;
size_t pfr_state_bytes(void);
int pfr_plan(const pfr_config *, pfr_layout *);
int pfr_format(pfr_store *, const pfr_io *, const pfr_config *, void *, size_t);
int pfr_format_v2(pfr_store *, const pfr_io *, const pfr_config *, void *, size_t);
int pfr_open(pfr_store *, const pfr_io *, const pfr_config *, void *, size_t);
int pfr_commit(pfr_store *, const void *, size_t, const void *, size_t);
int pfr_get_view(const pfr_store *, pfr_view *);
int pfr_get_segment(pfr_store *, uint32_t, pfr_segment *);
#endif
