/*
 * PS3 Custom Toolchain — minimal Cg runtime API.
 *
 * Ships the opaque handle typedefs and the ABI-visible enum values
 * that sce-cgc writes into CgBinaryProgram / CgBinaryParameter fields.
 * Client code uses these to decode what a loaded shader's parameters
 * bind to at the hardware level (e.g. `if (res == CG_ATTR0) ...`).
 *
 * Scope is deliberately narrow: this is not a full Cg runtime, just
 * the subset of identifiers that the cellGcmCg* surface exposes.
 * The numeric values are API ABI constants — they have to match the
 * compiler's output or parameter-resource comparisons would fail.
 *
 * Layout and comments are original to this file.  We picked plain C
 * enum declarations over NVIDIA's header-time X-macro expansion so
 * the values are immediately greppable.
 */

#ifndef PS3TC_CG_CG_H
#define PS3TC_CG_CG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque runtime handles ----------------------------------------- */
typedef void       *CGcontext;
typedef void       *CGprogram;
typedef const void *CGparameter;
typedef int32_t     CGbool;
#define CG_FALSE  0
#define CG_TRUE   1

/* ---- parameter data types (CgBinaryParameter.type) ------------------ */
/*
 * The compiler sits structs / arrays / undefined at the low end of the
 * namespace (values < CG_TYPE_START_ENUM), then lays out all scalar/
 * vector/matrix/sampler types sequentially beginning at 1024.  Each
 * family is packed together: the Half block, then the Float block,
 * then samplers, Fixed, integer scalars (Half1/Float1/Fixed1), then
 * Int, Bool, and finally the handle / object types.
 */
enum
{
    CG_UNKNOWN_TYPE      = 0,
    CG_STRUCT            = 1,
    CG_ARRAY             = 2,
    CG_TYPELESS_STRUCT   = 3,

    /* CG_TYPE_START_ENUM itself takes the value 1024; the first
     * datatype (CG_HALF) is the NEXT enumerator and gets 1025.  This
     * is a subtle C-enum auto-increment detail — bite us once already
     * when every datatype constant below was off-by-one and sce-cgc-
     * emitted `type` fields failed to match the switch in the Cg →
     * RSX bridge. */
    CG_TYPE_START_ENUM   = 1024,

    /* Half family */
    CG_HALF              = 1025,
    CG_HALF2             = 1026,
    CG_HALF3             = 1027,
    CG_HALF4             = 1028,
    CG_HALF1x1           = 1029,
    CG_HALF1x2           = 1030,
    CG_HALF1x3           = 1031,
    CG_HALF1x4           = 1032,
    CG_HALF2x1           = 1033,
    CG_HALF2x2           = 1034,
    CG_HALF2x3           = 1035,
    CG_HALF2x4           = 1036,
    CG_HALF3x1           = 1037,
    CG_HALF3x2           = 1038,
    CG_HALF3x3           = 1039,
    CG_HALF3x4           = 1040,
    CG_HALF4x1           = 1041,
    CG_HALF4x2           = 1042,
    CG_HALF4x3           = 1043,
    CG_HALF4x4           = 1044,

    /* Float family */
    CG_FLOAT             = 1045,
    CG_FLOAT2            = 1046,
    CG_FLOAT3            = 1047,
    CG_FLOAT4            = 1048,
    CG_FLOAT1x1          = 1049,
    CG_FLOAT1x2          = 1050,
    CG_FLOAT1x3          = 1051,
    CG_FLOAT1x4          = 1052,
    CG_FLOAT2x1          = 1053,
    CG_FLOAT2x2          = 1054,
    CG_FLOAT2x3          = 1055,
    CG_FLOAT2x4          = 1056,
    CG_FLOAT3x1          = 1057,
    CG_FLOAT3x2          = 1058,
    CG_FLOAT3x3          = 1059,
    CG_FLOAT3x4          = 1060,
    CG_FLOAT4x1          = 1061,
    CG_FLOAT4x2          = 1062,
    CG_FLOAT4x3          = 1063,
    CG_FLOAT4x4          = 1064,

    /* Samplers */
    CG_SAMPLER1D         = 1065,
    CG_SAMPLER2D         = 1066,
    CG_SAMPLER3D         = 1067,
    CG_SAMPLERRECT       = 1068,
    CG_SAMPLERCUBE       = 1069,

