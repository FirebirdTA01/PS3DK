/*
 * PS3 Custom Toolchain — <cell/gcm/gcm_enum.h>
 *
 * Cell-SDK-style spellings of the libgcm constants our PSL1GHT runtime
 * layer exposes under plain `GCM_` names.  Adds the `CELL_GCM_`
 * prefix so cell-SDK-authored sample code compiles unchanged; numerics
 * are preserved via macro aliases.
 *
 * This header stays header-only — no functions, no struct defs.
 * Pull in <rsx/gcm_sys.h> (which our <cell/gcm.h> already does) to
 * get the underlying GCM_* constants referenced below.
 */

#ifndef PS3TC_CELL_GCM_GCM_ENUM_H
#define PS3TC_CELL_GCM_GCM_ENUM_H

#include <rsx/gcm_sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boolean spellings (GCM uses TRUE/FALSE as ints). */
#ifndef CELL_GCM_TRUE
#define CELL_GCM_TRUE                    GCM_TRUE
#endif
#ifndef CELL_GCM_FALSE
#define CELL_GCM_FALSE                   GCM_FALSE
#endif

/* Memory-region tags passed to SetVertexDataArray / SetTexture / etc. */
#define CELL_GCM_LOCATION_LOCAL          GCM_LOCATION_RSX
#define CELL_GCM_LOCATION_MAIN           GCM_LOCATION_CELL

/* Surface / framebuffer color-clear masks (cellGcmSetClearSurface). */
#define CELL_GCM_CLEAR_R                 GCM_CLEAR_R
#define CELL_GCM_CLEAR_G                 GCM_CLEAR_G
#define CELL_GCM_CLEAR_B                 GCM_CLEAR_B
#define CELL_GCM_CLEAR_A                 GCM_CLEAR_A
#define CELL_GCM_CLEAR_Z                 GCM_CLEAR_Z
#define CELL_GCM_CLEAR_S                 GCM_CLEAR_S
#define CELL_GCM_CLEAR_M                 GCM_CLEAR_M

/* Color-mask bits (cellGcmSetColorMask / cellGcmSetColorMaskMrt). */
#define CELL_GCM_COLOR_MASK_R            GCM_COLOR_MASK_R
#define CELL_GCM_COLOR_MASK_G            GCM_COLOR_MASK_G
#define CELL_GCM_COLOR_MASK_B            GCM_COLOR_MASK_B
#define CELL_GCM_COLOR_MASK_A            GCM_COLOR_MASK_A

/* Depth-comparison functions (cellGcmSetDepthFunc). */
#define CELL_GCM_NEVER                   GCM_NEVER
#define CELL_GCM_LESS                    GCM_LESS
#define CELL_GCM_EQUAL                   GCM_EQUAL
#define CELL_GCM_LEQUAL                  GCM_LEQUAL
#define CELL_GCM_GREATER                 GCM_GREATER
#define CELL_GCM_NOTEQUAL                GCM_NOTEQUAL
#define CELL_GCM_GEQUAL                  GCM_GEQUAL
#define CELL_GCM_ALWAYS                  GCM_ALWAYS

/* Primitive types for cellGcmSetDrawArrays / DrawIndexArray. */
#define CELL_GCM_PRIMITIVE_POINTS        GCM_TYPE_POINTS
#define CELL_GCM_PRIMITIVE_LINES         GCM_TYPE_LINES
#define CELL_GCM_PRIMITIVE_LINE_LOOP     GCM_TYPE_LINE_LOOP
#define CELL_GCM_PRIMITIVE_LINE_STRIP    GCM_TYPE_LINE_STRIP
#define CELL_GCM_PRIMITIVE_TRIANGLES     GCM_TYPE_TRIANGLES
#define CELL_GCM_PRIMITIVE_TRIANGLE_STRIP GCM_TYPE_TRIANGLE_STRIP
#define CELL_GCM_PRIMITIVE_TRIANGLE_FAN  GCM_TYPE_TRIANGLE_FAN
#define CELL_GCM_PRIMITIVE_QUADS         GCM_TYPE_QUADS
#define CELL_GCM_PRIMITIVE_QUAD_STRIP    GCM_TYPE_QUAD_STRIP
#define CELL_GCM_PRIMITIVE_POLYGON       GCM_TYPE_POLYGON

