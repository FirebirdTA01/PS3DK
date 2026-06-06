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

static const char *const psgl_ffp_posonly_vp_source =
    "void main(\n"
    "    float3 in_position      : POSITION,\n"
    "    float4 in_color         : COLOR0,\n"
    "    uniform float4x4 modelViewProj,\n"
    "    out float4 out_position : POSITION,\n"
    "    out float4 out_color    : COLOR0)\n"
    "{\n"
    "    out_position = mul(modelViewProj, float4(in_position, 1.0f));\n"
    "    out_color = in_color;\n"
    "}\n";

static const char *const psgl_ffp_posonly_fp_source =
    "void main(\n"
    "    float4 in_color : COLOR0,\n"
    "    out float4 out_color : COLOR)\n"
    "{\n"
    "    out_color = in_color;\n"
    "}\n";

static const char *const psgl_ffp_lit_bootstrap_vp_source =
    "void main(\n"
    "    float3 in_position      : POSITION,\n"
    "    float4 in_normal        : NORMAL,\n"
    "    uniform float4x4 modelViewProj,\n"
    "    out float4 out_position : POSITION,\n"
    "    out float4 out_color    : COLOR0)\n"
    "{\n"
    "    out_position = mul(modelViewProj, float4(in_position, 1.0f));\n"
    "    out_color = in_normal;\n"
    "}\n";

static const char *const psgl_ffp_lit_bootstrap_fp_source =
    "void main(\n"
    "    float4 normal_color : COLOR0,\n"
    "    uniform float3 lightDirection,\n"
    "    out float4 out_color : COLOR)\n"
    "{\n"
    "    out_color = dot(normal_color.rgb, lightDirection);\n"
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
    if ((mask & PSGL_FFP_LIGHTING_MASK) != 0u) {
        if ((mask & PSGL_FFP_FOG_MASK) != 0u) return 0;
        return psgl_emit_copy(vp_source, vp_capacity,
                              psgl_ffp_lit_bootstrap_vp_source) &&
               psgl_emit_copy(fp_source, fp_capacity,
                              psgl_ffp_lit_bootstrap_fp_source);
    }
    if (mask == PSGL_FFP_POSITION_ONLY_MASK) {
        return psgl_emit_copy(vp_source, vp_capacity,
                              psgl_ffp_posonly_vp_source) &&
               psgl_emit_copy(fp_source, fp_capacity,
                              psgl_ffp_posonly_fp_source);
    }
    if (mask != PSGL_FFP_MINIMAL_MASK) return 0;
    return psgl_emit_copy(vp_source, vp_capacity, psgl_ffp_minimal_vp_source) &&
           psgl_emit_copy(fp_source, fp_capacity, psgl_ffp_minimal_fp_source);
}
