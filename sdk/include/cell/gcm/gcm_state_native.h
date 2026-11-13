/* <cell/gcm/gcm_state_native.h> — shared NV40 state natives.
 * Single writer for the logic-op, depth, blend, dither, stencil, and
 * raster-state emitter pairs; each safe/unsafe pair owns one method
 * (or method sequence) so callers can no longer emit stale words.
 *
 * SAFE natives reserve via ps3tc_gcm_reserve (shared ring-full / callback
 * path, same as gcm_tex_bridge.h).  When (current + count) exceeds end and
 * no callback is registered, the reservation is refused: the native emits
 * nothing and current is left unchanged, so the safe path never writes past
 * the ring.  If a callback is present it may relocate current/end and report
 * success (resume the write) or refuse (no write).  UNSAFE natives
 * plain-write, no buffer check, no callback.
 *
 * Needs CellGcmContextData in scope (from <cell/gcm.h>, or a local
 * typedef in commands.c). */
#ifndef PS3TC_CELL_GCM_STATE_NATIVE_H
#define PS3TC_CELL_GCM_STATE_NATIVE_H

#include <rsx/nv40.h>
#include <cell/gcm/gcm_cg_bridge.h>

#ifdef __cplusplus
extern "C" {
#endif

/* safe: ring-full reserved via the shared helper. */
static inline void ps3tc_gcm_state_logic_op(CellGcmContextData *ctx, uint32_t op)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_COLOR_LOGIC_OP, 1);
    *w    = op;
}

/* unsafe: plain writes, no buffer check. */
static inline void ps3tc_gcm_state_logic_op_unsafe(CellGcmContextData *ctx, uint32_t op)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_COLOR_LOGIC_OP, 1);
    ctx->current[1] = op;
    ctx->current   += 2;
}

/* safe */
static inline void ps3tc_gcm_state_logic_op_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_COLOR_LOGIC_OP_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_logic_op_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_COLOR_LOGIC_OP_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* ================================================================== *
 * Depth group — NV40TCL_DEPTH_* and NV40TCL_DEPTH_BOUNDS_*.
 * Each emitter carries a single method header + data words.
 * SetDepthMask maps to depth_write_enable (== rsxSetDepthWriteEnable).
 * depth_bounds carries the zMin/zMax float BIT PATTERNS (IEEE bits),
 * not values; both data words are written under DEPTH_BOUNDS_MIN with
 * count=2 so the auto-increment lands zMax at DEPTH_BOUNDS_MAX.
 * =================================================================== */

/* safe */
static inline void ps3tc_gcm_state_depth_func(CellGcmContextData *ctx, uint32_t func)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_DEPTH_FUNC, 1);
    *w    = func;
}

/* unsafe */
static inline void ps3tc_gcm_state_depth_func_unsafe(CellGcmContextData *ctx, uint32_t func)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_DEPTH_FUNC, 1);
    ctx->current[1] = func;
    ctx->current   += 2;
}

/* safe */
static inline void ps3tc_gcm_state_depth_test_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_DEPTH_TEST_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_depth_test_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_DEPTH_TEST_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — the native behind both cellGcmSetDepthMask and rsxSetDepthWriteEnable. */
static inline void ps3tc_gcm_state_depth_write_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_DEPTH_WRITE_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_depth_write_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_DEPTH_WRITE_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe */
static inline void ps3tc_gcm_state_depth_bounds_test_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_DEPTH_BOUNDS_TEST_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_depth_bounds_test_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_DEPTH_BOUNDS_TEST_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — 3 words; zMin/zMax written as IEEE-32 bit patterns. */
static inline void ps3tc_gcm_state_depth_bounds(CellGcmContextData *ctx, f32 zMin, f32 zMax)
{
    ieee32 a, b;
    a.f = zMin; b.f = zMax;
    uint32_t *w = ps3tc_gcm_reserve(ctx, 3);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_DEPTH_BOUNDS_MIN, 2);
    *w++ = a.u;
    *w   = b.u;
}

/* unsafe — 3 words; IEEE-32 bit patterns, no buffer check. */
static inline void ps3tc_gcm_state_depth_bounds_unsafe(CellGcmContextData *ctx, f32 zMin, f32 zMax)
{
    ieee32 a, b;
    a.f = zMin; b.f = zMax;
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_DEPTH_BOUNDS_MIN, 2);
    ctx->current[1] = a.u;
    ctx->current[2] = b.u;
    ctx->current   += 3;
}

/* ================================================================== *
 * Fog group: single-value mode + float pair.
 * ================================================================== */

/* safe — fog_mode (FOG_MODE 0x8cc, count=1, 2 words). */
static inline void ps3tc_gcm_state_fog_mode(CellGcmContextData *ctx, uint32_t mode)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_FOG_MODE, 1);
    *w    = mode;
}

/* unsafe */
static inline void ps3tc_gcm_state_fog_mode_unsafe(CellGcmContextData *ctx, uint32_t mode)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_FOG_MODE, 1);
    ctx->current[1] = mode;
    ctx->current   += 2;
}

/* safe — fog_params (FOG_EQUATION_CONSTANT 0x8d0, count=2, 3 words).
 * p0/p1 are written as IEEE-32 bit patterns (same pattern as depth_bounds). */
static inline void ps3tc_gcm_state_fog_params(CellGcmContextData *ctx, f32 p0, f32 p1)
{
    ieee32 a, b;
    a.f = p0; b.f = p1;
    uint32_t *w = ps3tc_gcm_reserve(ctx, 3);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_FOG_EQUATION_CONSTANT, 2);
    *w++ = a.u;
    *w   = b.u;
}

/* unsafe */
static inline void ps3tc_gcm_state_fog_params_unsafe(CellGcmContextData *ctx, f32 p0, f32 p1)
{
    ieee32 a, b;
    a.f = p0; b.f = p1;
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_FOG_EQUATION_CONSTANT, 2);
    ctx->current[1] = a.u;
    ctx->current[2] = b.u;
    ctx->current   += 3;
}

/* ================================================================== *
 * Anti-aliasing / clip / FP / zminmax / freq-divider group.
 * ================================================================== */

/* safe — anti_aliasing_control (ANTI_ALIASING_CONTROL 0x1d7c, count=1, 2 words).
 * Packing: enable bits 0-3, alphaToCoverage bits 4-7, alphaToOne bits 8-15,
 * sampleMask bits 16-31 (identical to the pre-patch rsx* body). */
static inline void ps3tc_gcm_state_anti_aliasing_control(CellGcmContextData *ctx,
    uint32_t enable, uint32_t alpha_to_coverage, uint32_t alpha_to_one, uint32_t sample_mask)
{
    uint32_t val = ((sample_mask << 16) | (alpha_to_one << 8) | (alpha_to_coverage << 4) | enable);
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_ANTI_ALIASING_CONTROL, 1);
    *w    = val;
}

/* unsafe */
static inline void ps3tc_gcm_state_anti_aliasing_control_unsafe(CellGcmContextData *ctx,
    uint32_t enable, uint32_t alpha_to_coverage, uint32_t alpha_to_one, uint32_t sample_mask)
{
    uint32_t val = ((sample_mask << 16) | (alpha_to_one << 8) | (alpha_to_coverage << 4) | enable);
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_ANTI_ALIASING_CONTROL, 1);
    ctx->current[1] = val;
    ctx->current   += 2;
}

/* safe — user_clip_plane_control (CLIP_PLANE_ENABLE 0x1478, count=1, 2 words).
 * Each plane is a 4-bit enable, packed: plane0 bits 0-3 ... plane5 bits 20-23. */