/* Vertex-attribute element type (cellGcmSetVertexDataArray).  PSL1GHT
 * only exposes a subset (U8, F32); the cell SDK's extra tags (S1, SF,
 * CMP, S32K, UB256) land when we own rsx/gcm_sys.h — for now we only
 * alias what the runtime can actually handle. */
#define CELL_GCM_VERTEX_UB               GCM_VERTEX_DATA_TYPE_U8
#define CELL_GCM_VERTEX_F                GCM_VERTEX_DATA_TYPE_F32
/* Additional cell-SDK-named vertex-data types verified against the
 * reference cell SDK's gcm_enum.h.  PSL1GHT only aliased F32=2 and
 * U8=4 — NV40 supports more element kinds. */
#define CELL_GCM_VERTEX_S1               (1)   /* signed 16-bit normalised */
#define CELL_GCM_VERTEX_SF               (3)   /* half float (16-bit) */
#define CELL_GCM_VERTEX_S32K             (5)   /* signed 16-bit scaled "keep" */
#define CELL_GCM_VERTEX_CMP              (6)   /* s11_11_10 packed */
#define CELL_GCM_VERTEX_UB256            (7)   /* unsigned byte * 256 */
/* Long-form aliases matching the reference cell SDK's alt names. */
#define CELL_GCM_VERTEX_S16_NR           CELL_GCM_VERTEX_S1
#define CELL_GCM_VERTEX_F32              CELL_GCM_VERTEX_F
#define CELL_GCM_VERTEX_F16              CELL_GCM_VERTEX_SF
#define CELL_GCM_VERTEX_U8_NR            CELL_GCM_VERTEX_UB
#define CELL_GCM_VERTEX_S16_UN           CELL_GCM_VERTEX_S32K
#define CELL_GCM_VERTEX_S11_11_10_NR     CELL_GCM_VERTEX_CMP
#define CELL_GCM_VERTEX_U8_UN            CELL_GCM_VERTEX_UB256

/* Draw-index array element size (cellGcmSetDrawIndexArray). */
#define CELL_GCM_DRAW_INDEX_ARRAY_TYPE_32   (0)
#define CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16   (1)

/* Polygon-fill mode (cellGcmSetFront/BackPolygonMode). */
#define CELL_GCM_POLYGON_MODE_POINT      (0x1B00)
#define CELL_GCM_POLYGON_MODE_LINE       (0x1B01)
#define CELL_GCM_POLYGON_MODE_FILL       (0x1B02)

/* Debug-output verbosity tags (cellGcmSetDebugOutputLevel). */
#define CELL_GCM_DEBUG_LEVEL0            (0)
#define CELL_GCM_DEBUG_LEVEL1            (1)
#define CELL_GCM_DEBUG_LEVEL2            (2)

/* Additional tile / surface alignment constants beyond what's aliased
 * further below.  Values match PSL1GHT's GCM_* (verified). */
#define CELL_GCM_TILE_ALIGN_BUFFER_START_BOUNDARY  GCM_TILE_ALIGN_BUFFER_START_BOUNDARY
#define CELL_GCM_TILE_LOCAL_ALIGN_HEIGHT           GCM_TILE_LOCAL_ALIGN_HEIGHT
#define CELL_GCM_TILE_MAIN_ALIGN_HEIGHT            GCM_TILE_MAIN_ALIGN_HEIGHT
#define CELL_GCM_ZCULL_ALIGN_OFFSET                GCM_ZCULL_ALIGN_OFFSET
#define CELL_GCM_ZCULL_Z16                         GCM_ZCULL_Z16
#define CELL_GCM_ZCULL_Z24S8                       2
#define CELL_GCM_TEXTURE_SWIZZLED_CUBEMAP_FACE_ALIGN_OFFSET \
    GCM_TEXTURE_SWIZZLED_CUBEMAP_FACE_ALIGN_OFFSET

/* libgcm error codes (cell-SDK numbering — out-of-band from
 * CELL_OK / sysmodule errors). */
