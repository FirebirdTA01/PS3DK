#ifndef PS3DK_LIBPSGL_FFP_EMIT_H
#define PS3DK_LIBPSGL_FFP_EMIT_H

#include <stddef.h>
#include <stdint.h>

#define PSGL_FFP_MINIMAL_MASK 0x00000001u
#define PSGL_FFP_LIGHTING_MASK 0x00000002u
#define PSGL_FFP_FOG_MASK 0x00000004u
#define PSGL_FFP_POSITION_ONLY_MASK 0x00000008u
/* Fixed-function texturing: bit 16+k is set when texture unit k (0-3) is
   enabled.  Each set selects one program; the texture environments are
   uniforms (psgl_ffp_texenv_coefficients), not part of the key.
   PSGL_FFP_MINIMAL_MASK alone is the unit-0 set under its original name. */
#define PSGL_FFP_TEXUNIT_SHIFT 16u
#define PSGL_FFP_TEXUNIT_COUNT 4u
#define PSGL_FFP_TEXUNITS_MASK 0x000f0000u
#define PSGL_FFP_TEXUNIT_MASK(unit) (1u << (PSGL_FFP_TEXUNIT_SHIFT + (unit)))
#define PSGL_SHADER_LIBRARY_VERSION 1u

int psgl_ffp_state_mask_to_cg(uint32_t mask,
                              char *vp_source, size_t vp_capacity,
                              char *fp_source, size_t fp_capacity);

/* The enabled-unit set a mask selects (bit k = unit k), or 0 when the mask
   is not a texturing mask. */
uint32_t psgl_ffp_mask_texture_units(uint32_t mask);

#endif
