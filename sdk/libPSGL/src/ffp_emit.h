#ifndef PS3DK_LIBPSGL_FFP_EMIT_H
#define PS3DK_LIBPSGL_FFP_EMIT_H

#include <stddef.h>
#include <stdint.h>

#define PSGL_FFP_MINIMAL_MASK 0x00000001u
#define PSGL_FFP_LIGHTING_MASK 0x00000002u
#define PSGL_SHADER_LIBRARY_VERSION 1u

int psgl_ffp_state_mask_to_cg(uint32_t mask,
                              char *vp_source, size_t vp_capacity,
                              char *fp_source, size_t fp_capacity);

#endif