#define CELL_GCM_ERROR_FAILURE                  0x802b0001
#define CELL_GCM_ERROR_INVALID_VALUE            0x802b0002
#define CELL_GCM_ERROR_NOT_SUPPORTED            0x802b0003
#define CELL_GCM_SURFACE_LINEAR_ALIGN_OFFSET       GCM_SURFACE_LINEAR_ALIGN_OFFSET

/* Texture remap field helper — the cell SDK builds the u32 remap value
 * inline via this macro; PSL1GHT exposes individual bit constants and
 * the user assembles them.  Provide the cell-SDK helper verbatim — it
 * just packs its nine 2-bit args into the canonical layout. */
#define CELL_GCM_REMAP_MODE(order, inputA, inputR, inputG, inputB, outputA, outputR, outputG, outputB) \
    (((order) << 16) | ((inputA)) | ((inputR) << 2) | ((inputG) << 4) | \
     ((inputB) << 6) | ((outputA) << 8) | ((outputR) << 10) | \
     ((outputG) << 12) | ((outputB) << 14))

/* Surface / framebuffer format + target tags (cellGcmSetSurface). */
#define CELL_GCM_SURFACE_A8R8G8B8        GCM_SURFACE_A8R8G8B8
#define CELL_GCM_SURFACE_B8              GCM_SURFACE_B8
#define CELL_GCM_SURFACE_G8B8            GCM_SURFACE_G8B8

/* PSL1GHT mis-spells reference NV40-tag 3 as "R5G5B5"; the correct
 * reference name is R5G6B5 (5 red / 6 green / 5 blue), and the same
 * applies to format 5 which the reference SDK calls X8R8G8B8_O8R8G8B8
 * (overlay second-buffer), not bare X8R8G8B8.  Add both to the global
 * (non-CELL-prefixed) namespace so cell-SDK source that references
 * the bare GCM_* name finds it. */
#ifndef GCM_SURFACE_R5G6B5
#  define GCM_SURFACE_R5G6B5             3
#endif
#ifndef GCM_SURFACE_X8R8G8B8_O8R8G8B8
#  define GCM_SURFACE_X8R8G8B8_O8R8G8B8  5
#endif

#define CELL_GCM_SURFACE_R5G6B5          GCM_SURFACE_R5G6B5
#define CELL_GCM_SURFACE_F_W16Z16Y16X16  GCM_SURFACE_F_W16Z16Y16X16
#define CELL_GCM_SURFACE_F_W32Z32Y32X32  GCM_SURFACE_F_W32Z32Y32X32
#define CELL_GCM_SURFACE_X1R5G5B5_Z1R5G5B5 GCM_SURFACE_X1R5G5B5_Z1R5G5B5
#define CELL_GCM_SURFACE_X1R5G5B5_O1R5G5B5 GCM_SURFACE_X1R5G5B5_O1R5G5B5
#define CELL_GCM_SURFACE_X8R8G8B8_Z8R8G8B8 GCM_SURFACE_X8R8G8B8_Z8R8G8B8
#define CELL_GCM_SURFACE_X8R8G8B8_O8R8G8B8 GCM_SURFACE_X8R8G8B8_O8R8G8B8
#define CELL_GCM_SURFACE_X8B8G8R8_Z8B8G8R8 GCM_SURFACE_X8B8G8R8_Z8B8G8R8
#define CELL_GCM_SURFACE_X8B8G8R8_O8B8G8R8 GCM_SURFACE_X8B8G8R8_O8B8G8R8
#define CELL_GCM_SURFACE_A8B8G8R8        GCM_SURFACE_A8B8G8R8
#define CELL_GCM_SURFACE_F_X16Z16Y16X16  GCM_SURFACE_F_X16Z16Y16X16
#define CELL_GCM_SURFACE_F_X32           GCM_SURFACE_F_X32

/* Depth/stencil formats — PSL1GHT uses the ZETA infix; the cell SDK omits it. */
#define CELL_GCM_SURFACE_Z16             GCM_SURFACE_ZETA_Z16
#define CELL_GCM_SURFACE_Z24S8           GCM_SURFACE_ZETA_Z24S8