static inline void ps3tc_gcm_state_user_clip_plane_control(CellGcmContextData *ctx,
    uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4, uint32_t p5)
{
    uint32_t val = ((p5 << 20) | (p4 << 16) | (p3 << 12) | (p2 << 8) | (p1 << 4) | p0);
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_CLIP_PLANE_ENABLE, 1);
    *w    = val;
}

/* unsafe */
static inline void ps3tc_gcm_state_user_clip_plane_control_unsafe(CellGcmContextData *ctx,
    uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4, uint32_t p5)
{
    uint32_t val = ((p5 << 20) | (p4 << 16) | (p3 << 12) | (p2 << 8) | (p1 << 4) | p0);
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_CLIP_PLANE_ENABLE, 1);
    ctx->current[1] = val;
    ctx->current   += 2;
}

/* safe — fragment_program_gamma_enable (FP_PACKER 0x1fec, count=1, 2 words). */
static inline void ps3tc_gcm_state_fragment_program_gamma_enable(CellGcmContextData *ctx,
                                                                  uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_FP_PACKER, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_fragment_program_gamma_enable_unsafe(CellGcmContextData *ctx,
                                                                         uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_FP_PACKER, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — zminmax_control (DEPTH_CONTROL 0x1d78, count=1, 2 words).
 * Packing: cullNearFar bits 0-3, zClampEnable bits 4-7, cullIgnoreW bits 8-11. */
static inline void ps3tc_gcm_state_zminmax_control(CellGcmContextData *ctx,
                                                   uint32_t cull_near_far,
                                                   uint32_t zclamp_enable,
                                                   uint32_t cull_ignore_w)
{
    uint32_t val = (cull_near_far | (zclamp_enable << 4) | (cull_ignore_w << 8));
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_DEPTH_CONTROL, 1);
    *w    = val;
}

/* unsafe */
static inline void ps3tc_gcm_state_zminmax_control_unsafe(CellGcmContextData *ctx,
                                                           uint32_t cull_near_far,
                                                           uint32_t zclamp_enable,
                                                           uint32_t cull_ignore_w)
{
    uint32_t val = (cull_near_far | (zclamp_enable << 4) | (cull_ignore_w << 8));
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_DEPTH_CONTROL, 1);
    ctx->current[1] = val;
    ctx->current   += 2;
}

/* safe — render_enable (RENDER_ENABLE 0x1e98).
 * Two shapes inherited from the pre-patch rsx* body (commands_impl.h:493-510):
 *   mode == GCM_CONDITIONAL: 4 words  [NOP, 0, RENDER_ENABLE, 0x2000000|offset]
 *   otherwise:                 2 words [RENDER_ENABLE, 0x1000000]
 * offset = 0x10 * index.  The reservation count differs (4 vs 2) per branch —
 * the safe native must reserve for the shape it takes.
 *
 * GCM_CONDITIONAL is the PPU enum (value 2, rsx/gcm_sys.h:681). We include
 * it here so the native stays self-contained; the forwarder's caller-facing
 * name is `enable` but the wire semantics is the `mode` selector. */
#ifndef PS3TC_GCM_RENDER_CONDITIONAL
#  define PS3TC_GCM_RENDER_CONDITIONAL 2u /* == GCM_CONDITIONAL, rsx/gcm_sys.h */
#endif
static inline void ps3tc_gcm_state_render_enable(CellGcmContextData *ctx,
                                                  uint8_t mode, uint32_t index)
{
    uint32_t offset = 0x10u * index;
    if (mode == PS3TC_GCM_RENDER_CONDITIONAL)
    {
        uint32_t *w = ps3tc_gcm_reserve(ctx, 4);
        if (!w) return;
        *w++ = PS3TC_GCM_METHOD(NV40TCL_NOP, 1);
        *w++ = 0;
        *w++ = PS3TC_GCM_METHOD(NV40TCL_RENDER_ENABLE, 1);
        *w   = (0x2000000u | offset);
    }
    else
    {
        uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
        if (!w) return;
        *w++ = PS3TC_GCM_METHOD(NV40TCL_RENDER_ENABLE, 1);
        *w   = 0x1000000u;
    }
}

/* unsafe — both branches written plainly. */
static inline void ps3tc_gcm_state_render_enable_unsafe(CellGcmContextData *ctx,
                                                         uint8_t mode, uint32_t index)
{
    uint32_t offset = 0x10u * index;
    if (mode == PS3TC_GCM_RENDER_CONDITIONAL)
    {
        ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_NOP, 1);
        ctx->current[1] = 0;
        ctx->current[2] = PS3TC_GCM_METHOD(NV40TCL_RENDER_ENABLE, 1);
        ctx->current[3] = (0x2000000u | offset);
        ctx->current   += 4;
    }
    else
    {
        ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_RENDER_ENABLE, 1);
        ctx->current[1] = 0x1000000u;
        ctx->current   += 2;
    }
}

/* safe — frequency_divider_operation (DIVIDER_FREQUENCY_OP 0x1fc0, count=1, 2 words).
 * op is a u16; the wire word is the value in the low 16 bits. */
static inline void ps3tc_gcm_state_frequency_divider_operation(CellGcmContextData *ctx,
                                                                uint16_t op)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_DIVIDER_FREQUENCY_OP, 1);
    *w   = (uint32_t)op;
}

/* unsafe */
static inline void ps3tc_gcm_state_frequency_divider_operation_unsafe(CellGcmContextData *ctx,
                                                                       uint16_t op)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_DIVIDER_FREQUENCY_OP, 1);
    ctx->current[1] = (uint32_t)op;
    ctx->current   += 2;
}

/* ================================================================== *
 * Blend / dither group — NV40TCL_BLEND_* and NV40TCL_DITHER_ENABLE.
 *   blend_func      (0x314) 3 words: (sfalpha<<16) | sfcolor, (dfalpha<<16) | dfcolor
 *   blend_equation  (0x320) 2 words: (alpha<<16) | color
 *   blend_color     (0x31c / 0x37c) 4 words: TWO method packets
 *                            BLEND_COLOR -> color0, BLEND_COLOR2 -> color1
 *   blend_enable    (0x310) 2 words
 *   dither_enable   (0x300) 2 words
 * =================================================================== */

/* safe */
static inline void ps3tc_gcm_state_blend_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_BLEND_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_blend_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_BLEND_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — 3 words; two 16-bit values each packed low/high. */
static inline void ps3tc_gcm_state_blend_func(CellGcmContextData *ctx,
                                              uint16_t sfcolor, uint16_t dfcolor,
                                              uint16_t sfalpha, uint16_t dfalpha)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 3);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_BLEND_FUNC_SRC, 2);
    *w++ = ((uint32_t)sfalpha << 16) | (uint32_t)sfcolor;
    *w   = ((uint32_t)dfalpha << 16) | (uint32_t)dfcolor;
}

/* unsafe */
static inline void ps3tc_gcm_state_blend_func_unsafe(CellGcmContextData *ctx,
                                                     uint16_t sfcolor, uint16_t dfcolor,
                                                     uint16_t sfalpha, uint16_t dfalpha)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_BLEND_FUNC_SRC, 2);
    ctx->current[1] = ((uint32_t)sfalpha << 16) | (uint32_t)sfcolor;
    ctx->current[2] = ((uint32_t)dfalpha << 16) | (uint32_t)dfcolor;
    ctx->current   += 3;
}

/* safe — 2 words; (alpha<<16) | color packed into one word. */
static inline void ps3tc_gcm_state_blend_equation(CellGcmContextData *ctx,
                                                  uint16_t color, uint16_t alpha)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_BLEND_EQUATION, 1);
    *w    = ((uint32_t)alpha << 16) | (uint32_t)color;
}

