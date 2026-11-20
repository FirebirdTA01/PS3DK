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
#define pfr_format_v2 ps3dk_prof__roll_format_v2
#define pfr_publish_state ps3dk_prof__roll_publish_state
#define pfr_open ps3dk_prof__roll_open
#define pfr_commit ps3dk_prof__roll_commit
#define pfr_get_view ps3dk_prof__roll_get_view
#define pfr_get_config ps3dk_prof__roll_get_config
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
    /* generation is the root revision, independent of next_segment in v2. */
    uint64_t generation, first_segment, next_segment, first_chunk, next_chunk;
    uint64_t evicted_segments, evicted_chunks, evicted_payload_bytes;
    uint32_t retained_segments, recovered;
    /* V1 state is LEGACY_UNSPECIFIED. SEALED is a physical declaration;
     * semantic finals, successful close/join and completion are not proved. */
    uint32_t format_version, state, reason;
    /* V2 cumulative framed payload bytes, excluding original 16-byte header;
     * zero/unspecified for v1. */
    uint64_t bound_payload_bytes;
} pfr_view;
enum { PFR_LEGACY_UNSPECIFIED = 0, PFR_OPEN = 1, PFR_SEALED = 2, PFR_STOPPED_RESOURCE = 3 };
enum { PFR_REASON_NONE = 0, PFR_CHECKPOINT_BYTES = 1, PFR_STATE_BYTES = 2,
       PFR_IDENTITIES = 3, PFR_SUPPORT = 4, PFR_COUNTER_RANGE = 5 };
typedef struct {
    uint64_t expected_revision, next_chunk, payload_bytes;
    uint32_t state, reason;
} pfr_transition;
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
/* Default formatter keeps the v1 wire bytes. V2 is explicit, no migration. */
int pfr_format(pfr_store *, const pfr_io *, const pfr_config *, void *, size_t);
int pfr_format_v2(pfr_store *, const pfr_io *, const pfr_config *, void *, size_t);
/* V2 OPEN -> OPEN/SEALED/STOPPED_RESOURCE. Expected revision/chunk/byte
 * frontiers must match exactly. Writes/syncs only the alternate root, leaving
 * all slots/ranges untouched. Terminal handles refuse further commits with
 * END; invalid requests perform no IO; IO failures poison as for commit.
 * Caller owns semantic validation before SEALED and reason attribution. */
int pfr_publish_state(pfr_store *, const pfr_transition *);
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
int pfr_get_config(const pfr_store *, pfr_config *);
int pfr_get_segment(pfr_store *, uint32_t, pfr_segment *);

#ifdef __cplusplus
}
#endif
#endif