/* Surface addressing mode.  The cell SDK calls the row-major layout "PITCH";
 * PSL1GHT exposes it as TYPE_LINEAR.  Semantics identical. */
#define CELL_GCM_SURFACE_PITCH           GCM_SURFACE_TYPE_LINEAR
#define CELL_GCM_SURFACE_SWIZZLE         GCM_SURFACE_TYPE_SWIZZLE

#define CELL_GCM_SURFACE_TARGET_NONE     GCM_SURFACE_TARGET_NONE
#define CELL_GCM_SURFACE_TARGET_0        GCM_SURFACE_TARGET_0
#define CELL_GCM_SURFACE_TARGET_1        GCM_SURFACE_TARGET_1
#define CELL_GCM_SURFACE_TARGET_MRT1     GCM_SURFACE_TARGET_MRT1
#define CELL_GCM_SURFACE_TARGET_MRT2     GCM_SURFACE_TARGET_MRT2
#define CELL_GCM_SURFACE_TARGET_MRT3     GCM_SURFACE_TARGET_MRT3

/* Antialias centering mode (cellGcmSetSurface.antialias). */
#define CELL_GCM_SURFACE_CENTER_1        GCM_SURFACE_CENTER_1
#define CELL_GCM_SURFACE_CENTER_2        GCM_SURFACE_CENTER_2
#define CELL_GCM_SURFACE_DIAGONAL_CENTERED_2 GCM_SURFACE_DIAGONAL_CENTERED_2
#define CELL_GCM_SURFACE_SQUARE_CENTERED_4 GCM_SURFACE_SQUARE_CENTERED_4
#define CELL_GCM_SURFACE_SQUARE_ROTATED_4 GCM_SURFACE_SQUARE_ROTATED_4

/* Flip-mode tag (cellGcmSetFlipMode). */
#define CELL_GCM_DISPLAY_HSYNC           GCM_FLIP_HSYNC
#define CELL_GCM_DISPLAY_VSYNC           GCM_FLIP_VSYNC

/* Depth-cull (Zcull) alignment (cellGcmBindZcull / GCM tile setup). */
#define CELL_GCM_ZCULL_ALIGN_WIDTH       GCM_ZCULL_ALIGN_WIDTH
#define CELL_GCM_ZCULL_ALIGN_HEIGHT      GCM_ZCULL_ALIGN_HEIGHT

/* Tile region alignment (cellGcmSetTileInfo / BindTile). */
#define CELL_GCM_TILE_ALIGN_OFFSET       GCM_TILE_ALIGN_OFFSET
#define CELL_GCM_TILE_ALIGN_SIZE         GCM_TILE_ALIGN_SIZE

/* Fragment-program ucode placement alignment.  Cell-SDK samples align
 * the local-memory allocation for FP ucode to this offset when
 * calling cellGcmUtilAllocateLocalMemory. */
#define CELL_GCM_FRAGMENT_UCODE_LOCAL_ALIGN_OFFSET  GCM_FRAGMENT_UCODE_LOCAL_ALIGN_OFFSET

/* Z-cull (early-depth) configuration.  The handle_systemmenu cell-SDK
 * sample binds these via cellGcmBindZcull. */
#define CELL_GCM_ZCULL_Z24S8             GCM_ZCULL_Z24S8
#define CELL_GCM_ZCULL_LESS              GCM_ZCULL_LESS
#define CELL_GCM_ZCULL_LONES             GCM_ZCULL_LONES
#define CELL_GCM_SCULL_SFUNC_LESS        GCM_SCULL_SFUNC_LESS

/* Additional flip modes.  HSYNC_WITH_NOISE is the cell-SDK name for what
 * PSL1GHT calls HSYNC_AND_BREAK_EVERYTHING (both == 3). */
#define CELL_GCM_DISPLAY_HSYNC_WITH_NOISE  GCM_FLIP_HSYNC_AND_BREAK_EVERYTHING

