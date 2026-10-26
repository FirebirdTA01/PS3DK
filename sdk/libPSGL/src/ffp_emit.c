#include "ffp_emit.h"

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

/* Bounded string builder for the generated programs. */
typedef struct PSGLemitBuffer {
    char *data;
    size_t capacity;
    size_t length;
    int ok;
} PSGLemitBuffer;

static void psgl_emit_put(PSGLemitBuffer *out, const char *text)
{
    while (out->ok && *text) {
        if (out->length + 1u >= out->capacity) {
            out->ok = 0;
            break;
        }
        out->data[out->length++] = *text++;
    }
    if (out->data && out->capacity) out->data[out->ok ? out->length : 0u] = '\0';
}

/* Writes text with every '#' replaced by the unit digit. */
static void psgl_emit_unit(PSGLemitBuffer *out, const char *text, uint32_t unit)
{
    char digit[2] = {(char)('0' + unit), '\0'};
    char piece[2] = {'\0', '\0'};
    for (; out->ok && *text; text++) {
        if (*text == '#') {
            psgl_emit_put(out, digit);
        } else {
            piece[0] = *text;
            psgl_emit_put(out, piece);
        }
    }
}

uint32_t psgl_ffp_mask_texture_units(uint32_t mask)
{
    if (mask == PSGL_FFP_MINIMAL_MASK) return 1u;
    if ((mask & ~PSGL_FFP_TEXUNITS_MASK) != 0u) return 0u;
    return (mask & PSGL_FFP_TEXUNITS_MASK) >> PSGL_FFP_TEXUNIT_SHIFT;
}

/* One program pair per enabled-unit set (OpenGL 2.1 sections 2.11.2 and
   3.13).  The vertex program passes the primary colour through and applies
   each enabled unit's texture matrix to its texcoord set.  The fragment
   program applies the enabled units in order; every stage computes

       C = saturate(Cp*m0 + Ct*m1 + Cp*Ct*m2 + Ct*m3 + (Ct - Cp)*At*m4)

   per channel, with Cp the previous stage's colour (the primary colour for
   the first stage) and Ct the sampled texel.  The CPU picks m0..m4 for each
   unit's mode and base format (tables 3.22 and 3.23), with the env colour
   folded into m3, so one program serves every mode. */
static int psgl_emit_texture_programs(uint32_t units,
                                      char *vp_source, size_t vp_capacity,
                                      char *fp_source, size_t fp_capacity)
{
    PSGLemitBuffer vp = {vp_source, vp_capacity, 0u, vp_source && vp_capacity};
    PSGLemitBuffer fp = {fp_source, fp_capacity, 0u, fp_source && fp_capacity};
    uint32_t unit;

    psgl_emit_put(&vp, "void main(\n"
                       "    float3 in_position      : POSITION,\n"
                       "    float4 in_color         : COLOR0,\n");
    for (unit = 0u; unit < PSGL_FFP_TEXUNIT_COUNT; unit++)
        if (units & (1u << unit))
            psgl_emit_unit(&vp, "    float4 in_texcoord#     : TEXCOORD#,\n", unit);
    psgl_emit_put(&vp, "    uniform float4x4 modelViewProj,\n");
    for (unit = 0u; unit < PSGL_FFP_TEXUNIT_COUNT; unit++)
        if (units & (1u << unit))
            psgl_emit_unit(&vp, "    uniform float4x4 texMatrix#,\n", unit);
    psgl_emit_put(&vp, "    out float4 out_position : POSITION,\n"
                       "    out float4 out_color    : COLOR0");
    for (unit = 0u; unit < PSGL_FFP_TEXUNIT_COUNT; unit++)
        if (units & (1u << unit))
            psgl_emit_unit(&vp, ",\n    out float4 out_texcoord#    : TEXCOORD#", unit);
    psgl_emit_put(&vp, ")\n{\n"
                       "    out_position = mul(modelViewProj, float4(in_position, 1.0f));\n"
                       "    out_color = in_color;\n");
    for (unit = 0u; unit < PSGL_FFP_TEXUNIT_COUNT; unit++)
        if (units & (1u << unit))
            psgl_emit_unit(&vp, "    out_texcoord# = mul(texMatrix#, in_texcoord#);\n", unit);
    psgl_emit_put(&vp, "}\n");

    psgl_emit_put(&fp, "void main(\n"
                       "    float4 in_color : COLOR0,\n");
    for (unit = 0u; unit < PSGL_FFP_TEXUNIT_COUNT; unit++)
        if (units & (1u << unit))
            psgl_emit_unit(&fp,
                "    float4 texcoord# : TEXCOORD#,\n"
                "    uniform sampler2D texunit# : TEXUNIT#,\n"
                "    uniform float4 texenv#_m0,\n"
                "    uniform float4 texenv#_m1,\n"
                "    uniform float4 texenv#_m2,\n"
                "    uniform float4 texenv#_m3,\n"
                "    uniform float4 texenv#_m4,\n", unit);
    psgl_emit_put(&fp, "    out float4 out_color : COLOR)\n"
                       "{\n"
                       "    float4 c = in_color;\n"
                       "    float4 t;\n");
    for (unit = 0u; unit < PSGL_FFP_TEXUNIT_COUNT; unit++)
        if (units & (1u << unit))
            psgl_emit_unit(&fp,
                "    t = tex2D(texunit#, texcoord#.xy / texcoord#.w);\n"
                "    c = saturate(c * texenv#_m0 + t * texenv#_m1 + c * t * texenv#_m2 +\n"
                "                 t * texenv#_m3 + (t - c) * t.wwww * texenv#_m4);\n", unit);
    psgl_emit_put(&fp, "    out_color = c;\n"
                       "}\n");
    if (!vp.ok || !fp.ok) {
        if (vp_source && vp_capacity) vp_source[0] = '\0';
        if (fp_source && fp_capacity) fp_source[0] = '\0';
        return 0;
    }
    return 1;
}

int psgl_ffp_state_mask_to_cg(uint32_t mask,
                              char *vp_source, size_t vp_capacity,
                              char *fp_source, size_t fp_capacity)
{
    uint32_t units;
    if ((mask & PSGL_FFP_LIGHTING_MASK) != 0u) {
        if ((mask & (PSGL_FFP_FOG_MASK | PSGL_FFP_TEXUNITS_MASK)) != 0u) return 0;
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
    units = psgl_ffp_mask_texture_units(mask);
    if (!units) return 0;
    return psgl_emit_texture_programs(units, vp_source, vp_capacity,
                                      fp_source, fp_capacity);
}
