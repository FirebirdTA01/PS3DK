/* Private runtime representation, independent of host test hooks.
 * Not an installed options API or an instruction to enable rolling capture. */
#ifndef PS3DK_PROF_ROLL_RUNTIME_TYPES_H
#define PS3DK_PROF_ROLL_RUNTIME_TYPES_H
#include "roll_ppu.h"
#include "roll_combined.h"
#include "roll_combined2.h"

enum { P_ROLL_NONE, P_ROLL_PPU, P_ROLL_COMBINED, P_ROLL_SCHEMA2 };
typedef struct {
    uint32_t kind;
    union { pfm_config combined; pfm2_config schema2; } limits;
} p_roll_selection;

typedef struct {
    uint32_t size, version, enabled, reserved;
    /* Full fixed core/extension/rolling allocation; OS stacks and separately
     * attached producer pools are not included in this internal ceiling. */
    uint64_t max_state_bytes;
    pfr_config store;
    pfp_config ppu;
} p_roll_options;

typedef struct {
    uint32_t size, version, original_flags, reserved;
    uint64_t accepted_next_chunk, accepted_payload_bytes;
    uint64_t committed_next_chunk, committed_payload_bytes;
    uint64_t revision, file_bytes, state_bytes;
    uint32_t state, reason;
    int32_t result, io_result, close_result, join_result;
} p_roll_record;

enum { P_ROLL_RESOURCE = -33 };
#endif
