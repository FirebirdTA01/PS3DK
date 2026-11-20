/* Fixed-capacity rolling capture. APPEND is the default; no implicit limits.
 * The fixed-state limit covers the core/extension/writer block, not OS stacks,
 * separately attached AUTO/FIFO pools, or bounded retention scratch. */
#ifndef PS3DK_PROF_ROLLING_H
#define PS3DK_PROF_ROLLING_H
#include <ps3dk/prof.h>

#define PS3DK_PROF_ROLLING_APPEND 0u
#define PS3DK_PROF_ROLLING_ENABLED 1u
#define PS3DK_PROF_ROLLING_PROFILE_NONE 0u
#define PS3DK_PROF_ROLLING_PROFILE_PPU 1u
#define PS3DK_PROF_ROLLING_PROFILE_SPU 2u
#define PS3DK_PROF_ROLLING_PROFILE_AUTO_FIFO 3u
#define PS3DK_PROF_ROLLING_NONE 0u
#define PS3DK_PROF_ROLLING_OPEN 1u
#define PS3DK_PROF_ROLLING_SEALED 2u
#define PS3DK_PROF_ROLLING_STOPPED 3u
#define PS3DK_PROF_ROLLING_REASON_NONE 0u
#define PS3DK_PROF_ROLLING_CHECKPOINT_BYTES 1u
#define PS3DK_PROF_ROLLING_STATE_BYTES 2u
#define PS3DK_PROF_ROLLING_IDENTITIES 3u
#define PS3DK_PROF_ROLLING_SUPPORT 4u
#define PS3DK_PROF_ROLLING_COUNTER_RANGE 5u

typedef struct ps3dk_prof_rolling_options {
    uint32_t size, version, mode, profile;
    uint64_t max_capture_bytes, max_fixed_state_bytes, prepare_work_limit;
    uint32_t retained_segments, checkpoint_bytes, segment_payload_bytes, metadata_bytes;
    uint32_t incarnations, descriptors, ppu_stack_rows, frame_identities;
    uint32_t spu_domains, spu_lanes, spu_images, epochs, brackets, image_descriptors;
    uint32_t journal_entries, event_runs, live_spu_stack_rows, saved_spu_witnesses;
    uint32_t rsx_domains, windows, fifo_membership_runs, accounting_groups;
    uint32_t auto_final_runs, auto_touched_runs, auto_build_entries, auto_clock_anchors;
    uint32_t auto_bootstrap_bytes, auto_support_bytes;
    uint32_t reserved[10];
} ps3dk_prof_rolling_options;

typedef struct ps3dk_prof_rolling_result {
    uint32_t size, version, format, profile, original_flags, state, reason, reserved0;
    uint64_t accepted_next_chunk, accepted_payload_bytes, committed_next_chunk, committed_payload_bytes;
    uint64_t revision, file_bytes, fixed_state_bytes;
    int32_t result, io_result, close_result, join_result;
    uint32_t reserved[6];
} ps3dk_prof_rolling_result;

#define PS3DK_PROF_ROLLING_OPTIONS_INIT \
    {192u,1u,0u,0u,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,{0}}
/* Finite preset: six retained segments plus one spare. Raising only the byte
 * ceiling does not enlarge the file or the semantic support pools. epochs and
 * journal_entries also serve RSX clock-domain records, not only SPU records. */
#define PS3DK_PROF_ROLLING_AUTO_FIFO_INIT(file_limit,fixed_limit) \
    {192u,1u,1u,3u,(file_limit),(fixed_limit),UINT64_C(536870912), \
     6u,2097152u,1048596u,1048576u,256u,1024u,1024u,4096u, \
     0u,0u,0u,256u,0u,0u,1024u,0u,0u,0u,1u,256u,1024u,256u, \
     256u,256u,256u,1024u,4096u,524288u,{0}}

#ifdef __cplusplus
extern "C" {
#endif
/* Before init only; copies on success, preserves prior options on refusal. */
int ps3dk_prof_set_rolling_options(const ps3dk_prof_rolling_options *);
/* Initialize size=128/version=1. Available only after shutdown has joined and
 * published its immutable result; leaves the destination unchanged on refusal.
 * SEALED alone does not prove success: result includes close/join/retention. */
int ps3dk_prof_get_rolling_result(ps3dk_prof_rolling_result *);
#ifdef __cplusplus
}
#define PS3DK_PROF_DEFINE_AUTOMATIC_ROLLING_OPTIONS(...) \
    extern "C" const ps3dk_prof_rolling_options ps3dk_prof_automatic_rolling_options = __VA_ARGS__
#else
#define PS3DK_PROF_DEFINE_AUTOMATIC_ROLLING_OPTIONS(...) \
    const ps3dk_prof_rolling_options ps3dk_prof_automatic_rolling_options = __VA_ARGS__
#endif
/* A constant executable definition, read by AUTO constructor100. No constructor
 * or dynamic initialization in the application. Keep it out of static archives
 * unless explicitly forced into the executable; weak references do not extract. */
#if (defined(PS3DK_PROF) && PS3DK_PROF) || (defined(PS3TC_PROF_AUTO) && PS3TC_PROF_AUTO)
#define PS3DK_PROF_SET_ROLLING_OPTIONS(p) ps3dk_prof_set_rolling_options(p)
#define PS3DK_PROF_GET_ROLLING_RESULT(p) ps3dk_prof_get_rolling_result(p)
#else
#define PS3DK_PROF_SET_ROLLING_OPTIONS(p) PS3DK_PROF_ENOTSUP
#define PS3DK_PROF_GET_ROLLING_RESULT(p) PS3DK_PROF_ENOTSUP
#endif
#endif
