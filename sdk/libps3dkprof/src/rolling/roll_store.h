/* Physical rolling-store foundation. Not wired into profiler capture/runtime.
 * Checkpoints are opaque here: this layer proves framing/storage consistency,
 * not that a checkpoint is a valid semantic restart of any record family. */
#ifndef PS3DK_PROF_ROLL_STORE_H
#define PS3DK_PROF_ROLL_STORE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Keep SDK-internal symbols out of the application's generic namespace. */
#define pfr_state_bytes ps3dk_prof__roll_state_bytes
#define pfr_plan ps3dk_prof__roll_plan
#define pfr_format ps3dk_prof__roll_format
#define pfr_open ps3dk_prof__roll_open
#define pfr_commit ps3dk_prof__roll_commit
#define pfr_get_view ps3dk_prof__roll_get_view
#define pfr_get_segment ps3dk_prof__roll_get_segment

enum { PFR_OK = 0, PFR_INVALID = -1, PFR_IO = -2, PFR_CORRUPT = -3,
       PFR_SPACE = -4, PFR_FAILED = -5, PFR_END = -6 };
typedef struct pfr_store pfr_store;
typedef struct {
    void *ctx;
    /* Positive short transfers are retried. Zero, negative, or overlarge
     * returns fail; adapters own EINTR handling. Offsets are always 64-bit. */
    int64_t (*read_at)(void *, uint64_t, void *, size_t);
    int64_t (*write_at)(void *, uint64_t, const void *, size_t);
    /* resize must produce exactly the requested logical file length. Only
     * format calls it; the caller supplies a new, exclusive, zero-length file.
     * This is not a promise of physical disk-space reservation. */
    int (*resize)(void *, uint64_t);
    int (*size)(void *, uint64_t *);
    /* A successful sync must order earlier writes before subsequent writes
     * under the adapter's documented failure model. This interface alone does
     * not establish power-loss durability for a filesystem/device. */
    int (*sync)(void *);
} pfr_io;
typedef struct {
    uint64_t max_bytes;
    uint32_t retained_slots;   /* 2..64, plus one spare slot */
    uint32_t checkpoint_bytes;
    uint32_t payload_bytes;    /* whole chunks, at least 20 */
    uint8_t session[16];       /* caller-generated capture identity */
} pfr_config;
typedef struct {
    uint64_t file_bytes;       /* all metadata, slots, spare and padding */
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
    /* Borrowed from caller-provided scratch until the next store operation. */
    const uint8_t *checkpoint, *payload;
    uint32_t checkpoint_bytes, payload_bytes;
} pfr_segment;

/* No allocation, locks, OS calls, runtime hooks, or mutable global state.
 * Caller allocates state_bytes with malloc/max_align_t alignment and at least
 * plan.scratch_bytes independent scratch bytes. State/scratch/input storage
 * must not overlap. Calls on a store are serialized by its owner. */
size_t pfr_state_bytes(void);
int pfr_plan(const pfr_config *, pfr_layout *);
int pfr_format(pfr_store *, const pfr_io *, const pfr_config *, void *, size_t);
/* Configuration/session must match exactly. IO may be read-only (write_at,
 * resize, sync NULL); commit is then refused. No resume into a live writer:
 * the caller must hold exclusive ownership before committing to an open store.
 * Reading a concurrently overwritten slot returns CORRUPT; a generation-aware
 * live snapshot/retry protocol is an integration requirement, not supplied here. */
int pfr_open(pfr_store *, const pfr_io *, const pfr_config *, void *, size_t);
/* Append one immutable segment. Payload is a concatenation of complete v1
 * {u32 type,u32 len,u64 chunk_seq,payload,u32 CRC} chunks; sequences start at
 * zero and continue from the committed frontier. A metadata-only segment is
 * allowed. Header/HEADER/CLOCKS and semantic state belong to the checkpoint.
 * Invalid input performs no IO. Any IO failure poisons this handle; reopen
 * after exclusive recovery to learn whether the old or new root committed. */
int pfr_commit(pfr_store *, const void *, size_t, const void *, size_t);
int pfr_get_view(const pfr_store *, pfr_view *);
int pfr_get_segment(pfr_store *, uint32_t, pfr_segment *);

#ifdef __cplusplus
}
#endif
#endif
