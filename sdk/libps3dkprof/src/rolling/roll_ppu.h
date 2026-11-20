/* Private PPU/WINDOWS accepted-state encoder. Not runtime wiring. Arenas are
 * caller-owned, relocatable and bounded; no allocator, IO or internal locks. */
#ifndef PS3DK_PROF_ROLL_PPU_H
#define PS3DK_PROF_ROLL_PPU_H
#include "roll_output.h"
#ifdef __cplusplus
extern "C" {
#endif
#define pfp_plan ps3dk_prof__roll_ppu_plan
#define pfp_init ps3dk_prof__roll_ppu_init
#define pfp_prepare ps3dk_prof__roll_ppu_prepare
typedef struct {
    uint32_t incarnations, descriptors, stack_rows, frame_identities;
    /* metadata excludes the original16-byte header, includes each u32 raw
     * chunk length prefix. frame_identities is runs + unresolved tokens. */
    uint32_t metadata_bytes, checkpoint_bytes;
} pfp_config;
typedef struct {
    size_t context_bytes, arena_bytes, arena_alignment, checkpoint_bound;
    /* Implementation-owned checked arena partition offsets. */
    uint32_t lanes_offset, descriptors_offset, stacks_offset;
    uint32_t frames_offset, tokens_offset, metadata_offset;
} pfp_layout;
/* Checked layout only. Callers allocate using these results, not sizeof
 * guesses. Buffers must be max-aligned and disjoint for their full lifetime. */
int pfp_plan(const pfp_config *, pfp_layout *);
/* Supported original header flags are 0 or WINDOWS512. Context is immutable
 * after successful init. Initial arena/checkpoint describe original chunk0;
 * no HEADER/CLOCKS chunks are fabricated. No serialized-arena import. */
int pfp_init(void *context, size_t context_bytes, const pfp_config *,
    const uint8_t original_header[16], void *arena0, size_t arena_bytes,
    void *initial_checkpoint, size_t capacity, size_t *written);
/* Exact pfo_prepare callback. A whole original framed chunk is the only input.
 * Accepted/context/input remain immutable. Failure exposes no successor;
 * prepared and checkpoint scratch may be dirty. Success populates the entire
 * prepared arena and returns canonical PFRCKP01, except genuine TRAILER has
 * terminal=1/checkpoint_bytes=0. Offsets, never interior pointers, in arenas.
 * INVALID is semantic/unsupported or caller-contract refusal; RESOURCE uses
 * one PFR reason. No EXPORT exemptions or unselected-family acceptance. */
int pfp_prepare(void *context, const void *accepted, void *prepared,
    const void *chunk, size_t chunk_bytes, void *checkpoint, size_t capacity,
    pfo_prepared *result);
#ifdef __cplusplus
}
#endif
#endif
