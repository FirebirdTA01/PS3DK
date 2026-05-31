#include "ffp_emit.h"

static const char *const psgl_ffp_minimal_vp_source =
    "void main(\n"
    "    float3 in_position       : POSITION,\n"
    "    float2 in_texcoord       : TEXCOORD0,\n"
    "    uniform float4x4 modelViewProj,\n"
    "    out float4 out_position  : POSITION,\n"
    "    out float2 out_texcoord  : TEXCOORD0)\n"
    "{\n"
    "    out_position = mul(modelViewProj, float4(in_position, 1.0f));\n"
    "    out_texcoord = in_texcoord;\n"
    "}\n";

static const char *const psgl_ffp_minimal_fp_source =
    "void main(\n"
    "    float2 texcoord : TEXCOORD0,\n"
    "    uniform sampler2D diffuseMap : TEXUNIT0,\n"
    "    out float4 out_color : COLOR)\n"
    "{\n"
    "    out_color = tex2D(diffuseMap, texcoord);\n"
    "}\n";

static int psgl_emit_copy(char *dst, size_t capacity, const char *src)
{
    size_t i = 0u;
    if (!dst || capacity == 0u || !src) return 0;
    while (src[i] != '\0') {
        if (i + 1u >= capacity) {
            dst[0] = '\0';
            return 0;
        }
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return 1;
}

int psgl_ffp_state_mask_to_cg(uint32_t mask,
                              char *vp_source, size_t vp_capacity,
                              char *fp_source, size_t fp_capacity)
{
    if (mask != PSGL_FFP_MINIMAL_MASK) return 0;
    return psgl_emit_copy(vp_source, vp_capacity, psgl_ffp_minimal_vp_source) &&
           psgl_emit_copy(fp_source, fp_capacity, psgl_ffp_minimal_fp_source);
}