    /* Fixed family */
    CG_FIXED             = 1070,
    CG_FIXED2            = 1071,
    CG_FIXED3            = 1072,
    CG_FIXED4            = 1073,
    CG_FIXED1x1          = 1074,
    CG_FIXED1x2          = 1075,
    CG_FIXED1x3          = 1076,
    CG_FIXED1x4          = 1077,
    CG_FIXED2x1          = 1078,
    CG_FIXED2x2          = 1079,
    CG_FIXED2x3          = 1080,
    CG_FIXED2x4          = 1081,
    CG_FIXED3x1          = 1082,
    CG_FIXED3x2          = 1083,
    CG_FIXED3x3          = 1084,
    CG_FIXED3x4          = 1085,
    CG_FIXED4x1          = 1086,
    CG_FIXED4x2          = 1087,
    CG_FIXED4x3          = 1088,
    CG_FIXED4x4          = 1089,

    /* 1-component spellings */
    CG_HALF1             = 1090,
    CG_FLOAT1            = 1091,
    CG_FIXED1            = 1092,

    /* Int family */
    CG_INT               = 1093,
    CG_INT1              = 1094,
    CG_INT2              = 1095,
    CG_INT3              = 1096,
    CG_INT4              = 1097,
    CG_INT1x1            = 1098,
    CG_INT1x2            = 1099,
    CG_INT1x3            = 1100,
    CG_INT1x4            = 1101,
    CG_INT2x1            = 1102,
    CG_INT2x2            = 1103,
    CG_INT2x3            = 1104,
    CG_INT2x4            = 1105,
    CG_INT3x1            = 1106,
    CG_INT3x2            = 1107,
    CG_INT3x3            = 1108,
    CG_INT3x4            = 1109,
    CG_INT4x1            = 1110,
    CG_INT4x2            = 1111,
    CG_INT4x3            = 1112,
    CG_INT4x4            = 1113,

    /* Bool family */
    CG_BOOL              = 1114,
    CG_BOOL1             = 1115,
    CG_BOOL2             = 1116,
    CG_BOOL3             = 1117,
    CG_BOOL4             = 1118,
    CG_BOOL1x1           = 1119,
    CG_BOOL1x2           = 1120,
    CG_BOOL1x3           = 1121,
    CG_BOOL1x4           = 1122,
    CG_BOOL2x1           = 1123,
    CG_BOOL2x2           = 1124,
    CG_BOOL2x3           = 1125,
    CG_BOOL2x4           = 1126,
    CG_BOOL3x1           = 1127,
    CG_BOOL3x2           = 1128,
    CG_BOOL3x3           = 1129,
    CG_BOOL3x4           = 1130,
    CG_BOOL4x1           = 1131,
    CG_BOOL4x2           = 1132,
    CG_BOOL4x3           = 1133,
    CG_BOOL4x4           = 1134,

    /* Handle / object types */
    CG_STRING            = 1135,
    CG_PROGRAM_TYPE      = 1136,
    CG_TEXTURE           = 1137,
    CG_SAMPLER1DARRAY    = 1138,
    CG_SAMPLER2DARRAY    = 1139,
    CG_SAMPLERCUBEARRAY  = 1140,
    CG_VERTEXSHADER_TYPE = 1141,
    CG_PIXELSHADER_TYPE  = 1142,
    CG_SAMPLER           = 1143
};
typedef uint32_t CGtype;

/* ---- parameter resource / bind locations (CgBinaryParameter.res) ---- */
/*
 * Tags where a parameter lives at the hardware level.  The compiler
 * assigns one to each parameter based on its `: SEMANTIC` annotation
 * and role (input / output / uniform / sampler).
 *
 * The layout is sparse — semantic groups (ATTRn, TEXn, CLPn, POSITIONn,
 * NORMALn, etc.) sit at well-known bases with 16 slots reserved per
 * group.  We only declare the groups we've seen used; if you need
 * NORMAL1..NORMAL15, TEXCOORD8..TEXCOORD31, etc., they follow the same
 * 1-step-per-index pattern from the bases enumerated below.
 */
enum
{
    /* Samplers */
    CG_TEXUNIT0          = 2048, CG_TEXUNIT1,  CG_TEXUNIT2,  CG_TEXUNIT3,
    CG_TEXUNIT4,  CG_TEXUNIT5,  CG_TEXUNIT6,  CG_TEXUNIT7,
    CG_TEXUNIT8,  CG_TEXUNIT9,  CG_TEXUNIT10, CG_TEXUNIT11,
    CG_TEXUNIT12, CG_TEXUNIT13, CG_TEXUNIT14, CG_TEXUNIT15,