/* Tile compression mode (cellGcmSetTileInfo). */
#define CELL_GCM_COMPMODE_DISABLED              GCM_COMPMODE_DISABLED
#define CELL_GCM_COMPMODE_Z32_SEPSTENCIL        GCM_COMPMODE_Z32_SEPSTENCIL
#define CELL_GCM_COMPMODE_Z32_SEPSTENCIL_REGULAR  GCM_COMPMODE_Z32_SEPSTENCIL_REGULAR
#define CELL_GCM_COMPMODE_Z32_SEPSTENCIL_DIAGONAL GCM_COMPMODE_Z32_SEPSTENCIL_DIAGONAL
#define CELL_GCM_COMPMODE_Z32_SEPSTENCIL_ROTATED  GCM_COMPMODE_Z32_SEPSTENCIL_ROTATED

/* ============================================================
 * Texture — cellGcmSetTexture / SetTextureControl /
 *           SetTextureFilter / SetTextureAddress.
 *
 * Numeric values are NV40-hardware constants that the reference
 * Cg compiler and libgcm encode the same way as our PSL1GHT runtime
 * (when PSL1GHT has an equivalent GCM_TEXTURE_* it's aliased,
 * otherwise the value is written out directly).
 * ============================================================ */

/* Texture pixel formats (cellGcmSetTexture.format low byte). */
#define CELL_GCM_TEXTURE_B8                      0x81
#define CELL_GCM_TEXTURE_A1R5G5B5                0x82
#define CELL_GCM_TEXTURE_A4R4G4B4                0x83
#define CELL_GCM_TEXTURE_R5G6B5                  0x84
#define CELL_GCM_TEXTURE_A8R8G8B8                0x85
#define CELL_GCM_TEXTURE_COMPRESSED_DXT1         0x86
#define CELL_GCM_TEXTURE_COMPRESSED_DXT23        0x87
#define CELL_GCM_TEXTURE_COMPRESSED_DXT45        0x88
#define CELL_GCM_TEXTURE_G8B8                    0x8B
#define CELL_GCM_TEXTURE_R6G5B5                  0x8F
#define CELL_GCM_TEXTURE_DEPTH24_D8              0x90
#define CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT        0x91
#define CELL_GCM_TEXTURE_DEPTH16                 0x92
#define CELL_GCM_TEXTURE_DEPTH16_FLOAT           0x93
#define CELL_GCM_TEXTURE_X16                     0x94
#define CELL_GCM_TEXTURE_Y16_X16                 0x95
#define CELL_GCM_TEXTURE_R5G5B5A1                0x97
#define CELL_GCM_TEXTURE_COMPRESSED_HILO8        0x98
#define CELL_GCM_TEXTURE_COMPRESSED_HILO_S8      0x99
#define CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT   0x9A
#define CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT   0x9B
#define CELL_GCM_TEXTURE_X32_FLOAT               0x9C
#define CELL_GCM_TEXTURE_D1R5G5B5                0x9D
#define CELL_GCM_TEXTURE_D8R8G8B8                0x9E
#define CELL_GCM_TEXTURE_Y16_X16_FLOAT           0x9F

/* Format modifiers ORed into the format byte. */
#define CELL_GCM_TEXTURE_SZ                      0x00
#define CELL_GCM_TEXTURE_LN                      0x20
#define CELL_GCM_TEXTURE_NR                      0x00
#define CELL_GCM_TEXTURE_UN                      0x40

/* Texture dimension (1D/2D/3D/cube). */
#define CELL_GCM_TEXTURE_DIMENSION_1             1
#define CELL_GCM_TEXTURE_DIMENSION_2             2
#define CELL_GCM_TEXTURE_DIMENSION_3             3

/* Remap field encodings (cellGcmSetTexture.remap).
 * remap is a u32 built from (FROM_A|FROM_R|FROM_G|FROM_B) at shifts
 * 0/2/4/6 and the per-channel REMAP_{ZERO,ONE,REMAP} at shifts 8..14. */
#define CELL_GCM_TEXTURE_REMAP_ORDER_XYXY        0
#define CELL_GCM_TEXTURE_REMAP_ORDER_XXXY        1
#define CELL_GCM_TEXTURE_REMAP_FROM_A            0
#define CELL_GCM_TEXTURE_REMAP_FROM_R            1
#define CELL_GCM_TEXTURE_REMAP_FROM_G            2
#define CELL_GCM_TEXTURE_REMAP_FROM_B            3
#define CELL_GCM_TEXTURE_REMAP_ZERO              0
#define CELL_GCM_TEXTURE_REMAP_ONE               1
#define CELL_GCM_TEXTURE_REMAP_REMAP             2

