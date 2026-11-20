/* Private cold ownership/framing inspection, not semantic validation.
 * Runtime enrollment additionally requires its trusted successful semantic
 * seal, close and join result. CRC32 is accidental-corruption detection, not
 * authenticated or signed ownership. No concurrent file mutation is allowed. */
#ifndef PS3DK_PROF_ROLL_RETENTION_INSPECT_H
#define PS3DK_PROF_ROLL_RETENTION_INSPECT_H
#include "roll_store.h"
#ifdef __cplusplus
extern "C" {
#endif
#define pfi_measure ps3dk_prof__roll_retention_measure
#define pfi_inspect ps3dk_prof__roll_retention_inspect

enum { PFI_OK = 0, PFI_INVALID = -1, PFI_IO = -2, PFI_CORRUPT = -3,
       PFI_SPACE = -4, PFI_INCOMPLETE = -5, PFI_UNSUPPORTED = -6 };
enum { PFI_CHECKPOINT_PPU = 1, PFI_CHECKPOINT_COMBINED = 2,
       PFI_IDENTITY_FRAMING_FINAL = 1 };
#define PFI_WORK_BYTES 4096u
typedef struct {
    uint64_t max_file_bytes;
    uint32_t max_slot_bytes, max_checkpoint_bytes, max_metadata_bytes;
} pfi_limits;
typedef struct {
    pfr_config config;
    pfr_layout layout;
    size_t store_bytes;
} pfi_requirements;
typedef struct {
    void *store; size_t store_bytes;
    void *slot; size_t slot_bytes;
    void *work; size_t work_bytes;
} pfi_buffers;
typedef struct {
    uint64_t file_bytes, generation, first_chunk, next_chunk;
    uint32_t file_crc32, checkpoint_kind, original_flags, recovered;
    uint32_t evidence; /* exactly IDENTITY_FRAMING_FINAL; never full semantics */
    uint8_t physical_session[16]; /* opaque; no invented mapping to HEADER's u64 */
} pfi_result;

/* Read-only, allocation-free; validates the config page and caller limits
 * before reporting a bounded full-slot requirement. work >= WORK_BYTES.
 * Output is unchanged on every failure. Does not select/validate a root. */
int pfi_measure(const pfr_io *, const pfi_limits *, void *work, size_t work_bytes,
                pfi_requirements *);
/* Revalidates measured requirements; never trusts a saved unchecked plan.
 * Caller supplies aligned store storage and mutually disjoint buffers, none
 * overlapping arguments/output. read_at and size are required. No write,
 * resize, sync, allocation, descriptor open/close, or producer state access.
 * Checks selected v2 SEALED roots/slots through pfr, checkpoint envelope and
 * actual original HEADER app/session, and an actual clean terminal TRAILER.
 * Cursor equations, canonical transitions, clocks and family finals are NOT
 * checked. Hashes every file byte with bounded scratch. Output is unchanged
 * on failure. The caller owns file synchronization/close and semantic trust. */
int pfi_inspect(const pfr_io *, const pfi_limits *, const pfi_buffers *,
                const char *app, size_t app_bytes, uint64_t original_session,
                pfi_result *);
#ifdef __cplusplus
}
#endif
#endif