/* unsafe */
static inline void ps3tc_gcm_state_blend_equation_unsafe(CellGcmContextData *ctx,
                                                         uint16_t color, uint16_t alpha)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_BLEND_EQUATION, 1);
    ctx->current[1] = ((uint32_t)alpha << 16) | (uint32_t)color;
    ctx->current   += 2;
}

/* safe — 4 words; TWO method packets: BLEND_COLOR -> color0, BLEND_COLOR2 -> color1. */
static inline void ps3tc_gcm_state_blend_color(CellGcmContextData *ctx,
                                               uint32_t color0, uint32_t color1)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 4);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_BLEND_COLOR, 1);
    *w++ = color0;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_BLEND_COLOR2, 1);
    *w   = color1;
}

/* unsafe */
static inline void ps3tc_gcm_state_blend_color_unsafe(CellGcmContextData *ctx,
                                                      uint32_t color0, uint32_t color1)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_BLEND_COLOR, 1);
    ctx->current[1] = color0;
    ctx->current[2] = PS3TC_GCM_METHOD(NV40TCL_BLEND_COLOR2, 1);
    ctx->current[3] = color1;
    ctx->current   += 4;
}

/* safe */
static inline void ps3tc_gcm_state_dither_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_DITHER_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_dither_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_DITHER_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* ================================================================== *
 * Stencil group — NV40TCL_STENCIL_{BACK,FRONT}_*.
 * Register names follow rsx/nv40.h; the mapping is unchanged from the
 * pre-patch rsx* bodies (SetStencil* -> STENCIL_BACK_*,
 * SetBackStencil* -> STENCIL_FRONT_*).
 * All three "Func"/"Op" functions carry count=3 (3 consecutive methods
 * under one header).  The two Enable functions and Mask are count=1.
 * =================================================================== */

/* safe — stencil_test_enable (STENCIL_BACK_ENABLE, 0x328) */
static inline void ps3tc_gcm_state_stencil_test_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_STENCIL_BACK_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_stencil_test_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_STENCIL_BACK_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — two_sided_stencil_test_enable (STENCIL_FRONT_ENABLE, 0x348) */
static inline void ps3tc_gcm_state_two_sided_stencil_test_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_STENCIL_FRONT_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_two_sided_stencil_test_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_STENCIL_FRONT_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — stencil_func (STENCIL_BACK_FUNC_FUNC, 0x330, count=3, 4 words) */
static inline void ps3tc_gcm_state_stencil_func(CellGcmContextData *ctx,
                                                 uint32_t func, uint32_t ref, uint32_t mask)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 4);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_STENCIL_BACK_FUNC_FUNC, 3);
    *w++ = func;
    *w++ = ref;
    *w   = mask;
}

/* unsafe */
static inline void ps3tc_gcm_state_stencil_func_unsafe(CellGcmContextData *ctx,
                                                        uint32_t func, uint32_t ref, uint32_t mask)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_STENCIL_BACK_FUNC_FUNC, 3);
    ctx->current[1] = func;
    ctx->current[2] = ref;
    ctx->current[3] = mask;
    ctx->current   += 4;
}

/* safe — stencil_mask (STENCIL_BACK_MASK, 0x32c) */
static inline void ps3tc_gcm_state_stencil_mask(CellGcmContextData *ctx, uint32_t mask)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_STENCIL_BACK_MASK, 1);
    *w    = mask;
}

/* unsafe */
static inline void ps3tc_gcm_state_stencil_mask_unsafe(CellGcmContextData *ctx, uint32_t mask)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_STENCIL_BACK_MASK, 1);
    ctx->current[1] = mask;
    ctx->current   += 2;
}

/* safe — stencil_op (STENCIL_BACK_OP_FAIL, 0x33c, count=3, 4 words) */
static inline void ps3tc_gcm_state_stencil_op(CellGcmContextData *ctx,
                                               uint32_t fail, uint32_t zfail, uint32_t zpass)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 4);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_STENCIL_BACK_OP_FAIL, 3);
    *w++ = fail;
    *w++ = zfail;
    *w   = zpass;
}

/* unsafe */
static inline void ps3tc_gcm_state_stencil_op_unsafe(CellGcmContextData *ctx,
                                                      uint32_t fail, uint32_t zfail, uint32_t zpass)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_STENCIL_BACK_OP_FAIL, 3);
    ctx->current[1] = fail;
    ctx->current[2] = zfail;
    ctx->current[3] = zpass;
    ctx->current   += 4;
}

/* safe — back_stencil_func (STENCIL_FRONT_FUNC_FUNC, 0x350, count=3, 4 words) */
static inline void ps3tc_gcm_state_back_stencil_func(CellGcmContextData *ctx,
                                                       uint32_t func, uint32_t ref, uint32_t mask)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 4);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_STENCIL_FRONT_FUNC_FUNC, 3);
    *w++ = func;
    *w++ = ref;
    *w   = mask;
}

/* unsafe */
static inline void ps3tc_gcm_state_back_stencil_func_unsafe(CellGcmContextData *ctx,
                                                              uint32_t func, uint32_t ref, uint32_t mask)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_STENCIL_FRONT_FUNC_FUNC, 3);
    ctx->current[1] = func;
    ctx->current[2] = ref;
    ctx->current[3] = mask;
    ctx->current   += 4;
}

/* safe — back_stencil_mask (STENCIL_FRONT_MASK, 0x34c) */
static inline void ps3tc_gcm_state_back_stencil_mask(CellGcmContextData *ctx, uint32_t mask)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_STENCIL_FRONT_MASK, 1);
    *w    = mask;
}

/* unsafe */
static inline void ps3tc_gcm_state_back_stencil_mask_unsafe(CellGcmContextData *ctx, uint32_t mask)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_STENCIL_FRONT_MASK, 1);
    ctx->current[1] = mask;
    ctx->current   += 2;
}

/* safe — back_stencil_op (STENCIL_FRONT_OP_FAIL, 0x35c, count=3, 4 words) */
static inline void ps3tc_gcm_state_back_stencil_op(CellGcmContextData *ctx,
                                                     uint32_t fail, uint32_t zfail, uint32_t zpass)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 4);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_STENCIL_FRONT_OP_FAIL, 3);
    *w++ = fail;
    *w++ = zfail;
    *w   = zpass;
}

/* unsafe */
static inline void ps3tc_gcm_state_back_stencil_op_unsafe(CellGcmContextData *ctx,
                                                            uint32_t fail, uint32_t zfail, uint32_t zpass)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_STENCIL_FRONT_OP_FAIL, 3);
    ctx->current[1] = fail;
    ctx->current[2] = zfail;
    ctx->current[3] = zpass;
    ctx->current   += 4;
}

/* ================================================================== *
 * Raster state group — color mask, shade model, cull/front-face,
 * polygon mode, polygon offset, antialias, alpha test.
 * All args are written verbatim (no clamping or translation in the
 * pre-patch rsx* bodies; this is preserved exactly here).
 * =================================================================== */

/* safe — color_mask (COLOR_MASK 0x324) */
static inline void ps3tc_gcm_state_color_mask(CellGcmContextData *ctx, uint32_t mask)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_COLOR_MASK, 1);
    *w    = mask;
}

/* unsafe */
static inline void ps3tc_gcm_state_color_mask_unsafe(CellGcmContextData *ctx, uint32_t mask)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_COLOR_MASK, 1);
    ctx->current[1] = mask;
    ctx->current   += 2;
}

/* safe — color_mask_mrt (MRT_COLOR_MASK 0x370) */
static inline void ps3tc_gcm_state_color_mask_mrt(CellGcmContextData *ctx, uint32_t mask)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_MRT_COLOR_MASK, 1);
    *w    = mask;
}