#define CELL_GCM_TEXTURE_BORDER_TEXTURE          0
#define CELL_GCM_TEXTURE_BORDER_COLOR            1

/* Texture filter (cellGcmSetTextureFilter). */
#define CELL_GCM_TEXTURE_NEAREST                 1
#define CELL_GCM_TEXTURE_LINEAR                  2
#define CELL_GCM_TEXTURE_NEAREST_NEAREST         3
#define CELL_GCM_TEXTURE_LINEAR_NEAREST          4
#define CELL_GCM_TEXTURE_NEAREST_LINEAR          5
#define CELL_GCM_TEXTURE_LINEAR_LINEAR           6
#define CELL_GCM_TEXTURE_CONVOLUTION_MIN         7
#define CELL_GCM_TEXTURE_CONVOLUTION_MAG         4
#define CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX    1
#define CELL_GCM_TEXTURE_CONVOLUTION_GAUSSIAN    2
#define CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX_ALT 3

/* Texture address modes (cellGcmSetTextureAddress). */
#define CELL_GCM_TEXTURE_WRAP                         1
#define CELL_GCM_TEXTURE_MIRROR                       2
#define CELL_GCM_TEXTURE_CLAMP_TO_EDGE                3
#define CELL_GCM_TEXTURE_BORDER                       4
#define CELL_GCM_TEXTURE_CLAMP                        5
#define CELL_GCM_TEXTURE_MIRROR_ONCE_CLAMP_TO_EDGE    6
#define CELL_GCM_TEXTURE_MIRROR_ONCE_BORDER           7
#define CELL_GCM_TEXTURE_MIRROR_ONCE_CLAMP            8

#define CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL   0
#define CELL_GCM_TEXTURE_UNSIGNED_REMAP_FORCE    1

#define CELL_GCM_TEXTURE_ZFUNC_NEVER             0
#define CELL_GCM_TEXTURE_ZFUNC_LESS              1
#define CELL_GCM_TEXTURE_ZFUNC_EQUAL             2
#define CELL_GCM_TEXTURE_ZFUNC_LEQUAL            3
#define CELL_GCM_TEXTURE_ZFUNC_GREATER           4
#define CELL_GCM_TEXTURE_ZFUNC_NOTEQUAL          5
#define CELL_GCM_TEXTURE_ZFUNC_GEQUAL            6
#define CELL_GCM_TEXTURE_ZFUNC_ALWAYS            7

#define CELL_GCM_TEXTURE_GAMMA_R                 0x01
#define CELL_GCM_TEXTURE_GAMMA_G                 0x02
#define CELL_GCM_TEXTURE_GAMMA_B                 0x04
#define CELL_GCM_TEXTURE_GAMMA_A                 0x08
#define CELL_GCM_TEXTURE_GAMMA_NONE              0x00
#define CELL_GCM_TEXTURE_GAMMA_RGB               0x07
#define CELL_GCM_TEXTURE_GAMMA_RGBA              0x0F

#define CELL_GCM_TEXTURE_MAX_ANISO_1             0
#define CELL_GCM_TEXTURE_MAX_ANISO_2             1
#define CELL_GCM_TEXTURE_MAX_ANISO_4             2
#define CELL_GCM_TEXTURE_MAX_ANISO_6             3
#define CELL_GCM_TEXTURE_MAX_ANISO_8             4
#define CELL_GCM_TEXTURE_MAX_ANISO_10            5
#define CELL_GCM_TEXTURE_MAX_ANISO_12            6
#define CELL_GCM_TEXTURE_MAX_ANISO_16            7

/* Shade model (NV40 shade-mode method arg). */
#define CELL_GCM_FLAT                            0x1D00
#define CELL_GCM_SMOOTH                          0x1D01