    /* Uniform buffers */
    CG_BUFFER0           = 2064, CG_BUFFER1,  CG_BUFFER2,  CG_BUFFER3,
    CG_BUFFER4,  CG_BUFFER5,  CG_BUFFER6,  CG_BUFFER7,
    CG_BUFFER8,  CG_BUFFER9,  CG_BUFFER10, CG_BUFFER11,
    CG_BUFFER12, CG_BUFFER13, CG_BUFFER14, CG_BUFFER15,

    /* Vertex attributes — ATTRn */
    CG_ATTR0             = 2113, CG_ATTR1,  CG_ATTR2,  CG_ATTR3,
    CG_ATTR4,  CG_ATTR5,  CG_ATTR6,  CG_ATTR7,
    CG_ATTR8,  CG_ATTR9,  CG_ATTR10, CG_ATTR11,
    CG_ATTR12, CG_ATTR13, CG_ATTR14, CG_ATTR15,

    /* Constant bank */
    CG_C                 = 2178,

    /* Texcoord bind locations — note the 2182..2191 gap the compiler
     * skips when laying these out; TEX3 and above continue at 2192. */
    CG_TEX0              = 2179,
    CG_TEX1              = 2180,
    CG_TEX2              = 2181,
    CG_TEX3              = 2192, CG_TEX4, CG_TEX5, CG_TEX6, CG_TEX7,
    CG_TEX8,  CG_TEX9,

    /* Vertex-shader homogeneous position + per-vertex outputs */
    CG_HPOS              = 2243,
    CG_COL0              = 2245,
    CG_COL1              = 2246,

    /* Clip planes + point size */
    CG_PSIZ              = 2309,
    CG_CLP0              = 2310, CG_CLP1, CG_CLP2, CG_CLP3, CG_CLP4, CG_CLP5,

    /* Viewport-space fragment position + point coordinate */
    CG_WPOS              = 2373,
    CG_POINTCOORD        = 2374,

    /* Semantic groups — only a few landmark bases; callers that deal
     * with arbitrary indices derive them by adding the offset. */
    CG_POSITION0         = 2437,
    CG_DIFFUSE0          = 2501,
    CG_TANGENT0          = 2565,
    CG_SPECULAR0         = 2629,
    CG_BLENDINDICES0     = 2693,
    CG_COLOR0            = 2757,
    CG_NORMAL0           = 3092,
    CG_FOGCOORD          = 3156,

    CG_UNDEFINED         = 3256
};
typedef uint32_t CGresource;

/* ---- enum values used in CgBinaryParameter.var / .direction --------- */
/*
 * The compiler writes `CGenum` values here to describe parameter
 * variability (varying / uniform / literal / constant / mixed) and
 * direction (in / out / inout) as well as a few other flags we don't
 * currently act on.  Kept in one flat enum to match the ABI layout.
 */
enum
{
    CG_UNKNOWN                = 4096,
    CG_IN                     = 4097,
    CG_OUT                    = 4098,
    CG_INOUT                  = 4099,
    CG_MIXED                  = 4100,
    CG_VARYING                = 4101,
    CG_UNIFORM                = 4102,
    CG_CONSTANT               = 4103,
    CG_PROGRAM_SOURCE         = 4104,
    CG_PROGRAM_ENTRY          = 4105,
    CG_COMPILED_PROGRAM       = 4106,
    CG_PROGRAM_PROFILE        = 4107,
    CG_LITERAL                = 4113
};
typedef uint32_t CGenum;

/* ---- shader profile tags (CgBinaryProgram.profile) ------------------ */
/*
 * We only enumerate the profiles relevant to PS3 shader authoring;
 * legacy DX9 / HLSL / VP20–VP30 profiles would be laid out between
 * CG_PROFILE_START and CG_PROFILE_SCE_VP_RSX but are not accepted by
 * sce-cgc anyway.
 */
enum
{
    CG_PROFILE_START         = 6144,
    CG_PROFILE_UNKNOWN       = 6145,
    CG_PROFILE_VP40          = 7001,
    CG_PROFILE_GENERIC       = 7002,
    CG_PROFILE_SCE_VP_RSX    = 7003,
    CG_PROFILE_SCE_FP_RSX    = 7004,
    CG_PROFILE_MAX           = 7100
};
typedef uint32_t CGprofile;

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CG_CG_H */