/* unsafe */
static inline void ps3tc_gcm_state_color_mask_mrt_unsafe(CellGcmContextData *ctx, uint32_t mask)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_MRT_COLOR_MASK, 1);
    ctx->current[1] = mask;
    ctx->current   += 2;
}

/* safe — shade_model (SHADE_MODEL 0x368).  Backs both cellGcmSetShadeModel
 * and cellGcmSetShadeMode (ABI alias, same value). */
static inline void ps3tc_gcm_state_shade_model(CellGcmContextData *ctx, uint32_t model)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_SHADE_MODEL, 1);
    *w    = model;
}

/* unsafe */
static inline void ps3tc_gcm_state_shade_model_unsafe(CellGcmContextData *ctx, uint32_t model)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_SHADE_MODEL, 1);
    ctx->current[1] = model;
    ctx->current   += 2;
}

/* safe — cull_face_enable (CULL_FACE_ENABLE 0x183c) */
static inline void ps3tc_gcm_state_cull_face_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_CULL_FACE_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_cull_face_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_CULL_FACE_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — cull_face (CULL_FACE 0x1830).  Value written verbatim. */
static inline void ps3tc_gcm_state_cull_face(CellGcmContextData *ctx, uint32_t cull)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_CULL_FACE, 1);
    *w    = cull;
}

/* unsafe */
static inline void ps3tc_gcm_state_cull_face_unsafe(CellGcmContextData *ctx, uint32_t cull)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_CULL_FACE, 1);
    ctx->current[1] = cull;
    ctx->current   += 2;
}

/* safe — front_face (FRONT_FACE 0x1834).  Value written verbatim. */
static inline void ps3tc_gcm_state_front_face(CellGcmContextData *ctx, uint32_t dir)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_FRONT_FACE, 1);
    *w    = dir;
}

/* unsafe */
static inline void ps3tc_gcm_state_front_face_unsafe(CellGcmContextData *ctx, uint32_t dir)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_FRONT_FACE, 1);
    ctx->current[1] = dir;
    ctx->current   += 2;
}

/* safe — front_polygon_mode (POLYGON_MODE_FRONT 0x1828) */
static inline void ps3tc_gcm_state_front_polygon_mode(CellGcmContextData *ctx, uint32_t mode)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_POLYGON_MODE_FRONT, 1);
    *w    = mode;
}

/* unsafe */
static inline void ps3tc_gcm_state_front_polygon_mode_unsafe(CellGcmContextData *ctx, uint32_t mode)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_POLYGON_MODE_FRONT, 1);
    ctx->current[1] = mode;
    ctx->current   += 2;
}

/* safe — back_polygon_mode (POLYGON_MODE_BACK 0x182c) */
static inline void ps3tc_gcm_state_back_polygon_mode(CellGcmContextData *ctx, uint32_t mode)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_POLYGON_MODE_BACK, 1);
    *w    = mode;
}

/* unsafe */
static inline void ps3tc_gcm_state_back_polygon_mode_unsafe(CellGcmContextData *ctx, uint32_t mode)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_POLYGON_MODE_BACK, 1);
    ctx->current[1] = mode;
    ctx->current   += 2;
}

/* safe — polygon_offset_fill_enable (POLYGON_OFFSET_FILL_ENABLE 0xa68) */
static inline void ps3tc_gcm_state_polygon_offset_fill_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_POLYGON_OFFSET_FILL_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_polygon_offset_fill_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_POLYGON_OFFSET_FILL_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — line_smooth_enable (LINE_SMOOTH_ENABLE 0x3bc) */
static inline void ps3tc_gcm_state_line_smooth_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_LINE_SMOOTH_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_line_smooth_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_LINE_SMOOTH_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — polygon_smooth_enable (POLYGON_SMOOTH_ENABLE 0x1838)
 * (rsx name: rsxSetPolygonSmoothEnable; cell name: cellGcmSetPolySmoothEnable.) */
static inline void ps3tc_gcm_state_polygon_smooth_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_POLYGON_SMOOTH_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_polygon_smooth_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_POLYGON_SMOOTH_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — alpha_test_enable (ALPHA_TEST_ENABLE 0x304) */
static inline void ps3tc_gcm_state_alpha_test_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_ALPHA_TEST_ENABLE, 1);
    *w    = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_alpha_test_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_ALPHA_TEST_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — alpha_func (ALPHA_TEST_FUNC 0x308, count=2, 3 words) */
static inline void ps3tc_gcm_state_alpha_func(CellGcmContextData *ctx,
                                               uint32_t func, uint32_t ref)
{
    uint32_t *w = ps3tc_gcm_reserve(ctx, 3);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_ALPHA_TEST_FUNC, 2);
    *w++ = func;
    *w   = ref;
}

/* unsafe */
static inline void ps3tc_gcm_state_alpha_func_unsafe(CellGcmContextData *ctx,
                                                      uint32_t func, uint32_t ref)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_ALPHA_TEST_FUNC, 2);
    ctx->current[1] = func;
    ctx->current[2] = ref;
    ctx->current   += 3;
}

/* safe — polygon_offset (POLYGON_OFFSET_FACTOR 0xa78, count=2, 3 words).
 * factor/units are written as IEEE-32 bit patterns (same as depth_bounds). */
static inline void ps3tc_gcm_state_polygon_offset(CellGcmContextData *ctx,
                                                   f32 factor, f32 units)
{
    ieee32 a, b;
    a.f = factor; b.f = units;
    uint32_t *w = ps3tc_gcm_reserve(ctx, 3);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_POLYGON_OFFSET_FACTOR, 2);
    *w++ = a.u;
    *w   = b.u;
}

/* unsafe */
static inline void ps3tc_gcm_state_polygon_offset_unsafe(CellGcmContextData *ctx,
                                                          f32 factor, f32 units)
{
    ieee32 a, b;
    a.f = factor; b.f = units;
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_POLYGON_OFFSET_FACTOR, 2);
    ctx->current[1] = a.u;
    ctx->current[2] = b.u;
    ctx->current   += 3;
}
/* ================================================================== *
 * Viewport / scissor / clear-value / zcull / scull group.
 * ================================================================== */

/* safe — viewport (24 words, 3 methods; copied verbatim from the pre-patch
 * rsxSetViewport body, including the two identical VIEWPORT_OFFSET
 * packets, which is the wire contract).
 *   VIEWPORT_HORIZ (count=2) : (width<<16)|x, (height<<16)|y
 *   DEPTH_RANGE    (count=2) : min, max          (IEEE-32)
 *   VIEWPORT_OFFSET (count=8): offset[4], scale[4]  (IEEE-32)   x2 */
static inline void ps3tc_gcm_state_viewport(CellGcmContextData *ctx,
    uint16_t x, uint16_t y, uint16_t w, uint16_t h,
    f32 min, f32 max, const f32 scale[4], const f32 offset[4])
{
    ieee32 _min, _max;
    ieee32 _offset[4], _scale[4];
    _min.f = min; _max.f = max;
    _scale[0].f = scale[0]; _scale[1].f = scale[1];
    _scale[2].f = scale[2]; _scale[3].f = scale[3];
    _offset[0].f = offset[0]; _offset[1].f = offset[1];
    _offset[2].f = offset[2]; _offset[3].f = offset[3];
    uint32_t *p = ps3tc_gcm_reserve(ctx, 24);
    if (!p) return;
    p[0]  = PS3TC_GCM_METHOD(NV40TCL_VIEWPORT_HORIZ, 2);
    p[1]  = ((uint32_t)w << 16) | x;
    p[2]  = ((uint32_t)h << 16) | y;
    p[3]  = PS3TC_GCM_METHOD(NV40TCL_DEPTH_RANGE, 2);
    p[4]  = _min.u;
    p[5]  = _max.u;
    p[6]  = PS3TC_GCM_METHOD(NV40TCL_VIEWPORT_OFFSET, 8);
    p[7]  = _offset[0].u; p[8]  = _offset[1].u; p[9]  = _offset[2].u; p[10] = _offset[3].u;
    p[11] = _scale[0].u;  p[12] = _scale[1].u;  p[13] = _scale[2].u;  p[14] = _scale[3].u;
    p[15] = PS3TC_GCM_METHOD(NV40TCL_VIEWPORT_OFFSET, 8);
    p[16] = _offset[0].u; p[17] = _offset[1].u; p[18] = _offset[2].u; p[19] = _offset[3].u;
    p[20] = _scale[0].u;  p[21] = _scale[1].u;  p[22] = _scale[2].u;  p[23] = _scale[3].u;
}

