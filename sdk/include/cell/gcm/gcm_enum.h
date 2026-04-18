/*
 * PS3 Custom Toolchain — <cell/gcm/gcm_enum.h>
 *
 * Sony-style spellings of the libgcm constants our PSL1GHT runtime
 * layer exposes under plain `GCM_` names.  Adds the `CELL_GCM_`
 * prefix so Sony-authored sample code compiles unchanged; numerics
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
 * only exposes a subset (U8, F32); Sony's extra tags (S1, SF, CMP,
 * S32K, UB256) land when we own rsx/gcm_sys.h — for now we only alias
 * what the runtime can actually handle. */
#define CELL_GCM_VERTEX_UB               GCM_VERTEX_DATA_TYPE_U8
#define CELL_GCM_VERTEX_F                GCM_VERTEX_DATA_TYPE_F32

/* Surface / framebuffer format + target tags (cellGcmSetSurface). */
#define CELL_GCM_SURFACE_A8R8G8B8        GCM_SURFACE_A8R8G8B8
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

/* Depth/stencil formats — PSL1GHT uses the ZETA infix; Sony omits it. */
#define CELL_GCM_SURFACE_Z16             GCM_SURFACE_ZETA_Z16
#define CELL_GCM_SURFACE_Z24S8           GCM_SURFACE_ZETA_Z24S8

/* Surface addressing mode.  Sony calls the row-major layout "PITCH";
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

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_GCM_GCM_ENUM_H */
