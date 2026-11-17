/* Private shared PPU family machinery. Not an SDK API or alternate stream.
 * The envelope owner admits actual original flags and any external metadata
 * type BEFORE calling accept. No historical records are synthesized. */
#ifndef PS3DK_PROF_ROLL_PPU_INTERNAL_H
#define PS3DK_PROF_ROLL_PPU_INTERNAL_H
#include "roll_ppu.h"
#define pfp_core_init ps3dk_prof__roll_ppu_core_init
#define pfp_core_accept ps3dk_prof__roll_ppu_core_accept
#define pfp_core_view ps3dk_prof__roll_ppu_core_view
#define pfp_core_sections ps3dk_prof__roll_ppu_core_sections
typedef struct {
    const uint8_t *header, *metadata;
    uint64_t next, bytes, clock_frequency, physical_rows;
    uint32_t metadata_bytes, metadata_count, lanes, stacks, frames, tokens, terminal;
} pfp_view;
typedef struct { uint32_t incarnation, reserved; uint64_t chunks; } pfp_record_chunks;
int pfp_core_init(void *, size_t, const pfp_config *, const uint8_t[16], void *, size_t);
/* external_metadata names exactly 8..13,15,16,17,18 only after the combined
 * owner validates actual flags, original position and the entire payload.
 * 13 advances whole-framed N/B without metadata; other external types append
 * metadata once. Zero admits only existing PPU types, unchanged.
 * Accepted is immutable. Prepared may be dirty on refusal. */
int pfp_core_accept(void *, const void *, void *, const void *, size_t,
                    uint32_t external_metadata, pfo_prepared *);
int pfp_core_view(const void *, const void *, pfp_view *);
/* Writes sections 2+3 using actual PPU rows. A non-NULL count array selects
 * combined 128-byte rows; NULL preserves standalone 120-byte rows. Counts
 * are keyed by incarnation, bounded by count, never a fabricated prefix. */
int pfp_core_sections(const void *, const void *, const pfp_record_chunks *, uint32_t,
                      void *, size_t, size_t *);
#endif