/* Texture-cache invalidation flag (cellGcmSetInvalidateTextureCache arg). */
#define CELL_GCM_INVALIDATE_TEXTURE              1
#define CELL_GCM_INVALIDATE_VERTEX_TEXTURE       2

/* Blend factors / equations / logic ops (NV40 method args).
 * Values match PSL1GHT's GCM_* defines in <rsx/gcm_sys.h>. */
#define CELL_GCM_ZERO                            0x0000
#define CELL_GCM_ONE                             0x0001
#define CELL_GCM_SRC_COLOR                       0x0300
#define CELL_GCM_ONE_MINUS_SRC_COLOR             0x0301
#define CELL_GCM_SRC_ALPHA                       0x0302
#define CELL_GCM_ONE_MINUS_SRC_ALPHA             0x0303
#define CELL_GCM_DST_ALPHA                       0x0304
#define CELL_GCM_ONE_MINUS_DST_ALPHA             0x0305
#define CELL_GCM_DST_COLOR                       0x0306
#define CELL_GCM_ONE_MINUS_DST_COLOR             0x0307
#define CELL_GCM_SRC_ALPHA_SATURATE              0x0308
#define CELL_GCM_CONSTANT_COLOR                  0x8001
#define CELL_GCM_ONE_MINUS_CONSTANT_COLOR        0x8002
#define CELL_GCM_CONSTANT_ALPHA                  0x8003
#define CELL_GCM_ONE_MINUS_CONSTANT_ALPHA        0x8004

#define CELL_GCM_FUNC_ADD                        0x8006
#define CELL_GCM_MIN                             0x8007
#define CELL_GCM_MAX                             0x8008
#define CELL_GCM_FUNC_SUBTRACT                   0x800a
#define CELL_GCM_FUNC_REVERSE_SUBTRACT           0x800b
#define CELL_GCM_FUNC_REVERSE_SUBTRACT_SIGNED    0x0000f005
#define CELL_GCM_FUNC_ADD_SIGNED                 0x0000f006
#define CELL_GCM_FUNC_REVERSE_ADD_SIGNED         0x0000f007

/* Logic-op opcodes (NV40 method args). */
#define CELL_GCM_CLEAR                           0x1500
#define CELL_GCM_AND                             0x1501
#define CELL_GCM_AND_REVERSE                     0x1502
#define CELL_GCM_COPY                            0x1503
#define CELL_GCM_AND_INVERTED                    0x1504
#define CELL_GCM_NOOP                            0x1505
#define CELL_GCM_XOR                             0x1506
#define CELL_GCM_OR                              0x1507
#define CELL_GCM_NOR                             0x1508
#define CELL_GCM_EQUIV                           0x1509
#define CELL_GCM_INVERT                          0x150a
#define CELL_GCM_OR_REVERSE                      0x150b
#define CELL_GCM_COPY_INVERTED                   0x150c
#define CELL_GCM_OR_INVERTED                     0x150d
#define CELL_GCM_NAND                            0x150e
#define CELL_GCM_SET                             0x150f

/* Front-face direction (cellGcmSetFrontFace). */
#define CELL_GCM_CW                              GCM_FRONTFACE_CW
#define CELL_GCM_CCW                             GCM_FRONTFACE_CCW

/* Cull-face mode (cellGcmSetCullFace). */
#define CELL_GCM_FRONT                           GCM_CULL_FRONT
#define CELL_GCM_BACK                            GCM_CULL_BACK
#define CELL_GCM_FRONT_AND_BACK                  GCM_CULL_ALL

/* Frequency-divider operation (cellGcmSetFrequencyDividerOperation).
 * MODULO mode: per-attribute frequency = vertex_index / divisor;
 * used to bind joint matrices that update per-instance, not per-vertex. */
#define CELL_GCM_FREQUENCY_DIVIDE                0
#define CELL_GCM_FREQUENCY_MODULO                GCM_FREQUENCY_MODULO