/* unsafe — 24 words, plain write, no reservation. */
static inline void ps3tc_gcm_state_viewport_unsafe(CellGcmContextData *ctx,
    uint16_t x, uint16_t y, uint16_t w, uint16_t h,
    f32 min, f32 max, const f32 scale[4], const f32 offset[4])
{
    ieee32 _min, _max;
    ieee32 _offset[4], _scale[4];
    _min.f = min; _max.f = max;
    _scale[0].f = scale[0]; _scale[1].f = scale[1];
    _scale[2].f = scale[2]; _scale[3].f = scale[3];
    _offset[0].f = offset[0]; _offset[1].f = offset[1];
    _offset[2].f = offset[2]; _offset[3].f = offset[3];
    ctx->current[0]  = PS3TC_GCM_METHOD(NV40TCL_VIEWPORT_HORIZ, 2);
    ctx->current[1]  = ((uint32_t)w << 16) | x;
    ctx->current[2]  = ((uint32_t)h << 16) | y;
    ctx->current[3]  = PS3TC_GCM_METHOD(NV40TCL_DEPTH_RANGE, 2);
    ctx->current[4]  = _min.u;
    ctx->current[5]  = _max.u;
    ctx->current[6]  = PS3TC_GCM_METHOD(NV40TCL_VIEWPORT_OFFSET, 8);
    ctx->current[7]  = _offset[0].u; ctx->current[8]  = _offset[1].u;
    ctx->current[9]  = _offset[2].u; ctx->current[10] = _offset[3].u;
    ctx->current[11] = _scale[0].u;  ctx->current[12] = _scale[1].u;
    ctx->current[13] = _scale[2].u;  ctx->current[14] = _scale[3].u;
    ctx->current[15] = PS3TC_GCM_METHOD(NV40TCL_VIEWPORT_OFFSET, 8);
    ctx->current[16] = _offset[0].u; ctx->current[17] = _offset[1].u;
    ctx->current[18] = _offset[2].u; ctx->current[19] = _offset[3].u;
    ctx->current[20] = _scale[0].u;  ctx->current[21] = _scale[1].u;
    ctx->current[22] = _scale[2].u;  ctx->current[23] = _scale[3].u;
    ctx->current   += 24;
}

/* safe — scissor (SCISSOR_HORIZ count=2, 3 words). */
static inline void ps3tc_gcm_state_scissor(CellGcmContextData *ctx,
                                           uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 3);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_SCISSOR_HORIZ, 2);
    p[1] = ((uint32_t)w << 16) | x;
    p[2] = ((uint32_t)h << 16) | y;
}

/* unsafe */
static inline void ps3tc_gcm_state_scissor_unsafe(CellGcmContextData *ctx,
    uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_SCISSOR_HORIZ, 2);
    ctx->current[1] = ((uint32_t)w << 16) | x;
    ctx->current[2] = ((uint32_t)h << 16) | y;
    ctx->current   += 3;
}

/* safe — clear_color (CLEAR_VALUE_COLOR count=1, 2 words). */
static inline void ps3tc_gcm_state_clear_color(CellGcmContextData *ctx, uint32_t color)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_CLEAR_VALUE_COLOR, 1);
    p[1] = color;
}

/* unsafe */
static inline void ps3tc_gcm_state_clear_color_unsafe(CellGcmContextData *ctx, uint32_t color)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_CLEAR_VALUE_COLOR, 1);
    ctx->current[1] = color;
    ctx->current   += 2;
}

/* safe — clear_depth_stencil (CLEAR_VALUE_DEPTH count=1, 2 words). */
static inline void ps3tc_gcm_state_clear_depth_stencil(CellGcmContextData *ctx, uint32_t value)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_CLEAR_VALUE_DEPTH, 1);
    p[1] = value;
}

/* unsafe */
static inline void ps3tc_gcm_state_clear_depth_stencil_unsafe(CellGcmContextData *ctx, uint32_t value)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_CLEAR_VALUE_DEPTH, 1);
    ctx->current[1] = value;
    ctx->current   += 2;
}

/* safe — zcull_control (ZCULL_CONTROL0 count=1, 2 words): (fmt<<4)|dir. */
static inline void ps3tc_gcm_state_zcull_control(CellGcmContextData *ctx,
                                                 uint32_t zcull_dir, uint32_t zcull_fmt)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_ZCULL_CONTROL0, 1);
    p[1] = ((zcull_fmt << 4) | zcull_dir);
}

/* unsafe */
static inline void ps3tc_gcm_state_zcull_control_unsafe(CellGcmContextData *ctx,
    uint32_t zcull_dir, uint32_t zcull_fmt)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_ZCULL_CONTROL0, 1);
    ctx->current[1] = ((zcull_fmt << 4) | zcull_dir);
    ctx->current   += 2;
}

/* safe — zcull_enable (ZCULL_ENABLE count=1, 2 words): (stencil<<1)|depth. */
static inline void ps3tc_gcm_state_zcull_enable(CellGcmContextData *ctx,
                                                uint32_t depth, uint32_t stencil)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_ZCULL_ENABLE, 1);
    p[1] = ((stencil << 1) | depth);
}

/* unsafe */
static inline void ps3tc_gcm_state_zcull_enable_unsafe(CellGcmContextData *ctx,
    uint32_t depth, uint32_t stencil)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_ZCULL_ENABLE, 1);
    ctx->current[1] = ((stencil << 1) | depth);
    ctx->current   += 2;
}

/* safe — zcull_limit (ZCULL_CONTROL1 count=1, 2 words): (mfl<<16)|pb. */
static inline void ps3tc_gcm_state_zcull_limit(CellGcmContextData *ctx,
                                               uint16_t move_forward_limit, uint16_t push_back_limit)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_ZCULL_CONTROL1, 1);
    p[1] = ((uint32_t)move_forward_limit << 16) | push_back_limit;
}

/* unsafe */
static inline void ps3tc_gcm_state_zcull_limit_unsafe(CellGcmContextData *ctx,
    uint16_t move_forward_limit, uint16_t push_back_limit)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_ZCULL_CONTROL1, 1);
    ctx->current[1] = ((uint32_t)move_forward_limit << 16) | push_back_limit;
    ctx->current   += 2;
}

/* safe — zcull_stats_enable (ZCULL_STATS_ENABLE count=1, 2 words). */
static inline void ps3tc_gcm_state_zcull_stats_enable(CellGcmContextData *ctx, uint32_t enable)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_ZCULL_STATS_ENABLE, 1);
    p[1] = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_zcull_stats_enable_unsafe(CellGcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_ZCULL_STATS_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — scull_control (SCULL_CONTROL count=1, 2 words):
 * (mask<<24)|(ref<<16)|func.  func is a u8 in the wire word. */
static inline void ps3tc_gcm_state_scull_control(CellGcmContextData *ctx,
                                                 uint8_t s_func, uint8_t s_ref, uint8_t s_mask)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_SCULL_CONTROL, 1);
    p[1] = ((s_mask << 24) | (s_ref << 16) | s_func);
}

