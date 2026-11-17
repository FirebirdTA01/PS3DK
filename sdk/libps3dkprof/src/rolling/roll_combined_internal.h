/* Private shared PPU/SPU core; not installed or a public SDK API.
 * Public schema1 entry points/layout structs and emitted bytes stay intact. */
#ifndef PS3DK_PROF_ROLL_COMBINED_INTERNAL_H
#define PS3DK_PROF_ROLL_COMBINED_INTERNAL_H
#include "roll_combined.h"
#include "roll_ppu_internal.h"
#define pfm_core_plan ps3dk_prof__roll_combined_core_plan
#define pfm_core_init ps3dk_prof__roll_combined_core_init
#define pfm_core_accept ps3dk_prof__roll_combined_core_accept
#define pfm_core_work ps3dk_prof__roll_combined_core_work
#define pfm_core_snapshot ps3dk_prof__roll_combined_core_snapshot
#define pfm_core_sections ps3dk_prof__roll_combined_core_sections
#define pfm_core_rsx_declared ps3dk_prof__roll_combined_core_rsx_declared
#define pfm_core_bind_rsx ps3dk_prof__roll_combined_core_bind_rsx
#define pfm_core_declared ps3dk_prof__roll_combined_core_declared
#define pfm_core_window ps3dk_prof__roll_combined_core_window
typedef struct {
    pfm_config base;
    uint32_t family_flags, rsx_domains;
} pfm_core_config;
enum { PFM_EXTERNAL_EVENT=1, PFM_EXTERNAL_APPEND=2, PFM_EXTERNAL_LATEST=3 };
typedef struct { uint32_t type, storage; } pfm_external;
typedef struct {
    pfp_view ppu;
    uint64_t ppu_record_chunks, spu_event_chunks, spu_events;
    uint32_t journal_entries;
    size_t sections_bytes;
} pfm_core_view;
typedef struct { uint64_t first_frame,last_frame; uint32_t closed; } pfm_window;
int pfm_core_plan(const pfm_core_config *,pfm_layout *);
int pfm_core_init(void *,size_t,const pfm_core_config *,const uint8_t[16],void *,size_t);
/* Whole genuine chunk; external NULL selects the shared typed validators.
 * Non-NULL means the envelope owner already validated the entire external
 * family payload/references. Core framing/bootstrap checks remain mandatory.
 * No checkpoint emitted; successful TRAILER alone sets terminal. */
int pfm_core_accept(void *,const void *,void *,const void *,size_t,
                     const pfm_external *,uint64_t,pfo_prepared *);
/* Fixed-field-only conservative quote, charged before CRC or state traversal.
 * Includes core clone/validation/snapshot/common-section encoding and their
 * output/CRC cost. Caller adds its family/query/extra-envelope reservation,
 * then passes the remaining total allowance to accept. Arithmetic overflow
 * returns PFO_RESOURCE (STATE_BYTES); invalid fixed shape returns INVALID. */
int pfm_core_work(const void *,const void *,size_t,uint64_t *);
int pfm_core_snapshot(const void *,const void *,pfm_core_view *);
/* Actual sections2..4, with support ordinals resolved in current metadata.
 * Caller has reserved work and output bytes before calling this function. */
int pfm_core_sections(const void *,const void *,void *,size_t,size_t *);
/* Read queries: 1 present, 0 absent, INVALID invalid context. */
int pfm_core_rsx_declared(const void *,const void *,uint32_t);
int pfm_core_declared(const void *,const void *,uint32_t);
/* Only after an accepted valid CONFIG, and only on prepared successor state. */
int pfm_core_bind_rsx(const void *,void *,uint32_t);
/* OK with validated original window; INVALID absent/out-of-range/context.
 * Open windows have closed=0,last_frame=0. Closed lookup permits late rows;
 * the family owner applies its actual row/window membership constraints. */
int pfm_core_window(const void *,const void *,uint64_t,pfm_window *);
#endif
