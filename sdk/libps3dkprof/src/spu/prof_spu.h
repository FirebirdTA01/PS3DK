/* The SPU lane extension's configuration (P3.2 integration, slice I1): INTERNAL and host-only in this checkpoint.
 * It is not installed under sdk/include and no target archive contains it; the runtime compiles the extension only
 * with PS3DK_PROF_WITH_SPU, which only host builds define.  Slice I1 provides init, config validation and the memory
 * layout; there is no lane API yet, so an enabled capture is a valid EMPTY extension file (spec r6).
 *
 * ps3dk_prof_config is unchanged.  ps3dk_prof_init(cfg) is ps3dk_prof_init_ex(cfg, NULL). */
#ifndef PS3DK_PROF_SPU_EXT_H
#define PS3DK_PROF_SPU_EXT_H

#include <stddef.h>
#include <stdint.h>

#include <ps3dk/prof.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3dk_prof_ext_config {
    uint32_t size;                /* exactly sizeof(ps3dk_prof_ext_config) = 40, else EINVAL */
    uint32_t spu_lanes_max;       /* 0 = SPU lanes disabled (then spu_ring_slots and spu_image_bytes_max must be 0); 1..64 */
    uint32_t spu_ring_slots;      /* a power of two, 64..1048576; 0 = 8192 */
    uint32_t spu_image_bytes_max; /* 4096..16777216; 0 = 65536 */
    uint64_t spu_mem_max;         /* the cap on the EXTENSION subtotal of the init block (SPU and GPU); 0 = 16 MiB;
                                   * nonzero only when an extension is enabled */
    uint32_t gpu_domains_max;     /* P3.3 R2: 0 = GPU domains disabled (then gpu_report_slots_max must be 0); 1..4 */
    uint32_t gpu_report_slots_max; /* 0 = 256, else 2..1024: the largest report_count a GPU domain may attach */
    uint32_t reserved[2];         /* all 0, else EINVAL */
} ps3dk_prof_ext_config;

#define PS3DK_PROF_EXT_CONFIG_SIZE 40u

#if defined(__cplusplus)
static_assert(sizeof(ps3dk_prof_ext_config) == PS3DK_PROF_EXT_CONFIG_SIZE, "ext config size");
#else
_Static_assert(sizeof(ps3dk_prof_ext_config) == PS3DK_PROF_EXT_CONFIG_SIZE, "ext config size");
_Static_assert(offsetof(ps3dk_prof_ext_config, spu_lanes_max) == 4, "ext config offset");
_Static_assert(offsetof(ps3dk_prof_ext_config, spu_ring_slots) == 8, "ext config offset");
_Static_assert(offsetof(ps3dk_prof_ext_config, spu_image_bytes_max) == 12, "ext config offset");
_Static_assert(offsetof(ps3dk_prof_ext_config, spu_mem_max) == 16, "ext config offset");
_Static_assert(offsetof(ps3dk_prof_ext_config, gpu_domains_max) == 24, "ext config offset");
_Static_assert(offsetof(ps3dk_prof_ext_config, gpu_report_slots_max) == 28, "ext config offset");
_Static_assert(offsetof(ps3dk_prof_ext_config, reserved) == 32, "ext config offset");
#endif

/* ---- slice I2: the lane API (PPU side, host only) ----
 * States: ENOTINIT before init; ENOTSUP when initialized without lanes; EBUSY inside a drain or a sink callback
 * (checked before any lock); ESTOPPED after a terminal extension fault, and for admissions after the shutdown claim
 * or the seal.  Outputs are written only on success, except that an operation whose mutation happened before its
 * unlock failed still reports that mutation (with ESTOPPED): a lane handle returned that way is diagnostic
 * ownership information, NOT permission to start a producer (it will never be drained).
 * Callbacks must not wait for another thread that is inside one of these calls. */
#include "psp_desc.h" /* struct ps3dk_prof_spu_desc: the one type the runtime's descriptors use too */
/* Capacity: an image's descriptors take n * sizeof(ps3dk_prof_spu_desc) bytes of the spu_image_bytes_max arena (16
 * bytes each in ILP32, 24 in LP64: two pointers) plus their strings, so one arena holds fewer descriptors in LP64.
 * The SPU-side records the ELF path decodes are 16 bytes in both data models. */
typedef struct ps3dk_prof_spu_desc ps3dk_prof_spu_desc;

typedef struct ps3dk_prof_spu_lane {
    uint32_t handle;     /* the lane_id: 1..1024, never reused in a capture */
    uint32_t ring_slots;
    uint64_t ea_ring, ea_ctl;
} ps3dk_prof_spu_lane;