/* unsafe */
static inline void ps3tc_gcm_state_scull_control_unsafe(CellGcmContextData *ctx,
    uint8_t s_func, uint8_t s_ref, uint8_t s_mask)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_SCULL_CONTROL, 1);
    ctx->current[1] = ((s_mask << 24) | (s_ref << 16) | s_func);
    ctx->current   += 2;
}

/* safe — invalidate_zcull (INVALIDATE_ZCULL count=1, 2 words, value 0). */
static inline void ps3tc_gcm_state_invalidate_zcull(CellGcmContextData *ctx)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_INVALIDATE_ZCULL, 1);
    p[1] = 0;
}

/* unsafe */
static inline void ps3tc_gcm_state_invalidate_zcull_unsafe(CellGcmContextData *ctx)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_INVALIDATE_ZCULL, 1);
    ctx->current[1] = 0;
    ctx->current   += 2;
}

/* ===================================================================
 * Texture border color, vertex-attribute output mask, Z-pass pixel
 * count, clear-zcull surface, reference command, cache invalidates,
 * vertex-data 4-vector and report reset.  Each SAFE variant reserves
 * via ps3tc_gcm_reserve; each UNSAFE variant plain-writes.
 *
 * Parameter widths match the pre-patch rsx* signatures exactly (none
 * of these forwarders truncate a wider public argument, so no extra
 * byte-case is required).
 * =================================================================== */

/* safe — texture_border_color (TEX_BORDER_COLOR(index) count=1, 2 words). */
static inline void ps3tc_gcm_state_texture_border_color(CellGcmContextData *ctx,
                                                        uint8_t index, uint32_t color)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_TEX_BORDER_COLOR(index), 1);
    p[1] = color;
}

/* unsafe */
static inline void ps3tc_gcm_state_texture_border_color_unsafe(CellGcmContextData *ctx,
                                                               uint8_t index, uint32_t color)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_TEX_BORDER_COLOR(index), 1);
    ctx->current[1] = color;
    ctx->current   += 2;
}

/* safe — vertex_attrib_output_mask (VP_RESULT_EN count=1, 2 words). */
static inline void ps3tc_gcm_state_vertex_attrib_output_mask(CellGcmContextData *ctx,
                                                             uint32_t mask)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_VP_RESULT_EN, 1);
    p[1] = mask;
}

/* unsafe */
static inline void ps3tc_gcm_state_vertex_attrib_output_mask_unsafe(CellGcmContextData *ctx,
                                                                    uint32_t mask)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_VP_RESULT_EN, 1);
    ctx->current[1] = mask;
    ctx->current   += 2;
}

/* safe — zpass_pixel_count_enable (QUERY_ENABLE count=1, 2 words). */
static inline void ps3tc_gcm_state_zpass_pixel_count_enable(CellGcmContextData *ctx,
                                                            uint32_t enable)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_QUERY_ENABLE, 1);
    p[1] = enable;
}

/* unsafe */
static inline void ps3tc_gcm_state_zpass_pixel_count_enable_unsafe(CellGcmContextData *ctx,
                                                                   uint32_t enable)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_QUERY_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}

/* safe — clear_zcull_surface (CLEAR_ZCULL_SURFACE count=1, 2 words, (stencil<<1)|depth). */
static inline void ps3tc_gcm_state_clear_zcull_surface(CellGcmContextData *ctx,
                                                       uint32_t depth, uint32_t stencil)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_CLEAR_ZCULL_SURFACE, 1);
    p[1] = ((stencil << 1) | depth);
}

/* unsafe */
static inline void ps3tc_gcm_state_clear_zcull_surface_unsafe(CellGcmContextData *ctx,
                                                              uint32_t depth, uint32_t stencil)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_CLEAR_ZCULL_SURFACE, 1);
    ctx->current[1] = ((stencil << 1) | depth);
    ctx->current   += 2;
}

/* safe — reference_command (NV406ETCL_SET_REF count=1, 2 words). */
static inline void ps3tc_gcm_state_reference_command(CellGcmContextData *ctx, uint32_t ref_value)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV406ETCL_SET_REF, 1);
    p[1] = ref_value;
}

/* unsafe */
static inline void ps3tc_gcm_state_reference_command_unsafe(CellGcmContextData *ctx, uint32_t ref_value)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV406ETCL_SET_REF, 1);
    ctx->current[1] = ref_value;
    ctx->current   += 2;
}

/* safe — invalidate_vertex_cache (VTX_CACHE_INVALIDATE2 + 3x VTX_CACHE_INVALIDATE, 8 words). */
static inline void ps3tc_gcm_state_invalidate_vertex_cache(CellGcmContextData *ctx)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 8);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_VTX_CACHE_INVALIDATE2, 1);
    p[1] = 0;
    p[2] = PS3TC_GCM_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);
    p[3] = 0;
    p[4] = PS3TC_GCM_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);
    p[5] = 0;
    p[6] = PS3TC_GCM_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);
    p[7] = 0;
}

/* unsafe */
static inline void ps3tc_gcm_state_invalidate_vertex_cache_unsafe(CellGcmContextData *ctx)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_VTX_CACHE_INVALIDATE2, 1);
    ctx->current[1] = 0;
    ctx->current[2] = PS3TC_GCM_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);
    ctx->current[3] = 0;
    ctx->current[4] = PS3TC_GCM_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);
    ctx->current[5] = 0;
    ctx->current[6] = PS3TC_GCM_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);
    ctx->current[7] = 0;
    ctx->current   += 8;
}

/* safe — invalidate_texture_cache (TEX_CACHE_CTL count=1, 2 words). */
static inline void ps3tc_gcm_state_invalidate_texture_cache(CellGcmContextData *ctx, uint32_t type)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_TEX_CACHE_CTL, 1);
    p[1] = type;
}

/* unsafe */
static inline void ps3tc_gcm_state_invalidate_texture_cache_unsafe(CellGcmContextData *ctx, uint32_t type)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_TEX_CACHE_CTL, 1);
    ctx->current[1] = type;
    ctx->current   += 2;
}

/* safe — vertex_data_4f (VTX_ATTR_4F_X(index) count=4, 5 words, v[4] IEEE-32 bits). */
static inline void ps3tc_gcm_state_vertex_data_4f(CellGcmContextData *ctx,
                                                  uint8_t index, const f32 v[4])
{
    ieee32 w0, w1, w2, w3;
    w0.f = v[0]; w1.f = v[1]; w2.f = v[2]; w3.f = v[3];
    uint32_t *p = ps3tc_gcm_reserve(ctx, 5);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_VTX_ATTR_4F_X(index), 4);
    p[1] = w0.u;
    p[2] = w1.u;
    p[3] = w2.u;
    p[4] = w3.u;
}

/* unsafe */
static inline void ps3tc_gcm_state_vertex_data_4f_unsafe(CellGcmContextData *ctx,
                                                         uint8_t index, const f32 v[4])
{
    ieee32 w0, w1, w2, w3;
    w0.f = v[0]; w1.f = v[1]; w2.f = v[2]; w3.f = v[3];
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_VTX_ATTR_4F_X(index), 4);
    ctx->current[1] = w0.u;
    ctx->current[2] = w1.u;
    ctx->current[3] = w2.u;
    ctx->current[4] = w3.u;
    ctx->current   += 5;
}