/* Z-cull / S-cull surface-function tags (cellGcmSetScullControl). */
#define CELL_GCM_SCULL_SFUNC_ALWAYS              GCM_SCULL_SFUNC_ALWAYS
#define CELL_GCM_SCULL_SFUNC_NEVER               0
#define CELL_GCM_SCULL_SFUNC_LESS_THAN           1
#define CELL_GCM_SCULL_SFUNC_LESS_OR_EQUAL       2
#define CELL_GCM_SCULL_SFUNC_EQUAL               3
#define CELL_GCM_SCULL_SFUNC_GREATER_OR_EQUAL    4
#define CELL_GCM_SCULL_SFUNC_GREATER_THAN        5
#define CELL_GCM_SCULL_SFUNC_NOT_EQUAL           6

/* Vertex-program attribute output mask bits — TEX0..7 are the
 * texture-coord output channels VP writes that FP picks up via
 * TEXCOORD<N> bindings. */
#define CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE   GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE
#define CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTSPECULAR  GCM_ATTRIB_OUTPUT_MASK_FRONTSPECULAR
#define CELL_GCM_ATTRIB_OUTPUT_MASK_BACKDIFFUSE    GCM_ATTRIB_OUTPUT_MASK_BACKDIFFUSE
#define CELL_GCM_ATTRIB_OUTPUT_MASK_BACKSPECULAR   GCM_ATTRIB_OUTPUT_MASK_BACKSPECULAR
#define CELL_GCM_ATTRIB_OUTPUT_MASK_FOG            GCM_ATTRIB_OUTPUT_MASK_FOG
#define CELL_GCM_ATTRIB_OUTPUT_MASK_POINTSIZE      GCM_ATTRIB_OUTPUT_MASK_POINTSIZE
#define CELL_GCM_ATTRIB_OUTPUT_MASK_UC0            GCM_ATTRIB_OUTPUT_MASK_UC0
#define CELL_GCM_ATTRIB_OUTPUT_MASK_UC1            GCM_ATTRIB_OUTPUT_MASK_UC1
#define CELL_GCM_ATTRIB_OUTPUT_MASK_UC2            GCM_ATTRIB_OUTPUT_MASK_UC2
#define CELL_GCM_ATTRIB_OUTPUT_MASK_UC3            GCM_ATTRIB_OUTPUT_MASK_UC3
#define CELL_GCM_ATTRIB_OUTPUT_MASK_UC4            GCM_ATTRIB_OUTPUT_MASK_UC4
#define CELL_GCM_ATTRIB_OUTPUT_MASK_UC5            GCM_ATTRIB_OUTPUT_MASK_UC5
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX0           GCM_ATTRIB_OUTPUT_MASK_TEX0
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX1           GCM_ATTRIB_OUTPUT_MASK_TEX1
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX2           GCM_ATTRIB_OUTPUT_MASK_TEX2
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX3           GCM_ATTRIB_OUTPUT_MASK_TEX3
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX4           GCM_ATTRIB_OUTPUT_MASK_TEX4
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX5           GCM_ATTRIB_OUTPUT_MASK_TEX5
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX6           (1<<20)
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX7           (1<<21)
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX8           GCM_ATTRIB_OUTPUT_MASK_TEX8
#define CELL_GCM_ATTRIB_OUTPUT_MASK_TEX9           GCM_ATTRIB_OUTPUT_MASK_TEX9

/* Conditional render / z-pass-pixel-count report tag
 * (cellGcmSetRenderEnable / cellGcmSetReport / cellGcmSetClearReport). */
#define CELL_GCM_CONDITIONAL                     GCM_CONDITIONAL
#define CELL_GCM_ZPASS_PIXEL_CNT                 GCM_ZPASS_PIXEL_CNT

/* Compression-mode tag (cellGcmSetTileInfo extra). */
#define CELL_GCM_COMPMODE_C32_2X2                GCM_COMPMODE_C32_2X2

/* Texture unsigned-remap tag (cellGcmSetTextureAddress). The reference
 * SDK adds a BIASED sampling mode the PSL1GHT enum doesn't name; the
 * NV40 register encodes it as bit 1 set, NORMAL is 0, FORCE is 1. */
#ifndef CELL_GCM_TEXTURE_UNSIGNED_REMAP_BIASED
#define CELL_GCM_TEXTURE_UNSIGNED_REMAP_BIASED   2
#endif

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_GCM_GCM_ENUM_H */