typedef struct ps3dk_prof_spu_lane_stats {
    /* FROZEN_AT_FINAL: what the file states (counts of ACCEPTED chunks; final_written only for an accepted FINAL) */
    uint32_t final_written, fin_state;
    uint64_t next_seq, events, lost;
    /* NOT_IN_FILE: the producer's line-B counters read after a successful join, only from lane_end */
    uint32_t have_post_join, post_state;
    uint64_t post_next_seq, post_older_lost, post_refused;
    uint64_t stale, cancelled, unstable;
} ps3dk_prof_spu_lane_stats;

#if defined(PS3DK_PROF) && PS3DK_PROF

/* The same gate, results and states as ps3dk_prof_init.  ext NULL, or a disabled ext, takes the unchanged v1 path:
 * the same block layout and the same bytes on the wire (file_flags 0). */
int ps3dk_prof_init_ex(const ps3dk_prof_config *cfg, const ps3dk_prof_ext_config *ext);
/* Copies the table (all-or-nothing). EFULL: 32 images; EINVAL: a bad table; ENOMEM: the image arena is full. */
int ps3dk_prof_spu_image_register_table(const uint8_t *build_id, uint32_t build_len, const ps3dk_prof_spu_desc *descs,
                                        uint32_t n, uint32_t *image);
/* The SPU program's RAW ELF FILE BYTES (an embedspu blob or a file in memory, not a sys_spu_image_t): record i of its
 * ps3dkprof_spudesc section becomes descriptor i, the index the SPU writer computes.  The bytes must stay alive and
 * unchanged for the call (no concurrent writer); the descriptor strings and the build identity are copied (the ELF
 * itself is not kept), so the caller may free it afterwards.  EINVAL: any ELF rule of the design, nothing
 * registered; otherwise the results of register_table.  Strings: printable ASCII 0x20..0x7E (stricter than the table
 * path, which is unchanged); the build id is 0x01 + the GNU build id, or 0x02 + FNV-1a-64 of the whole file. */
int ps3dk_prof_spu_image_register_elf(const void *elf, uint64_t size, uint32_t *image);
/* owner_self: 1 = the calling thread's live incarnation (EINVAL if it has none), 0 = NONE.  EFULL: lane limits. */
int ps3dk_prof_spu_lane_create(const char *name, uint32_t image, int owner_self, ps3dk_prof_spu_lane *out);
/* After a SUCCESSFUL join of the SPU thread group that ran the lane.  timeout_us is a polling deadline only: it does
 * not bound the mutex, a sink callback or an inline drain.  0 = one poll, no sleep.  OK or ESTOPPED (sealed) once
 * the slot is released; ETIMEDOUT keeps it charged (call again); ECLOCK: no monotonic clock; EINVAL: a stale handle. */
int ps3dk_prof_spu_lane_end(uint32_t handle, uint64_t timeout_us, ps3dk_prof_spu_lane_stats *final_or_null);
int ps3dk_prof_spu_lane_get_stats(uint32_t handle, ps3dk_prof_spu_lane_stats *st);

#define PS3DK_PROF_INIT_EX(cfg, ext) ps3dk_prof_init_ex((cfg), (ext))
#define PS3DK_PROF_SPU_IMAGE_REGISTER_TABLE(b, l, d, n, out) ps3dk_prof_spu_image_register_table((b), (l), (d), (n), (out))
#define PS3DK_PROF_SPU_IMAGE_REGISTER_ELF(e, s, out) ps3dk_prof_spu_image_register_elf((e), (s), (out))
#define PS3DK_PROF_SPU_LANE_CREATE(name, image, self, out) ps3dk_prof_spu_lane_create((name), (image), (self), (out))
#define PS3DK_PROF_SPU_LANE_END(h, t, out) ps3dk_prof_spu_lane_end((h), (t), (out))
#define PS3DK_PROF_SPU_LANE_STATS(h, st) ps3dk_prof_spu_lane_get_stats((h), (st))

#else /* off build: nothing runs and no argument is evaluated */

#define PS3DK_PROF_INIT_EX(cfg, ext) ((void)sizeof(cfg), (void)sizeof(ext), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_SPU_IMAGE_REGISTER_TABLE(b, l, d, n, out)                                                          \
    ((void)sizeof(b), (void)sizeof(l), (void)sizeof(d), (void)sizeof(n), (void)sizeof(out), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_SPU_IMAGE_REGISTER_ELF(e, s, out) ((void)sizeof(e), (void)sizeof(s), (void)sizeof(out), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_SPU_LANE_CREATE(name, image, self, out)                                                           \
    ((void)sizeof(name), (void)sizeof(image), (void)sizeof(self), (void)sizeof(out), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_SPU_LANE_END(h, t, out) ((void)sizeof(h), (void)sizeof(t), (void)sizeof(out), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_SPU_LANE_STATS(h, st) ((void)sizeof(h), (void)sizeof(st), PS3DK_PROF_ENOTSUP)

#endif

#ifdef __cplusplus
}
#endif

#endif
