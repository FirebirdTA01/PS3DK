#ifndef SD_INPUT_COVERAGE_H
#define SD_INPUT_COVERAGE_H
#include <stdint.h>
static uint32_t sd_vp_output_channel(unsigned dest, uint32_t output_mask)
{
    /* TEX9 shares result register 6 with PSIZE/CLP3..5. Its output-mask
     * bit selects the texture interpretation; a PSIZE write alone does
     * not supply TEX9. TEX8 is register 15, not a mask-bit continuation. */
    if (dest == 0) return 1u;
    if (dest == 1 || dest == 2) return 1u << (10 + dest);
    if (dest == 5) return 1u << 13;
    if (dest == 6 && (output_mask & (1u << 13))) return 1u << 10;
    if (dest >= 7 && dest <= 15) return 1u << (dest - 6);
    return 0;
}
static uint32_t sd_missing_input_channels(uint32_t fp_attributes, uint32_t vp_channels)
{
    /* Convert Cg's FP attribute contract to the rig's channel numbering:
     * cov=0, TEX0..9=1..10, COL0/1=11/12, FOG=13. TEX8/9 use attribute
     * bits 12/13, not the continuation of TEX0..7 at bits 14..21.
     * Colour front/back aliases share an interpolated input here (the
     * rig disables two-sided lighting). Unknown bits never imply coverage.
     * This is a necessary whole-channel check, not proof that every lane
     * is defined on every control-flow path of a vertex program. */
    uint32_t required = ((fp_attributes >> 14) & 0xffu) << 1;
    required |= ((fp_attributes >> 12) & 3u) << 9;
    if (fp_attributes & 5u) required |= 1u << 11;
    if (fp_attributes & 10u) required |= 1u << 12;
    if (fp_attributes & 16u) required |= 1u << 13;
    if (fp_attributes & ~0x003ff01fu) return 0x80000000u;
    return required & ~vp_channels;
}
#endif
