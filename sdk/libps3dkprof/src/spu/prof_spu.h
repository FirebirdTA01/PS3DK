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
    uint32_t spu_lanes_max;       /* 0 = disabled (then every field but size must be 0); 1..64 */
    uint32_t spu_ring_slots;      /* a power of two, 64..1048576; 0 = 8192 */
    uint32_t spu_image_bytes_max; /* 4096..16777216; 0 = 65536 */
    uint64_t spu_mem_max;         /* the cap on the EXTENSION subtotal of the init block; 0 = 16 MiB */
    uint32_t reserved[4];         /* all 0, else EINVAL */
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
_Static_assert(offsetof(ps3dk_prof_ext_config, reserved) == 24, "ext config offset");
#endif

/* The same gate, results and states as ps3dk_prof_init.  ext NULL, or a disabled ext, takes the unchanged v1 path:
 * the same block layout and the same bytes on the wire (file_flags 0). */
int ps3dk_prof_init_ex(const ps3dk_prof_config *cfg, const ps3dk_prof_ext_config *ext);

#ifdef __cplusplus
}
#endif

#endif
