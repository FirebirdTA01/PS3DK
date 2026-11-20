/* Private forward-only combined schema-1 encoder boundary.
 * Accepts original PPU/WINDOWS + PROFILE and SPU families. GPU/AUTO/FIFO
 * capabilities remain refused. All configured state uses preallocated pools.
 * Not installed or runtime enabled.
 * SPU schema 1 carries causal support only, never qualified counter history. */
#ifndef PS3DK_PROF_ROLL_COMBINED_H
#define PS3DK_PROF_ROLL_COMBINED_H
#include "roll_output.h"
#ifdef __cplusplus
extern "C" {
#endif
#define pfm_plan ps3dk_prof__roll_combined_plan
#define pfm_init ps3dk_prof__roll_combined_init
#define pfm_prepare ps3dk_prof__roll_combined_prepare
typedef struct {
    uint32_t incarnations, descriptors, ppu_stack_rows, frame_identities;
    uint32_t spu_domains, spu_lanes, spu_images, epochs, brackets, image_descriptors;
    uint32_t journal_entries, event_runs, live_spu_stack_rows, saved_spu_witnesses;
    /* Includes original16 and each u32 raw-chunk length prefix. */
    uint32_t metadata_bytes, checkpoint_bytes;
    uint64_t prepare_work;
} pfm_config;
typedef struct {
    size_t context_bytes, arena_bytes, arena_alignment, checkpoint_bound;
} pfm_layout;
/* Checked conservative checkpoint bound, independent of current occupancy:
 * 148+(metadata_bytes-16)+128*incarnations+80*ppu_stack_rows
 * +16*frame_identities+32*journal_entries+64*event_runs+24*saved_spu_witnesses.
 * Undersized configured checkpoint capacity refuses; exact per-prepare
 * measurement also applies. Offsets/counts/lengths must fit u32 and size_t. */
int pfm_plan(const pfm_config *, pfm_layout *);
/* Admit real flags 1/3/7, optionally WINDOWS512; original header unmodified.
 * Caller owns max-aligned disjoint context, two relocatable arenas and output
 * buffers. No allocation/IO/locks, serialized-state import or synthetic prefix.
 * Context immutable after init. Initial checkpoint describes genuine chunk0. */
int pfm_init(void *, size_t, const pfm_config *, const uint8_t original_header[16],
             void *, size_t, void *, size_t, size_t *);
/* Exact pfo callback. Accepted/context/input unchanged even after rejection.
 * Prepared and checkpoint scratch are unobservable on failure. Original
 * TRAILER alone returns terminal=1/checkpoint_bytes=0; terminal is not a
 * complete-source, close/join or retention-eligibility claim. */
int pfm_prepare(void *, const void *, void *, const void *, size_t,
                void *, size_t, pfo_prepared *);
#ifdef __cplusplus
}
#endif
#endif
