/* Opt-in automatic RSX observation. PS3_PROF_AUTO selects its startup and GCM
 * provider; application code does not need to call attach. These optional
 * owner APIs change defaults or explicitly close an associated coverage frame.
 * Draw/clear marker brackets describe reached-marker elapsed intervals. They
 * do not measure isolated execution time, GPU utilization or starvation. */
#ifndef PS3DK_PROF_AUTO_H
#define PS3DK_PROF_AUTO_H
#include <stdint.h>
#include <ps3dk/prof.h>
#include <ps3dk/prof_gpu.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3dk_prof_auto_options {
    uint32_t size;              /* sizeof(ps3dk_prof_auto_options) = 32 */
    uint32_t report_first;      /* local reports, first index 0..2047 */
    uint32_t report_count;      /* even, 18..1024, wholly within 0..2047 */
    uint32_t operation_label;   /* 64 or 69..254; distinct from clock_label */
    uint32_t clock_label;
    uint32_t metadata_records;  /* 1..1024 operation snapshots */
    uint32_t frame_records;     /* 1..256 coverage snapshots */
    uint32_t max_bytes;         /* actual checked layout must fit; <= 2 MiB */
} ps3dk_prof_auto_options;

#define PS3DK_PROF_AUTO_OPTIONS_INIT {32u,1024u,1024u,239u,238u,1024u,256u,2097152u}

/* Optional override, on the registered context owner before its first covered
 * operation, clock or frame observation. NULL options select the defaults.
 * The common SDK registry reserves the whole range and both labels; public
 * SDK conflicts stop observation while preserving the application command.
 * Raw/private command writers must honor these reservations and are outside
 * observable coverage. Invalid overrides leave the existing default intact.
 * No second instance, replacement CONFIG or serial reset is permitted.
 * Both outputs are zero on refusal. No allocation, marker or flush on a
 * refused option validation. Shutdown owns finalization and report quarantine;
 * there is no detach that can free still-referenced reports. */
int ps3dk_prof_auto_attach(ps3dk_prof_gcm_ctx *context, const ps3dk_prof_auto_options *options,
                          uint32_t *instance, uint32_t *domain);

/* Closes only this owner's active matching frame. It does not wait for the
 * RSX or prove GPU completion. Missing/foreign/duplicate closure refuses;
 * Reserved token fields must be zero. Opening a successor with ps3dk_prof_frame leaves an unclosed predecessor
 * explicitly open/unknown. A token's owner==0 is NONE; frame id zero is valid. */
int ps3dk_prof_auto_frame_end(uint32_t instance, ps3dk_prof_token frame);

#ifdef __cplusplus
}
#endif
#endif