/* safe — clear_report (QUERY_RESET count=1, 2 words).  The pre-patch rsx took u32; the
 * cellGcm forwarder passes a wider uint8_t, which widens losslessly, so the native keeps u32. */
static inline void ps3tc_gcm_state_clear_report(CellGcmContextData *ctx, uint32_t type)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_QUERY_RESET, 1);
    p[1] = type;
}

/* unsafe */
static inline void ps3tc_gcm_state_clear_report_unsafe(CellGcmContextData *ctx, uint32_t type)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_QUERY_RESET, 1);
    ctx->current[1] = type;
    ctx->current   += 2;
}

/* ===================================================================
 * Surface, vertex data array, vertex program constants, update FP
 * parameter, nop command, and report location.
 * Each SAFE variant reserves via ps3tc_gcm_reserve; each UNSAFE
 * variant plain-writes.
 * =================================================================== */

/* floor(log2(x)) as 31 - __cntlzw(x) computes it (0xffffffff for x == 0), without
 * needing <ppu_intrinsics.h>: this header is included by applications that never
 * include it, and builds on non-PowerPC hosts too. */
static inline uint32_t ps3tc_gcm_log2_floor(uint32_t x)
{
    return x ? 31u - (uint32_t)__builtin_clz(x) : 0xffffffffu;
}

/* safe — surface (multi-method framebuffer configuration, 32 words).
 * Emits DMA color/zeta binds, RT format/pitch/offsets, RT enable,
 * window offset, RT horizontal/vertical dimensions, and shader window. */
static inline void ps3tc_gcm_state_surface(CellGcmContextData *ctx, const CellGcmSurface *surface)
{
    uint32_t log2Width = ps3tc_gcm_log2_floor(surface->width);
    uint32_t log2Height = ps3tc_gcm_log2_floor(surface->height);

    uint32_t *p = ps3tc_gcm_reserve(ctx, 32);
    if (!p) return;

    p[0]  = PS3TC_GCM_METHOD(NV40TCL_DMA_COLOR0, 1);
    p[1]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[0];
    p[2]  = PS3TC_GCM_METHOD(NV40TCL_DMA_COLOR1, 1);
    p[3]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[1];
    p[4]  = PS3TC_GCM_METHOD(NV40TCL_DMA_COLOR2, 2);
    p[5]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[2];
    p[6]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[3];
    p[7]  = PS3TC_GCM_METHOD(NV40TCL_DMA_ZETA, 1);
    p[8]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->depthLocation;
    p[9]  = PS3TC_GCM_METHOD(NV40TCL_RT_FORMAT, 6);
    p[10] = ((log2Height << NV40TCL_RT_FORMAT_LOG2_HEIGHT_SHIFT) |
             (log2Width  << NV40TCL_RT_FORMAT_LOG2_WIDTH_SHIFT) |
             (surface->antiAlias << NV40TCL_RT_FORMAT_ANTIALIAS_SHIFT) |
             (surface->type << NV40TCL_RT_FORMAT_TYPE_SHIFT) |
             (surface->depthFormat << NV40TCL_RT_FORMAT_ZETA_SHIFT) |
             (surface->colorFormat << NV40TCL_RT_FORMAT_COLOR_SHIFT));
    p[11] = surface->colorPitch[0];
    p[12] = surface->colorOffset[0];
    p[13] = surface->depthOffset;
    p[14] = surface->colorOffset[1];
    p[15] = surface->colorPitch[1];
    p[16] = PS3TC_GCM_METHOD(NV40TCL_ZETA_PITCH, 1);
    p[17] = surface->depthPitch;
    p[18] = PS3TC_GCM_METHOD(NV40TCL_COLOR2_PITCH, 4);
    p[19] = surface->colorPitch[2];
    p[20] = surface->colorPitch[3];
    p[21] = surface->colorOffset[2];
    p[22] = surface->colorOffset[3];
    p[23] = PS3TC_GCM_METHOD(NV40TCL_RT_ENABLE, 1);
    p[24] = surface->colorTarget;
    p[25] = PS3TC_GCM_METHOD(NV40TCL_WINDOW_OFFSET, 1);
    p[26] = ((surface->y << 16) | surface->x);
    p[27] = PS3TC_GCM_METHOD(NV40TCL_RT_HORIZ, 2);
    p[28] = ((surface->width << 16) | surface->x);
    p[29] = ((surface->height << 16) | surface->y);
    p[30] = PS3TC_GCM_METHOD(NV40TCL_SHADER_WINDOW, 1);
    p[31] = ((0 << 16) | (1 << 12) | surface->height);
}

/* unsafe */
static inline void ps3tc_gcm_state_surface_unsafe(CellGcmContextData *ctx, const CellGcmSurface *surface)
{
    uint32_t log2Width = ps3tc_gcm_log2_floor(surface->width);
    uint32_t log2Height = ps3tc_gcm_log2_floor(surface->height);

    ctx->current[0]  = PS3TC_GCM_METHOD(NV40TCL_DMA_COLOR0, 1);
    ctx->current[1]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[0];
    ctx->current[2]  = PS3TC_GCM_METHOD(NV40TCL_DMA_COLOR1, 1);
    ctx->current[3]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[1];
    ctx->current[4]  = PS3TC_GCM_METHOD(NV40TCL_DMA_COLOR2, 2);
    ctx->current[5]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[2];
    ctx->current[6]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[3];
    ctx->current[7]  = PS3TC_GCM_METHOD(NV40TCL_DMA_ZETA, 1);
    ctx->current[8]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->depthLocation;
    ctx->current[9]  = PS3TC_GCM_METHOD(NV40TCL_RT_FORMAT, 6);
    ctx->current[10] = ((log2Height << NV40TCL_RT_FORMAT_LOG2_HEIGHT_SHIFT) |
                        (log2Width  << NV40TCL_RT_FORMAT_LOG2_WIDTH_SHIFT) |
                        (surface->antiAlias << NV40TCL_RT_FORMAT_ANTIALIAS_SHIFT) |
                        (surface->type << NV40TCL_RT_FORMAT_TYPE_SHIFT) |
                        (surface->depthFormat << NV40TCL_RT_FORMAT_ZETA_SHIFT) |
                        (surface->colorFormat << NV40TCL_RT_FORMAT_COLOR_SHIFT));
    ctx->current[11] = surface->colorPitch[0];
    ctx->current[12] = surface->colorOffset[0];
    ctx->current[13] = surface->depthOffset;
    ctx->current[14] = surface->colorOffset[1];
    ctx->current[15] = surface->colorPitch[1];
    ctx->current[16] = PS3TC_GCM_METHOD(NV40TCL_ZETA_PITCH, 1);
    ctx->current[17] = surface->depthPitch;
    ctx->current[18] = PS3TC_GCM_METHOD(NV40TCL_COLOR2_PITCH, 4);
    ctx->current[19] = surface->colorPitch[2];
    ctx->current[20] = surface->colorPitch[3];
    ctx->current[21] = surface->colorOffset[2];
    ctx->current[22] = surface->colorOffset[3];
    ctx->current[23] = PS3TC_GCM_METHOD(NV40TCL_RT_ENABLE, 1);
    ctx->current[24] = surface->colorTarget;
    ctx->current[25] = PS3TC_GCM_METHOD(NV40TCL_WINDOW_OFFSET, 1);
    ctx->current[26] = ((surface->y << 16) | surface->x);
    ctx->current[27] = PS3TC_GCM_METHOD(NV40TCL_RT_HORIZ, 2);
    ctx->current[28] = ((surface->width << 16) | surface->x);
    ctx->current[29] = ((surface->height << 16) | surface->y);
    ctx->current[30] = PS3TC_GCM_METHOD(NV40TCL_SHADER_WINDOW, 1);
    ctx->current[31] = ((0 << 16) | (1 << 12) | surface->height);
    ctx->current    += 32;
}

/* safe — vertex_data_array (VTXFMT(index) + VTXBUF_ADDRESS(index), 4 words). */
static inline void ps3tc_gcm_state_vertex_data_array(CellGcmContextData *ctx,
                                                     uint8_t index, uint16_t frequency,
                                                     uint8_t stride, uint8_t size,
                                                     uint8_t type, uint8_t location,
                                                     uint32_t offset)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 4);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_VTXFMT(index), 1);
    p[1] = ((frequency << 16) | (stride << NV40TCL_VTXFMT_STRIDE_SHIFT) | (size << NV40TCL_VTXFMT_SIZE_SHIFT) | type);
    p[2] = PS3TC_GCM_METHOD(NV40TCL_VTXBUF_ADDRESS(index), 1);
    p[3] = ((location << 31) | offset);
}

/* unsafe */
static inline void ps3tc_gcm_state_vertex_data_array_unsafe(CellGcmContextData *ctx,
                                                            uint8_t index, uint16_t frequency,
                                                            uint8_t stride, uint8_t size,
                                                            uint8_t type, uint8_t location,
                                                            uint32_t offset)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_VTXFMT(index), 1);
    ctx->current[1] = ((frequency << 16) | (stride << NV40TCL_VTXFMT_STRIDE_SHIFT) | (size << NV40TCL_VTXFMT_SIZE_SHIFT) | type);
    ctx->current[2] = PS3TC_GCM_METHOD(NV40TCL_VTXBUF_ADDRESS(index), 1);
    ctx->current[3] = ((location << 31) | offset);
    ctx->current   += 4;
}

/* safe — vertex_program_constants (variable-length constant upload).
 * Emits VP_UPLOAD_CONST_ID packets in chunks of 32 floats (34 words per chunk)
 * plus a remainder packet of (2 + rest) words. Exact reserve: loop*34 + (rest ? 2 + rest : 0). */
static inline void ps3tc_gcm_state_vertex_program_constants(CellGcmContextData *ctx,
                                                            uint32_t start, uint32_t count,
                                                            const f32 *data)
{
    uint32_t loop = count >> 5;
    uint32_t rest = count & 0x1f;
    uint32_t reserve = loop * 34 + (rest != 0 ? 2 + rest : 0);
    if (reserve == 0) return;

    uint32_t *p = ps3tc_gcm_reserve(ctx, reserve);
    if (!p) return;

    const f32 *value = data;
    for (uint32_t i = 0; i < loop; i++) {
        uint32_t loadAt = start + i * 8;
        p[0] = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, 33);
        p[1] = loadAt;
        memcpy(&p[2], value, sizeof(f32) * 16);
        memcpy(&p[18], &value[16], sizeof(f32) * 16);
        p += 34;
        value += 32;
    }
    if (rest) {
        p[0] = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, rest + 1);
        p[1] = start + (loop << 3);
        memcpy(&p[2], value, sizeof(f32) * rest);
    }
}

/* unsafe */
static inline void ps3tc_gcm_state_vertex_program_constants_unsafe(CellGcmContextData *ctx,
                                                                   uint32_t start, uint32_t count,
                                                                   const f32 *data)
{
    uint32_t loop = count >> 5;
    uint32_t rest = count & 0x1f;
    const f32 *value = data;
    for (uint32_t i = 0; i < loop; i++) {
        uint32_t loadAt = start + i * 8;
        ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, 33);
        ctx->current[1] = loadAt;
        memcpy(&ctx->current[2], value, sizeof(f32) * 16);
        memcpy(&ctx->current[18], &value[16], sizeof(f32) * 16);
        ctx->current += 34;
        value += 32;
    }
    if (rest) {
        ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, rest + 1);
        ctx->current[1] = start + (loop << 3);
        memcpy(&ctx->current[2], value, sizeof(f32) * rest);
        ctx->current += 2 + rest;
    }
}

/* safe — update_fragment_program_location (NV40TCL_FP_ADDRESS count=1, 2 words). */
static inline void ps3tc_gcm_state_update_fragment_program_location(CellGcmContextData *ctx,
                                                                    uint32_t offset, uint32_t location)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_FP_ADDRESS, 1);
    p[1] = ((location + 1) | (offset & 0x1fffffff));
}

/* unsafe */
static inline void ps3tc_gcm_state_update_fragment_program_location_unsafe(CellGcmContextData *ctx,
                                                                           uint32_t offset, uint32_t location)
{
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_FP_ADDRESS, 1);
    ctx->current[1] = ((location + 1) | (offset & 0x1fffffff));
    ctx->current   += 2;
}

/* safe — update_fragment_program_parameter (cellGcm form: fixed GCM_LOCATION_RSX). */
static inline void ps3tc_gcm_state_update_fragment_program_parameter(CellGcmContextData *ctx,
                                                                     uint32_t offset)
{
    ps3tc_gcm_state_update_fragment_program_location(ctx, offset, GCM_LOCATION_RSX);
}

/* unsafe */
static inline void ps3tc_gcm_state_update_fragment_program_parameter_unsafe(CellGcmContextData *ctx,
                                                                            uint32_t offset)
{
    ps3tc_gcm_state_update_fragment_program_location_unsafe(ctx, offset, GCM_LOCATION_RSX);
}

/* safe — nop_command (count words of 0). Zero words if count == 0. */
static inline void ps3tc_gcm_state_nop_command(CellGcmContextData *ctx, uint32_t count)
{
    if (count == 0) return;
    uint32_t *p = ps3tc_gcm_reserve(ctx, count);
    if (!p) return;
    for (uint32_t i = 0; i < count; i++)
        p[i] = 0;
}

/* unsafe */
static inline void ps3tc_gcm_state_nop_command_unsafe(CellGcmContextData *ctx, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        ctx->current[i] = 0;
    ctx->current += count;
}

/* safe — report_location (NV40TCL_DMA_QUERY count=1, 2 words; handle by location). */
static inline void ps3tc_gcm_state_report_location(CellGcmContextData *ctx, uint32_t location)
{
    uint32_t handle = (location == GCM_LOCATION_CELL) ? 0xBAD68000 : 0x66626660;
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_DMA_QUERY, 1);
    p[1] = handle;
}

/* unsafe */
static inline void ps3tc_gcm_state_report_location_unsafe(CellGcmContextData *ctx, uint32_t location)
{
    uint32_t handle = (location == GCM_LOCATION_CELL) ? 0xBAD68000 : 0x66626660;
    ctx->current[0] = PS3TC_GCM_METHOD(NV40TCL_DMA_QUERY, 1);
    ctx->current[1] = handle;
    ctx->current   += 2;
}

/* safe — report (NV40TCL_QUERY_GET count=1, 2 words).
 * Bits 0-23 = byte offset (0x10 * index); bits 24-31 = report type. */
static inline void ps3tc_gcm_state_report(CellGcmContextData *ctx, uint32_t type, uint32_t index)
{
    uint32_t *p = ps3tc_gcm_reserve(ctx, 2);
    if (!p) return;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_QUERY_GET, 1);
    p[1] = ((type << 24) | (index * 0x10u));
}

/* unsafe */
static inline void ps3tc_gcm_state_report_unsafe(CellGcmContextData *ctx, uint32_t type, uint32_t index)
{
    uint32_t *p = ctx->current;
    p[0] = PS3TC_GCM_METHOD(NV40TCL_QUERY_GET, 1);
    p[1] = ((type << 24) | (index * 0x10u));
    ctx->current = p + 2;
}

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_GCM_STATE_NATIVE_H */
