/*
 * gcm-state-native-test.c — byte-identical guard (aarch64 / x86 host).
 *
 * Covers the native emitters in <cell/gcm/gcm_state_native.h> across five
 * groups (logic-op, depth, blend, dither, stencil, raster-state), each
 * with an _unsafe twin.
 *
 * Two checks per native:
 *   1. byte-identity vs the pre-patch commands_impl.h body
 *      (safe == old RSX_UNSAFE=0 body; unsafe == old RSX_UNSAFE=1 body),
 *      across a small argument sweep;
 *   2. RING-FULL: with end==current (zero room) and a recording callback,
 *      the SAFE native must invoke the callback ONCE with the correct
 *      count; the UNSAFE native must never invoke it (plain write).
 *
 * No expected hex is hard-coded: the reference uses the real <rsx/nv40.h>
 * method IDs and the same (count<<18)|method layout.
 *
 * Float-carrying natives (SetDepthBounds, SetPolygonOffset) are compared
 * on the IEEE-32 BIT PATTERNS (d.u), not float values.
 *
 * Host note:
 *   - Runs on a non-PowerPC host via `-DATTRIBUTE_PRXPTR=` (ppu-types.h is
 *     wrapped in #ifndef) — no per-SDK-header shim copies.
 *   - <cell/gcm.h> host-compiles under that setup, so the SAFE path drives
 *     the REAL ps3tc_gcm_reserve (the single ring-full + callback path
 *     shared with the tex/cg bridges).
 *   - The PPU-side rsx* / rsx*-Unsafe are the same inline natives (one
 *     writer); old-vs-new rsx object word comparison is done under the PPU
 *     cross toolchain, not on this host.
 *   - The public cell* forwarders are verified separately by
 *     gcm-state-cellpath-host.c (byte-for-byte == the shared native).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include <ppu-types.h>                 /* u32/f32/ieee32 + guarded ATTRIBUTE_PRXPTR */
#include <cell/gcm.h>                  /* CellGcmContextData + ps3tc_gcm_reserve */
#include <cell/gcm/gcm_state_native.h> /* the natives under test */

/* The reference copies below keep the original bodies' __cntlzw; on a non-PowerPC
 * host it is supplied here (the public headers no longer use it). */
#ifndef __cntlzw
#define __cntlzw(x) ((x) ? (uint32_t)__builtin_clz(x) : 32u)
#endif

/* (count<<18)|method — identical to RSX_METHOD() and PS3TC_GCM_METHOD(). */
#define REF_METHOD(m, count) ((((uint32_t)(count)) << 18) | (uint32_t)(m))
#define REF_SUBCHANNEL_METHOD(ch, m, count) \
    ((((uint32_t)(count)) << 18) | (((uint32_t)(ch)) << 13) | (uint32_t)(m))

/* -------------------------------------------------------------------- *
 * Reference (pre-patch commands_impl.h bodies), in-test.
 * -------------------------------------------------------------------- */
static void ref_safe1(gcmContextData *ctx, uint32_t method, uint32_t val)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(method, 1);
    ctx->current[1] = val;
    ctx->current   += 2;
}
static void ref_unsafe1(gcmContextData *ctx, uint32_t method, uint32_t val)
{
    ctx->current[0] = REF_METHOD(method, 1);
    ctx->current[1] = val;
    ctx->current   += 2;
}
static void ref_safe_floats(gcmContextData *ctx, f32 a, f32 b)
{
    ieee32 d0, d1; d0.f = a; d1.f = b;
    if (ctx->current + 3 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 3) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_DEPTH_BOUNDS_MIN, 2);
    ctx->current[1] = d0.u;
    ctx->current[2] = d1.u;
    ctx->current   += 3;
}
static void ref_unsafe_floats(gcmContextData *ctx, f32 a, f32 b)
{
    ieee32 d0, d1; d0.f = a; d1.f = b;
    ctx->current[0] = REF_METHOD(NV40TCL_DEPTH_BOUNDS_MIN, 2);
    ctx->current[1] = d0.u;
    ctx->current[2] = d1.u;
    ctx->current   += 3;
}
/* ---- blend / dither references (pre-patch rsx* bodies), in-test ---- */
static void ref_blend_func(gcmContextData *ctx, uint16_t sf,uint16_t df,uint16_t sa,uint16_t da)
{
    if (ctx->current + 3 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 3) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_BLEND_FUNC_SRC, 2);
    ctx->current[1] = ((uint32_t)sa << 16) | sf;
    ctx->current[2] = ((uint32_t)da << 16) | df;
    ctx->current   += 3;
}
static void ref_blend_equation(gcmContextData *ctx, uint16_t color, uint16_t alpha)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_BLEND_EQUATION, 1);
    ctx->current[1] = ((uint32_t)alpha << 16) | color;
    ctx->current   += 2;
}
static void ref_blend_color(gcmContextData *ctx, uint32_t c0, uint32_t c1)
{
    if (ctx->current + 4 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 4) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_BLEND_COLOR, 1);
    ctx->current[1] = c0;
    ctx->current[2] = REF_METHOD(NV40TCL_BLEND_COLOR2, 1);
    ctx->current[3] = c1;
    ctx->current   += 4;
}
/* unsafe (plain-write) twins */
static void ref_unsafe_blend_func(gcmContextData *ctx, uint16_t sf,uint16_t df,uint16_t sa,uint16_t da)
{
    ctx->current[0] = REF_METHOD(NV40TCL_BLEND_FUNC_SRC, 2);
    ctx->current[1] = ((uint32_t)sa << 16) | sf;
    ctx->current[2] = ((uint32_t)da << 16) | df;
    ctx->current   += 3;
}
static void ref_unsafe_blend_equation(gcmContextData *ctx, uint16_t color, uint16_t alpha)
{
    ctx->current[0] = REF_METHOD(NV40TCL_BLEND_EQUATION, 1);
    ctx->current[1] = ((uint32_t)alpha << 16) | color;
    ctx->current   += 2;
}
static void ref_unsafe_blend_color(gcmContextData *ctx, uint32_t c0, uint32_t c1)
{
    ctx->current[0] = REF_METHOD(NV40TCL_BLEND_COLOR, 1);
    ctx->current[1] = c0;
    ctx->current[2] = REF_METHOD(NV40TCL_BLEND_COLOR2, 1);
    ctx->current[3] = c1;
    ctx->current   += 4;
}
/* ---- stencil references: one header (count=3) + 3 data words (4 words) ---- */
static void ref_safe_triple(gcmContextData *ctx, uint32_t method, uint32_t a, uint32_t b, uint32_t c)
{
    if (ctx->current + 4 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 4) != 0) return;
    ctx->current[0] = REF_METHOD(method, 3);
    ctx->current[1] = a;
    ctx->current[2] = b;
    ctx->current[3] = c;
    ctx->current   += 4;
}
static void ref_unsafe_triple(gcmContextData *ctx, uint32_t method, uint32_t a, uint32_t b, uint32_t c)
{
    ctx->current[0] = REF_METHOD(method, 3);
    ctx->current[1] = a;
    ctx->current[2] = b;
    ctx->current[3] = c;
    ctx->current   += 4;
}
/* ---- raster-state extra references (3 words, header + 2 data) ---- */
static void ref_safe_pair3(gcmContextData *ctx, uint32_t method, uint32_t a, uint32_t b)
{
    if (ctx->current + 3 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 3) != 0) return;
    ctx->current[0] = REF_METHOD(method, 2);
    ctx->current[1] = a;
    ctx->current[2] = b;
    ctx->current   += 3;
}
static void ref_unsafe_pair3(gcmContextData *ctx, uint32_t method, uint32_t a, uint32_t b)
{
    ctx->current[0] = REF_METHOD(method, 2);
    ctx->current[1] = a;
    ctx->current[2] = b;
    ctx->current   += 3;
}

/* ---- fog / antialias / clip / fp / zminmax / render-enable / freq-divider ----
 * references: the pre-patch commands_impl.h bodies, in-test (byte-for-byte). */
static void ref_safe_fog_params(gcmContextData *ctx, f32 p0, f32 p1)
{
    ieee32 d0, d1; d0.f = p0; d1.f = p1;
    if (ctx->current + 3 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 3) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_FOG_EQUATION_CONSTANT, 2);
    ctx->current[1] = d0.u;
    ctx->current[2] = d1.u;
    ctx->current   += 3;
}
static void ref_unsafe_fog_params(gcmContextData *ctx, f32 p0, f32 p1)
{
    ieee32 d0, d1; d0.f = p0; d1.f = p1;
    ctx->current[0] = REF_METHOD(NV40TCL_FOG_EQUATION_CONSTANT, 2);
    ctx->current[1] = d0.u;
    ctx->current[2] = d1.u;
    ctx->current   += 3;
}
static void ref_safe_aa(gcmContextData *ctx, u32 en, u32 a2c, u32 a2o, u32 sm)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_ANTI_ALIASING_CONTROL, 1);
    ctx->current[1] = ((sm<<16) | (a2o<<8) | (a2c<<4) | en);
    ctx->current   += 2;
}
static void ref_unsafe_aa(gcmContextData *ctx, u32 en, u32 a2c, u32 a2o, u32 sm)
{
    ctx->current[0] = REF_METHOD(NV40TCL_ANTI_ALIASING_CONTROL, 1);
    ctx->current[1] = ((sm<<16) | (a2o<<8) | (a2c<<4) | en);
    ctx->current   += 2;
}
static void ref_safe_clip(gcmContextData *ctx, u32 p0,u32 p1,u32 p2,u32 p3,u32 p4,u32 p5)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_CLIP_PLANE_ENABLE, 1);
    ctx->current[1] = ((p5<<20)|(p4<<16)|(p3<<12)|(p2<<8)|(p1<<4)|p0);
    ctx->current   += 2;
}
static void ref_unsafe_clip(gcmContextData *ctx, u32 p0,u32 p1,u32 p2,u32 p3,u32 p4,u32 p5)
{
    ctx->current[0] = REF_METHOD(NV40TCL_CLIP_PLANE_ENABLE, 1);
    ctx->current[1] = ((p5<<20)|(p4<<16)|(p3<<12)|(p2<<8)|(p1<<4)|p0);
    ctx->current   += 2;
}
static void ref_safe_zminmax(gcmContextData *ctx, u32 cnf, u32 zc, u32 ciw)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_DEPTH_CONTROL, 1);
    ctx->current[1] = (cnf | (zc<<4) | (ciw<<8));
    ctx->current   += 2;
}
static void ref_unsafe_zminmax(gcmContextData *ctx, u32 cnf, u32 zc, u32 ciw)
{
    ctx->current[0] = REF_METHOD(NV40TCL_DEPTH_CONTROL, 1);
    ctx->current[1] = (cnf | (zc<<4) | (ciw<<8));
    ctx->current   += 2;
}
static void ref_safe_render(gcmContextData *ctx, u8 mode, u32 index)
{
    u32 offset = 0x10u*index;
    if (mode == 2 /* GCM_CONDITIONAL */) {
        if (ctx->current + 4 > ctx->end)
            if (ctx->callback && ctx->callback(ctx, 4) != 0) return;
        ctx->current[0] = REF_METHOD(NV40TCL_NOP, 1);
        ctx->current[1] = 0;
        ctx->current[2] = REF_METHOD(NV40TCL_RENDER_ENABLE, 1);
        ctx->current[3] = (0x2000000 | offset);
        ctx->current   += 4;
    } else {
        if (ctx->current + 2 > ctx->end)
            if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
        ctx->current[0] = REF_METHOD(NV40TCL_RENDER_ENABLE, 1);
        ctx->current[1] = 0x1000000;
        ctx->current   += 2;
    }
}
static void ref_unsafe_render(gcmContextData *ctx, u8 mode, u32 index)
{
    u32 offset = 0x10u*index;
    if (mode == 2) {
        ctx->current[0] = REF_METHOD(NV40TCL_NOP, 1);
        ctx->current[1] = 0;
        ctx->current[2] = REF_METHOD(NV40TCL_RENDER_ENABLE, 1);
        ctx->current[3] = (0x2000000 | offset);
        ctx->current   += 4;
    } else {
        ctx->current[0] = REF_METHOD(NV40TCL_RENDER_ENABLE, 1);
        ctx->current[1] = 0x1000000;
        ctx->current   += 2;
    }
}

/* ---- viewport / scissor / clear / zcull / scull references ---- */
static void ref_safe_scissor(gcmContextData *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (ctx->current + 3 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 3) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_SCISSOR_HORIZ, 2);
    ctx->current[1] = ((uint32_t)w << 16) | x;
    ctx->current[2] = ((uint32_t)h << 16) | y;
    ctx->current   += 3;
}
static void ref_unsafe_scissor(gcmContextData *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    ctx->current[0] = REF_METHOD(NV40TCL_SCISSOR_HORIZ, 2);
    ctx->current[1] = ((uint32_t)w << 16) | x;
    ctx->current[2] = ((uint32_t)h << 16) | y;
    ctx->current   += 3;
}
static void ref_safe_zcull_ctrl(gcmContextData *ctx, uint32_t dir, uint32_t fmt)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_ZCULL_CONTROL0, 1);
    ctx->current[1] = ((fmt << 4) | dir);
    ctx->current   += 2;
}
static void ref_unsafe_zcull_ctrl(gcmContextData *ctx, uint32_t dir, uint32_t fmt)
{
    ctx->current[0] = REF_METHOD(NV40TCL_ZCULL_CONTROL0, 1);
    ctx->current[1] = ((fmt << 4) | dir);
    ctx->current   += 2;
}
static void ref_safe_zcull_en(gcmContextData *ctx, uint32_t depth, uint32_t stencil)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_ZCULL_ENABLE, 1);
    ctx->current[1] = ((stencil << 1) | depth);
    ctx->current   += 2;
}
static void ref_unsafe_zcull_en(gcmContextData *ctx, uint32_t depth, uint32_t stencil)
{
    ctx->current[0] = REF_METHOD(NV40TCL_ZCULL_ENABLE, 1);
    ctx->current[1] = ((stencil << 1) | depth);
    ctx->current   += 2;
}
static void ref_safe_zcull_lim(gcmContextData *ctx, uint16_t mfl, uint16_t pb)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_ZCULL_CONTROL1, 1);
    ctx->current[1] = ((uint32_t)mfl << 16) | pb;
    ctx->current   += 2;
}
static void ref_unsafe_zcull_lim(gcmContextData *ctx, uint16_t mfl, uint16_t pb)
{
    ctx->current[0] = REF_METHOD(NV40TCL_ZCULL_CONTROL1, 1);
    ctx->current[1] = ((uint32_t)mfl << 16) | pb;
    ctx->current   += 2;
}
static void ref_safe_scull(gcmContextData *ctx, uint8_t func, uint8_t ref, uint8_t mask)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_SCULL_CONTROL, 1);
    ctx->current[1] = ((mask << 24) | (ref << 16) | func);
    ctx->current   += 2;
}
static void ref_unsafe_scull(gcmContextData *ctx, uint8_t func, uint8_t ref, uint8_t mask)
{
    ctx->current[0] = REF_METHOD(NV40TCL_SCULL_CONTROL, 1);
    ctx->current[1] = ((mask << 24) | (ref << 16) | func);
    ctx->current   += 2;
}
static void ref_safe_invalz(gcmContextData *ctx)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_INVALIDATE_ZCULL, 1);
    ctx->current[1] = 0;
    ctx->current   += 2;
}
static void ref_unsafe_invalz(gcmContextData *ctx)
{
    ctx->current[0] = REF_METHOD(NV40TCL_INVALIDATE_ZCULL, 1);
    ctx->current[1] = 0;
    ctx->current   += 2;
}

/* ---- texture-border / attrib-mask / zpass / clear-zcull / ref / caches / vertex-4f / report ---- */
static void ref_safe_texborder(gcmContextData *ctx, uint8_t index, uint32_t color)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_TEX_BORDER_COLOR(index), 1);
    ctx->current[1] = color;
    ctx->current   += 2;
}
static void ref_unsafe_texborder(gcmContextData *ctx, uint8_t index, uint32_t color)
{
    ctx->current[0] = REF_METHOD(NV40TCL_TEX_BORDER_COLOR(index), 1);
    ctx->current[1] = color;
    ctx->current   += 2;
}
static void ref_safe_vattrmask(gcmContextData *ctx, uint32_t mask)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_VP_RESULT_EN, 1);
    ctx->current[1] = mask;
    ctx->current   += 2;
}
static void ref_unsafe_vattrmask(gcmContextData *ctx, uint32_t mask)
{
    ctx->current[0] = REF_METHOD(NV40TCL_VP_RESULT_EN, 1);
    ctx->current[1] = mask;
    ctx->current   += 2;
}
static void ref_safe_zpass(gcmContextData *ctx, uint32_t enable)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_QUERY_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}
static void ref_unsafe_zpass(gcmContextData *ctx, uint32_t enable)
{
    ctx->current[0] = REF_METHOD(NV40TCL_QUERY_ENABLE, 1);
    ctx->current[1] = enable;
    ctx->current   += 2;
}
static void ref_safe_clearzcull(gcmContextData *ctx, uint32_t depth, uint32_t stencil)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_CLEAR_ZCULL_SURFACE, 1);
    ctx->current[1] = ((stencil << 1) | depth);
    ctx->current   += 2;
}
static void ref_unsafe_clearzcull(gcmContextData *ctx, uint32_t depth, uint32_t stencil)
{
    ctx->current[0] = REF_METHOD(NV40TCL_CLEAR_ZCULL_SURFACE, 1);
    ctx->current[1] = ((stencil << 1) | depth);
    ctx->current   += 2;
}
static void ref_safe_refcmd(gcmContextData *ctx, uint32_t ref_value)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV406ETCL_SET_REF, 1);
    ctx->current[1] = ref_value;
    ctx->current   += 2;
}
static void ref_unsafe_refcmd(gcmContextData *ctx, uint32_t ref_value)
{
    ctx->current[0] = REF_METHOD(NV406ETCL_SET_REF, 1);
    ctx->current[1] = ref_value;
    ctx->current   += 2;
}
static void ref_safe_invvert(gcmContextData *ctx)
{
    if (ctx->current + 8 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 8) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_VTX_CACHE_INVALIDATE2, 1);  ctx->current[1] = 0;
    ctx->current[2] = REF_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);   ctx->current[3] = 0;
    ctx->current[4] = REF_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);   ctx->current[5] = 0;
    ctx->current[6] = REF_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);   ctx->current[7] = 0;
    ctx->current   += 8;
}
static void ref_unsafe_invvert(gcmContextData *ctx)
{
    ctx->current[0] = REF_METHOD(NV40TCL_VTX_CACHE_INVALIDATE2, 1);  ctx->current[1] = 0;
    ctx->current[2] = REF_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);   ctx->current[3] = 0;
    ctx->current[4] = REF_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);   ctx->current[5] = 0;
    ctx->current[6] = REF_METHOD(NV40TCL_VTX_CACHE_INVALIDATE, 1);   ctx->current[7] = 0;
    ctx->current   += 8;
}
static void ref_safe_invtex(gcmContextData *ctx, uint32_t type)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_TEX_CACHE_CTL, 1);
    ctx->current[1] = type;
    ctx->current   += 2;
}
static void ref_unsafe_invtex(gcmContextData *ctx, uint32_t type)
{
    ctx->current[0] = REF_METHOD(NV40TCL_TEX_CACHE_CTL, 1);
    ctx->current[1] = type;
    ctx->current   += 2;
}
static void ref_safe_vtx4f(gcmContextData *ctx, uint8_t index, const f32 v[4])
{
    ieee32 w0, w1, w2, w3;
    w0.f = v[0]; w1.f = v[1]; w2.f = v[2]; w3.f = v[3];
    if (ctx->current + 5 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 5) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_VTX_ATTR_4F_X(index), 4);
    ctx->current[1] = w0.u; ctx->current[2] = w1.u; ctx->current[3] = w2.u; ctx->current[4] = w3.u;
    ctx->current   += 5;
}
static void ref_unsafe_vtx4f(gcmContextData *ctx, uint8_t index, const f32 v[4])
{
    ieee32 w0, w1, w2, w3;
    w0.f = v[0]; w1.f = v[1]; w2.f = v[2]; w3.f = v[3];
    ctx->current[0] = REF_METHOD(NV40TCL_VTX_ATTR_4F_X(index), 4);
    ctx->current[1] = w0.u; ctx->current[2] = w1.u; ctx->current[3] = w2.u; ctx->current[4] = w3.u;
    ctx->current   += 5;
}
static void ref_safe_clearrep(gcmContextData *ctx, uint32_t type)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_QUERY_RESET, 1);
    ctx->current[1] = type;
    ctx->current   += 2;
}
static void ref_unsafe_clearrep(gcmContextData *ctx, uint32_t type)
{
    ctx->current[0] = REF_METHOD(NV40TCL_QUERY_RESET, 1);
    ctx->current[1] = type;
    ctx->current   += 2;
}

/* surface (32 words) */
static void ref_safe_surface(gcmContextData *ctx, const CellGcmSurface *surface)
{
    uint32_t log2Width = 31 - __cntlzw(surface->width);
    uint32_t log2Height = 31 - __cntlzw(surface->height);
    if (ctx->current + 32 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 32) != 0) return;
    ctx->current[0]  = REF_METHOD(NV40TCL_DMA_COLOR0, 1);
    ctx->current[1]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[0];
    ctx->current[2]  = REF_METHOD(NV40TCL_DMA_COLOR1, 1);
    ctx->current[3]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[1];
    ctx->current[4]  = REF_METHOD(NV40TCL_DMA_COLOR2, 2);
    ctx->current[5]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[2];
    ctx->current[6]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[3];
    ctx->current[7]  = REF_METHOD(NV40TCL_DMA_ZETA, 1);
    ctx->current[8]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->depthLocation;
    ctx->current[9]  = REF_METHOD(NV40TCL_RT_FORMAT, 6);
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
    ctx->current[16] = REF_METHOD(NV40TCL_ZETA_PITCH, 1);
    ctx->current[17] = surface->depthPitch;
    ctx->current[18] = REF_METHOD(NV40TCL_COLOR2_PITCH, 4);
    ctx->current[19] = surface->colorPitch[2];
    ctx->current[20] = surface->colorPitch[3];
    ctx->current[21] = surface->colorOffset[2];
    ctx->current[22] = surface->colorOffset[3];
    ctx->current[23] = REF_METHOD(NV40TCL_RT_ENABLE, 1);
    ctx->current[24] = surface->colorTarget;
    ctx->current[25] = REF_METHOD(NV40TCL_WINDOW_OFFSET, 1);
    ctx->current[26] = ((surface->y << 16) | surface->x);
    ctx->current[27] = REF_METHOD(NV40TCL_RT_HORIZ, 2);
    ctx->current[28] = ((surface->width << 16) | surface->x);
    ctx->current[29] = ((surface->height << 16) | surface->y);
    ctx->current[30] = REF_METHOD(NV40TCL_SHADER_WINDOW, 1);
    ctx->current[31] = ((0 << 16) | (1 << 12) | surface->height);
    ctx->current    += 32;
}
static void ref_unsafe_surface(gcmContextData *ctx, const CellGcmSurface *surface)
{
    uint32_t log2Width = 31 - __cntlzw(surface->width);
    uint32_t log2Height = 31 - __cntlzw(surface->height);
    ctx->current[0]  = REF_METHOD(NV40TCL_DMA_COLOR0, 1);
    ctx->current[1]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[0];
    ctx->current[2]  = REF_METHOD(NV40TCL_DMA_COLOR1, 1);
    ctx->current[3]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[1];
    ctx->current[4]  = REF_METHOD(NV40TCL_DMA_COLOR2, 2);
    ctx->current[5]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[2];
    ctx->current[6]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->colorLocation[3];
    ctx->current[7]  = REF_METHOD(NV40TCL_DMA_ZETA, 1);
    ctx->current[8]  = GCM_DMA_MEMORY_FRAME_BUFFER + surface->depthLocation;
    ctx->current[9]  = REF_METHOD(NV40TCL_RT_FORMAT, 6);
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
    ctx->current[16] = REF_METHOD(NV40TCL_ZETA_PITCH, 1);
    ctx->current[17] = surface->depthPitch;
    ctx->current[18] = REF_METHOD(NV40TCL_COLOR2_PITCH, 4);
    ctx->current[19] = surface->colorPitch[2];
    ctx->current[20] = surface->colorPitch[3];
    ctx->current[21] = surface->colorOffset[2];
    ctx->current[22] = surface->colorOffset[3];
    ctx->current[23] = REF_METHOD(NV40TCL_RT_ENABLE, 1);
    ctx->current[24] = surface->colorTarget;
    ctx->current[25] = REF_METHOD(NV40TCL_WINDOW_OFFSET, 1);
    ctx->current[26] = ((surface->y << 16) | surface->x);
    ctx->current[27] = REF_METHOD(NV40TCL_RT_HORIZ, 2);
    ctx->current[28] = ((surface->width << 16) | surface->x);
    ctx->current[29] = ((surface->height << 16) | surface->y);
    ctx->current[30] = REF_METHOD(NV40TCL_SHADER_WINDOW, 1);
    ctx->current[31] = ((0 << 16) | (1 << 12) | surface->height);
    ctx->current    += 32;
}

/* vertex_data_array (4 words) */
static void ref_safe_vtxarray(gcmContextData *ctx, uint8_t index, uint16_t frequency,
                              uint8_t stride, uint8_t size, uint8_t type,
                              uint8_t location, uint32_t offset)
{
    if (ctx->current + 4 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 4) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_VTXFMT(index), 1);
    ctx->current[1] = ((frequency << 16) | (stride << NV40TCL_VTXFMT_STRIDE_SHIFT) | (size << NV40TCL_VTXFMT_SIZE_SHIFT) | type);
    ctx->current[2] = REF_METHOD(NV40TCL_VTXBUF_ADDRESS(index), 1);
    ctx->current[3] = ((location << 31) | offset);
    ctx->current   += 4;
}
static void ref_unsafe_vtxarray(gcmContextData *ctx, uint8_t index, uint16_t frequency,
                                uint8_t stride, uint8_t size, uint8_t type,
                                uint8_t location, uint32_t offset)
{
    ctx->current[0] = REF_METHOD(NV40TCL_VTXFMT(index), 1);
    ctx->current[1] = ((frequency << 16) | (stride << NV40TCL_VTXFMT_STRIDE_SHIFT) | (size << NV40TCL_VTXFMT_SIZE_SHIFT) | type);
    ctx->current[2] = REF_METHOD(NV40TCL_VTXBUF_ADDRESS(index), 1);
    ctx->current[3] = ((location << 31) | offset);
    ctx->current   += 4;
}

/* vertex_program_constants */
static void ref_safe_vp_consts(gcmContextData *ctx, uint32_t start, uint32_t count, const f32 *data)
{
    uint32_t loop = count >> 5;
    uint32_t rest = count & 0x1f;
    const f32 *value = data;
    uint32_t reserve = loop * 34 + (rest != 0 ? 2 + rest : 0);
    if (reserve == 0) return;
    if (ctx->current + reserve > ctx->end)
        if (ctx->callback && ctx->callback(ctx, reserve) != 0) return;
    for (uint32_t i = 0; i < loop; i++) {
        uint32_t loadAt = start + i * 8;
        ctx->current[0] = REF_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, 33);
        ctx->current[1] = loadAt;
        memcpy(&ctx->current[2], value, sizeof(f32) * 16);
        memcpy(&ctx->current[18], &value[16], sizeof(f32) * 16);
        ctx->current += 34;
        value += 32;
    }
    if (rest) {
        ctx->current[0] = REF_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, rest + 1);
        ctx->current[1] = start + (loop << 3);
        memcpy(&ctx->current[2], value, sizeof(f32) * rest);
        ctx->current += 2 + rest;
    }
}
static void ref_unsafe_vp_consts(gcmContextData *ctx, uint32_t start, uint32_t count, const f32 *data)
{
    uint32_t loop = count >> 5;
    uint32_t rest = count & 0x1f;
    const f32 *value = data;
    for (uint32_t i = 0; i < loop; i++) {
        uint32_t loadAt = start + i * 8;
        ctx->current[0] = REF_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, 33);
        ctx->current[1] = loadAt;
        memcpy(&ctx->current[2], value, sizeof(f32) * 16);
        memcpy(&ctx->current[18], &value[16], sizeof(f32) * 16);
        ctx->current += 34;
        value += 32;
    }
    if (rest) {
        ctx->current[0] = REF_METHOD(NV40TCL_VP_UPLOAD_CONST_ID, rest + 1);
        ctx->current[1] = start + (loop << 3);
        memcpy(&ctx->current[2], value, sizeof(f32) * rest);
        ctx->current += 2 + rest;
    }
}

/* update_fragment_program_parameter */
static void ref_safe_upd_fp(gcmContextData *ctx, uint32_t offset)
{
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_FP_ADDRESS, 1);
    ctx->current[1] = ((GCM_LOCATION_RSX + 1) | (offset & 0x1fffffff));
    ctx->current   += 2;
}
static void ref_unsafe_upd_fp(gcmContextData *ctx, uint32_t offset)
{
    ctx->current[0] = REF_METHOD(NV40TCL_FP_ADDRESS, 1);
    ctx->current[1] = ((GCM_LOCATION_RSX + 1) | (offset & 0x1fffffff));
    ctx->current   += 2;
}

/* nop_command */
static void ref_safe_nop(gcmContextData *ctx, uint32_t count)
{
    if (count == 0) return;
    if (ctx->current + count > ctx->end)
        if (ctx->callback && ctx->callback(ctx, count) != 0) return;
    for (uint32_t i = 0; i < count; i++)
        ctx->current[i] = 0;
    ctx->current += count;
}
static void ref_unsafe_nop(gcmContextData *ctx, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        ctx->current[i] = 0;
    ctx->current += count;
}

/* report_location */
static void ref_safe_report_loc(gcmContextData *ctx, uint32_t location)
{
    uint32_t handle = (location == GCM_LOCATION_CELL) ? 0xBAD68000 : 0x66626660;
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_DMA_QUERY, 1);
    ctx->current[1] = handle;
    ctx->current   += 2;
}
static void ref_unsafe_report_loc(gcmContextData *ctx, uint32_t location)
{
    uint32_t handle = (location == GCM_LOCATION_CELL) ? 0xBAD68000 : 0x66626660;
    ctx->current[0] = REF_METHOD(NV40TCL_DMA_QUERY, 1);
    ctx->current[1] = handle;
    ctx->current   += 2;
}

/* report */
static void ref_safe_report(gcmContextData *ctx, uint32_t type, uint32_t index)
{
    uint32_t offset = 0x10 * index;
    if (ctx->current + 2 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 2) != 0) return;
    ctx->current[0] = REF_METHOD(NV40TCL_QUERY_GET, 1);
    ctx->current[1] = ((type << 24) | offset);
    ctx->current   += 2;
}
static void ref_unsafe_report(gcmContextData *ctx, uint32_t type, uint32_t index)
{
    uint32_t offset = 0x10 * index;
    ctx->current[0] = REF_METHOD(NV40TCL_QUERY_GET, 1);
    ctx->current[1] = ((type << 24) | offset);
    ctx->current   += 2;
}

/* transfer_data_mode */
static void ref_safe_transfer_data_mode(gcmContextData *ctx, uint8_t mode)
{
    if (ctx->current + 3 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 3) != 0) return;
    ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 2);
    ctx->current[1] = (mode & 0x01) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[2] = (mode & 0x02) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current   += 3;
}
static void ref_unsafe_transfer_data_mode(gcmContextData *ctx, uint8_t mode)
{
    ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 2);
    ctx->current[1] = (mode & 0x01) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[2] = (mode & 0x02) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current   += 3;
}

/* transfer_data_offset */
static void ref_safe_transfer_data_offset(gcmContextData *ctx, uint32_t dst, uint32_t src)
{
    if (ctx->current + 8 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 8) != 0) return;
    ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN, 1);
    ctx->current[1] = src;
    ctx->current[2] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_OUT, 1);
    ctx->current[3] = dst;
    ctx->current[4] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_BUF_NOTIFY, 1);
    ctx->current[5] = 0;
    ctx->current[6] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_OUT, 1);
    ctx->current[7] = 0;
    ctx->current   += 8;
}
static void ref_unsafe_transfer_data_offset(gcmContextData *ctx, uint32_t dst, uint32_t src)
{
    ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN, 1);
    ctx->current[1] = src;
    ctx->current[2] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_OUT, 1);
    ctx->current[3] = dst;
    ctx->current[4] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_BUF_NOTIFY, 1);
    ctx->current[5] = 0;
    ctx->current[6] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_OUT, 1);
    ctx->current[7] = 0;
    ctx->current   += 8;
}

/* transfer_data_format */
static void ref_safe_transfer_data_format(gcmContextData *ctx, int32_t inpitch, int32_t outpitch,
                                          uint32_t linelength, uint32_t linecount,
                                          uint8_t inbytes, uint8_t outbytes)
{
    if (ctx->current + 6 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 6) != 0) return;
    ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_PITCH_IN, 5);
    ctx->current[1] = (uint32_t)inpitch;
    ctx->current[2] = (uint32_t)outpitch;
    ctx->current[3] = linelength;
    ctx->current[4] = linecount;
    ctx->current[5] = (((uint32_t)outbytes << 8) | inbytes);
    ctx->current   += 6;
}
static void ref_unsafe_transfer_data_format(gcmContextData *ctx, int32_t inpitch, int32_t outpitch,
                                            uint32_t linelength, uint32_t linecount,
                                            uint8_t inbytes, uint8_t outbytes)
{
    ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_PITCH_IN, 5);
    ctx->current[1] = (uint32_t)inpitch;
    ctx->current[2] = (uint32_t)outpitch;
    ctx->current[3] = linelength;
    ctx->current[4] = linecount;
    ctx->current[5] = (((uint32_t)outbytes << 8) | inbytes);
    ctx->current   += 6;
}

/* transfer_scale_mode */
static void ref_safe_transfer_scale_mode(gcmContextData *ctx, uint8_t mode, uint8_t surface)
{
    if (ctx->current + 6 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 6) != 0) return;
    ctx->current[0] = REF_SUBCHANNEL_METHOD(6, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 1);
    ctx->current[1] = (mode & 0x01) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[2] = (surface == GCM_TRANSFER_SWIZZLE)
        ? REF_SUBCHANNEL_METHOD(4, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 1)
        : REF_SUBCHANNEL_METHOD(3, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_OUT, 1);
    ctx->current[3] = (mode & 0x02) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[4] = REF_SUBCHANNEL_METHOD(6, NV01_IMAGE_FROM_CPU_SURFACE, 1);
    ctx->current[5] = (surface == GCM_TRANSFER_SWIZZLE) ? GCM_CONTEXT_SWIZZLE2D : GCM_CONTEXT_SURFACE2D;
    ctx->current   += 6;
}
static void ref_unsafe_transfer_scale_mode(gcmContextData *ctx, uint8_t mode, uint8_t surface)
{
    ctx->current[0] = REF_SUBCHANNEL_METHOD(6, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 1);
    ctx->current[1] = (mode & 0x01) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[2] = (surface == GCM_TRANSFER_SWIZZLE)
        ? REF_SUBCHANNEL_METHOD(4, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 1)
        : REF_SUBCHANNEL_METHOD(3, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_OUT, 1);
    ctx->current[3] = (mode & 0x02) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[4] = REF_SUBCHANNEL_METHOD(6, NV01_IMAGE_FROM_CPU_SURFACE, 1);
    ctx->current[5] = (surface == GCM_TRANSFER_SWIZZLE) ? GCM_CONTEXT_SWIZZLE2D : GCM_CONTEXT_SURFACE2D;
    ctx->current   += 6;
}

/* transfer_scale_surface */
static void ref_safe_transfer_scale_surface(gcmContextData *ctx,
                                            const gcmTransferScale *scale,
                                            const gcmTransferSurface *surface)
{
    if (ctx->current + 20 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 20) != 0) return;
    ctx->current[0] = REF_SUBCHANNEL_METHOD(3, NV04_CONTEXT_SURFACES_2D_FORMAT, 4);
    ctx->current[1] = surface->format;
    ctx->current[2] = ((uint32_t)surface->pitch << 16) | 0x40;
    ctx->current[3] = 0;
    ctx->current[4] = surface->offset;

    ctx->current[5] = REF_SUBCHANNEL_METHOD(6, NV03_STRETCHED_IMAGE_FROM_CPU_OPERATION, 9);
    ctx->current[6] = scale->conversion;
    ctx->current[7] = scale->format;
    ctx->current[8] = scale->operation;
    ctx->current[9] = ((scale->clipY << 16) | scale->clipX);
    ctx->current[10] = ((scale->clipH << 16) | scale->clipW);
    ctx->current[11] = ((scale->outY << 16) | scale->outX);
    ctx->current[12] = ((scale->outH << 16) | scale->outW);
    ctx->current[13] = (uint32_t)scale->ratioX;
    ctx->current[14] = (uint32_t)scale->ratioY;

    ctx->current[15] = REF_SUBCHANNEL_METHOD(6, NV03_SCALED_IMAGE_FROM_MEMORY_IMAGE_IN_SIZE, 4);
    ctx->current[16] = (((uint32_t)scale->inH << 16) | scale->inW);
    ctx->current[17] = ((uint32_t)scale->pitch | ((uint32_t)scale->origin << 16) | ((uint32_t)scale->interp << 24));
    ctx->current[18] = scale->offset;
    ctx->current[19] = (((uint32_t)scale->inY << 16) | scale->inX);
    ctx->current    += 20;
}
static void ref_unsafe_transfer_scale_surface(gcmContextData *ctx,
                                              const gcmTransferScale *scale,
                                              const gcmTransferSurface *surface)
{
    ctx->current[0] = REF_SUBCHANNEL_METHOD(3, NV04_CONTEXT_SURFACES_2D_FORMAT, 4);
    ctx->current[1] = surface->format;
    ctx->current[2] = ((uint32_t)surface->pitch << 16) | 0x40;
    ctx->current[3] = 0;
    ctx->current[4] = surface->offset;

    ctx->current[5] = REF_SUBCHANNEL_METHOD(6, NV03_STRETCHED_IMAGE_FROM_CPU_OPERATION, 9);
    ctx->current[6] = scale->conversion;
    ctx->current[7] = scale->format;
    ctx->current[8] = scale->operation;
    ctx->current[9] = ((scale->clipY << 16) | scale->clipX);
    ctx->current[10] = ((scale->clipH << 16) | scale->clipW);
    ctx->current[11] = ((scale->outY << 16) | scale->outX);
    ctx->current[12] = ((scale->outH << 16) | scale->outW);
    ctx->current[13] = (uint32_t)scale->ratioX;
    ctx->current[14] = (uint32_t)scale->ratioY;

    ctx->current[15] = REF_SUBCHANNEL_METHOD(6, NV03_SCALED_IMAGE_FROM_MEMORY_IMAGE_IN_SIZE, 4);
    ctx->current[16] = (((uint32_t)scale->inH << 16) | scale->inW);
    ctx->current[17] = ((uint32_t)scale->pitch | ((uint32_t)scale->origin << 16) | ((uint32_t)scale->interp << 24));
    ctx->current[18] = scale->offset;
    ctx->current[19] = (((uint32_t)scale->inY << 16) | scale->inX);
    ctx->current    += 20;
}

/* transfer_scale_swizzle */
static void ref_safe_transfer_scale_swizzle(gcmContextData *ctx,
                                            const gcmTransferScale *scale,
                                            const gcmTransferSwizzle *swizzle)
{
    if (ctx->current + 18 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 18) != 0) return;
    ctx->current[0] = REF_SUBCHANNEL_METHOD(4, NV04_SWIZZLED_SURFACE_FORMAT, 2);
    ctx->current[1] = (((uint32_t)swizzle->height << 24) | ((uint32_t)swizzle->width << 16) | swizzle->format);
    ctx->current[2] = swizzle->offset;

    ctx->current[3] = REF_SUBCHANNEL_METHOD(6, NV04_SCALED_IMAGE_FROM_MEMORY_COLOR_CONVERSION, 9);
    ctx->current[4] = GCM_TRANSFER_CONVERSION_TRUNCATE;
    ctx->current[5] = scale->format;
    ctx->current[6] = GCM_TRANSFER_OPERATION_SRCCOPY;
    ctx->current[7] = ((scale->clipY << 16) | scale->clipX);
    ctx->current[8] = (((uint32_t)scale->clipH << 16) | scale->clipW);
    ctx->current[9] = ((scale->outY << 16) | scale->outX);
    ctx->current[10] = (((uint32_t)scale->outH << 16) | scale->outW);
    ctx->current[11] = (uint32_t)scale->ratioX;
    ctx->current[12] = (uint32_t)scale->ratioY;

    ctx->current[13] = REF_SUBCHANNEL_METHOD(6, NV04_SCALED_IMAGE_FROM_MEMORY_SIZE, 4);
    ctx->current[14] = (((uint32_t)scale->inH << 16) | scale->inW);
    ctx->current[15] = (((uint32_t)scale->interp << 24) | ((uint32_t)scale->origin << 16) | (uint32_t)scale->pitch);
    ctx->current[16] = scale->offset;
    ctx->current[17] = (((uint32_t)scale->inY << 16) | scale->inX);
    ctx->current    += 18;
}

static void ref_unsafe_transfer_scale_swizzle(gcmContextData *ctx,
                                              const gcmTransferScale *scale,
                                              const gcmTransferSwizzle *swizzle)
{
    ctx->current[0] = REF_SUBCHANNEL_METHOD(4, NV04_SWIZZLED_SURFACE_FORMAT, 2);
    ctx->current[1] = (((uint32_t)swizzle->height << 24) | ((uint32_t)swizzle->width << 16) | swizzle->format);
    ctx->current[2] = swizzle->offset;

    ctx->current[3] = REF_SUBCHANNEL_METHOD(6, NV04_SCALED_IMAGE_FROM_MEMORY_COLOR_CONVERSION, 9);
    ctx->current[4] = GCM_TRANSFER_CONVERSION_TRUNCATE;
    ctx->current[5] = scale->format;
    ctx->current[6] = GCM_TRANSFER_OPERATION_SRCCOPY;
    ctx->current[7] = ((scale->clipY << 16) | scale->clipX);
    ctx->current[8] = (((uint32_t)scale->clipH << 16) | scale->clipW);
    ctx->current[9] = ((scale->outY << 16) | scale->outX);
    ctx->current[10] = (((uint32_t)scale->outH << 16) | scale->outW);
    ctx->current[11] = (uint32_t)scale->ratioX;
    ctx->current[12] = (uint32_t)scale->ratioY;

    ctx->current[13] = REF_SUBCHANNEL_METHOD(6, NV04_SCALED_IMAGE_FROM_MEMORY_SIZE, 4);
    ctx->current[14] = (((uint32_t)scale->inH << 16) | scale->inW);
    ctx->current[15] = (((uint32_t)scale->interp << 24) | ((uint32_t)scale->origin << 16) | (uint32_t)scale->pitch);
    ctx->current[16] = scale->offset;
    ctx->current[17] = (((uint32_t)scale->inY << 16) | scale->inX);
    ctx->current    += 18;
}

/* transfer_data */
static void ref_safe_transfer_data(gcmContextData *ctx, uint8_t mode,
                                   uint32_t dstOffset, int32_t dstPitch,
                                   uint32_t srcOffset, int32_t srcPitch,
                                   uint32_t bytesPerRow, uint32_t rowCount)
{
    const int32_t MIN_PITCH = -32768;
    const int32_t MAX_PITCH =  32767;
    const uint32_t MAX_ROWS = 0x7ff;
    const uint32_t MAX_LINES = 0x3fffff;
    uint32_t colCount, rows, cols, row;

    if (ctx->current + 3 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 3) != 0) return;
    ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 2);
    ctx->current[1] = (mode & 0x01) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[2] = (mode & 0x02) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current   += 3;

    if ((uint32_t)srcPitch == bytesPerRow && (uint32_t)dstPitch == bytesPerRow) {
        bytesPerRow *= rowCount;
        rowCount = 1;
        srcPitch = 0;
        dstPitch = 0;
    }

    if (srcPitch < MIN_PITCH || srcPitch > MAX_PITCH || dstPitch < MIN_PITCH || dstPitch > MAX_PITCH) {
        for (row = 0; row < rowCount; row++) {
            for (colCount = bytesPerRow; colCount > 0; colCount -= cols) {
                cols = (colCount > MAX_LINES) ? MAX_LINES : colCount;

                if (ctx->current + 9 > ctx->end)
                    if (ctx->callback && ctx->callback(ctx, 9) != 0) return;
                ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN, 8);
                ctx->current[1] = srcOffset + (bytesPerRow - colCount);
                ctx->current[2] = dstOffset + (bytesPerRow - colCount);
                ctx->current[3] = 0;
                ctx->current[4] = 0;
                ctx->current[5] = cols;
                ctx->current[6] = 1;
                ctx->current[7] = (((uint32_t)1 << 8) | 1);
                ctx->current[8] = 0;
                ctx->current   += 9;
            }
            srcOffset += (uint32_t)srcPitch;
            dstOffset += (uint32_t)dstPitch;
        }
    } else {
        for (; rowCount > 0; rowCount -= rows) {
            rows = (rowCount > MAX_ROWS) ? MAX_ROWS : rowCount;

            for (colCount = bytesPerRow; colCount > 0; colCount -= cols) {
                cols = (colCount > MAX_LINES) ? MAX_LINES : colCount;

                if (ctx->current + 9 > ctx->end)
                    if (ctx->callback && ctx->callback(ctx, 9) != 0) return;
                ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN, 8);
                ctx->current[1] = srcOffset + (bytesPerRow - colCount);
                ctx->current[2] = dstOffset + (bytesPerRow - colCount);
                ctx->current[3] = (uint32_t)srcPitch;
                ctx->current[4] = (uint32_t)dstPitch;
                ctx->current[5] = cols;
                ctx->current[6] = rows;
                ctx->current[7] = (((uint32_t)1 << 8) | 1);
                ctx->current[8] = 0;
                ctx->current   += 9;
            }

            srcOffset += rows * (uint32_t)srcPitch;
            dstOffset += rows * (uint32_t)dstPitch;
        }
    }
}

static void ref_unsafe_transfer_data(gcmContextData *ctx, uint8_t mode,
                                     uint32_t dstOffset, int32_t dstPitch,
                                     uint32_t srcOffset, int32_t srcPitch,
                                     uint32_t bytesPerRow, uint32_t rowCount)
{
    const int32_t MIN_PITCH = -32768;
    const int32_t MAX_PITCH =  32767;
    const uint32_t MAX_ROWS = 0x7ff;
    const uint32_t MAX_LINES = 0x3fffff;
    uint32_t colCount, rows, cols, row;

    ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 2);
    ctx->current[1] = (mode & 0x01) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[2] = (mode & 0x02) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current   += 3;

    if ((uint32_t)srcPitch == bytesPerRow && (uint32_t)dstPitch == bytesPerRow) {
        bytesPerRow *= rowCount;
        rowCount = 1;
        srcPitch = 0;
        dstPitch = 0;
    }

    if (srcPitch < MIN_PITCH || srcPitch > MAX_PITCH || dstPitch < MIN_PITCH || dstPitch > MAX_PITCH) {
        for (row = 0; row < rowCount; row++) {
            for (colCount = bytesPerRow; colCount > 0; colCount -= cols) {
                cols = (colCount > MAX_LINES) ? MAX_LINES : colCount;

                ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN, 8);
                ctx->current[1] = srcOffset + (bytesPerRow - colCount);
                ctx->current[2] = dstOffset + (bytesPerRow - colCount);
                ctx->current[3] = 0;
                ctx->current[4] = 0;
                ctx->current[5] = cols;
                ctx->current[6] = 1;
                ctx->current[7] = (((uint32_t)1 << 8) | 1);
                ctx->current[8] = 0;
                ctx->current   += 9;
            }
            srcOffset += (uint32_t)srcPitch;
            dstOffset += (uint32_t)dstPitch;
        }
    } else {
        for (; rowCount > 0; rowCount -= rows) {
            rows = (rowCount > MAX_ROWS) ? MAX_ROWS : rowCount;

            for (colCount = bytesPerRow; colCount > 0; colCount -= cols) {
                cols = (colCount > MAX_LINES) ? MAX_LINES : colCount;

                ctx->current[0] = REF_SUBCHANNEL_METHOD(1, NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN, 8);
                ctx->current[1] = srcOffset + (bytesPerRow - colCount);
                ctx->current[2] = dstOffset + (bytesPerRow - colCount);
                ctx->current[3] = (uint32_t)srcPitch;
                ctx->current[4] = (uint32_t)dstPitch;
                ctx->current[5] = cols;
                ctx->current[6] = rows;
                ctx->current[7] = (((uint32_t)1 << 8) | 1);
                ctx->current[8] = 0;
                ctx->current   += 9;
            }

            srcOffset += rows * (uint32_t)srcPitch;
            dstOffset += rows * (uint32_t)dstPitch;
        }
    }
}

/* transfer_image */
static void ref_safe_transfer_image(gcmContextData *ctx,
                                    uint8_t mode,
                                    uint32_t dstOffset, uint32_t dstPitch,
                                    uint32_t dstX, uint32_t dstY,
                                    uint32_t srcOffset, uint32_t srcPitch,
                                    uint32_t srcX, uint32_t srcY,
                                    uint32_t width, uint32_t height,
                                    uint32_t bytesPerPixel)
{
    const uint32_t SURFACE_MAX_DIM = 10;
    const uint32_t BLOCK_SIZE = 1 << SURFACE_MAX_DIM;
    uint32_t finalDstX, finalDstY, x, y;

    if (ctx->current + 6 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 6) != 0) return;
    ctx->current[0] = REF_SUBCHANNEL_METHOD(6, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 1);
    ctx->current[1] = (mode & 0x01) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[2] = REF_SUBCHANNEL_METHOD(3, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_OUT, 1);
    ctx->current[3] = (mode & 0x02) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[4] = REF_SUBCHANNEL_METHOD(6, NV01_IMAGE_FROM_CPU_SURFACE, 1);
    ctx->current[5] = GCM_CONTEXT_SURFACE2D;
    ctx->current   += 6;

    finalDstX = dstX + width;
    finalDstY = dstY + height;
    for (y = dstY; y < finalDstY;) {
        uint32_t dstTop = y & ~(BLOCK_SIZE - 1);
        uint32_t dstBot = dstTop + BLOCK_SIZE;
        uint32_t dstBltHeight = ((dstBot < finalDstY) ? dstBot : finalDstY) - y;

        for (x = dstX; x < finalDstX;) {
            uint32_t dstLeft = x & ~(BLOCK_SIZE - 1);
            uint32_t dstRight = dstLeft + BLOCK_SIZE;
            uint32_t dstBltWidth = ((dstRight < finalDstX) ? dstRight : finalDstX) - x;
            uint32_t dstBlockOffset = bytesPerPixel * (dstLeft & ~(BLOCK_SIZE - 1)) + dstPitch * dstTop;
            uint32_t srcBlockOffset = bytesPerPixel * (srcX + x - dstX) + srcPitch * (srcY + y - dstY);
            uint32_t safeDstBltWidth = (dstBltWidth < 16) ? 16 : (dstBltWidth + 1) & ~1;
            uint32_t destinOffset = dstOffset + dstBlockOffset;

            if (ctx->current + 20 > ctx->end)
                if (ctx->callback && ctx->callback(ctx, 20) != 0) return;
            ctx->current[0] = REF_SUBCHANNEL_METHOD(3, NV04_CONTEXT_SURFACES_2D_OFFSET_DESTIN, 1);
            ctx->current[1] = destinOffset;
            ctx->current[2] = REF_SUBCHANNEL_METHOD(3, NV04_CONTEXT_SURFACES_2D_FORMAT, 2);
            ctx->current[3] = (bytesPerPixel == 4) ? NV04_CONTEXT_SURFACES_2D_FORMAT_A8R8G8B8 : ((bytesPerPixel == 2) ? NV04_CONTEXT_SURFACES_2D_FORMAT_X1R5G5B5_X1R5G5B5 : 0);
            ctx->current[4] = ((dstPitch << 16) | dstPitch);
            ctx->current[5] = REF_SUBCHANNEL_METHOD(6, NV01_IMAGE_FROM_CPU_OPERATION, 9);
            ctx->current[6] = GCM_TRANSFER_CONVERSION_TRUNCATE;
            ctx->current[7] = (bytesPerPixel == 4) ? NV01_IMAGE_FROM_CPU_COLOR_FORMAT_A8R8G8B8 : ((bytesPerPixel == 2) ? NV01_IMAGE_FROM_CPU_COLOR_FORMAT_A1R5G5B5 : 0);
            ctx->current[8] = GCM_TRANSFER_OPERATION_SRCCOPY;
            ctx->current[9] = (((y - dstTop) << 16) | (x - dstLeft));
            ctx->current[10] = ((dstBltHeight << 16) | dstBltWidth);
            ctx->current[11] = (((y - dstTop) << 16) | (x - dstLeft));
            ctx->current[12] = ((dstBltHeight << 16) | dstBltWidth);
            ctx->current[13] = (1 << 20);
            ctx->current[14] = (1 << 20);
            ctx->current[15] = REF_SUBCHANNEL_METHOD(6, NV03_SCALED_IMAGE_FROM_MEMORY_IMAGE_IN_SIZE, 4);
            ctx->current[16] = ((dstBltHeight << 16) | safeDstBltWidth);
            ctx->current[17] = (srcPitch | (GCM_TRANSFER_ORIGIN_CORNER << 16) | (GCM_TRANSFER_INTERPOLATOR_NEAREST << 24));
            ctx->current[18] = srcOffset + srcBlockOffset;
            ctx->current[19] = 0;
            ctx->current    += 20;

            x += dstBltWidth;
        }
        y += dstBltHeight;
    }
}

static void ref_unsafe_transfer_image(gcmContextData *ctx,
                                      uint8_t mode,
                                      uint32_t dstOffset, uint32_t dstPitch,
                                      uint32_t dstX, uint32_t dstY,
                                      uint32_t srcOffset, uint32_t srcPitch,
                                      uint32_t srcX, uint32_t srcY,
                                      uint32_t width, uint32_t height,
                                      uint32_t bytesPerPixel)
{
    const uint32_t SURFACE_MAX_DIM = 10;
    const uint32_t BLOCK_SIZE = 1 << SURFACE_MAX_DIM;
    uint32_t finalDstX, finalDstY, x, y;

    ctx->current[0] = REF_SUBCHANNEL_METHOD(6, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN, 1);
    ctx->current[1] = (mode & 0x01) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[2] = REF_SUBCHANNEL_METHOD(3, NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_OUT, 1);
    ctx->current[3] = (mode & 0x02) ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
    ctx->current[4] = REF_SUBCHANNEL_METHOD(6, NV01_IMAGE_FROM_CPU_SURFACE, 1);
    ctx->current[5] = GCM_CONTEXT_SURFACE2D;
    ctx->current   += 6;

    finalDstX = dstX + width;
    finalDstY = dstY + height;
    for (y = dstY; y < finalDstY;) {
        uint32_t dstTop = y & ~(BLOCK_SIZE - 1);
        uint32_t dstBot = dstTop + BLOCK_SIZE;
        uint32_t dstBltHeight = ((dstBot < finalDstY) ? dstBot : finalDstY) - y;

        for (x = dstX; x < finalDstX;) {
            uint32_t dstLeft = x & ~(BLOCK_SIZE - 1);
            uint32_t dstRight = dstLeft + BLOCK_SIZE;
            uint32_t dstBltWidth = ((dstRight < finalDstX) ? dstRight : finalDstX) - x;
            uint32_t dstBlockOffset = bytesPerPixel * (dstLeft & ~(BLOCK_SIZE - 1)) + dstPitch * dstTop;
            uint32_t srcBlockOffset = bytesPerPixel * (srcX + x - dstX) + srcPitch * (srcY + y - dstY);
            uint32_t safeDstBltWidth = (dstBltWidth < 16) ? 16 : (dstBltWidth + 1) & ~1;
            uint32_t destinOffset = dstOffset + dstBlockOffset;

            ctx->current[0] = REF_SUBCHANNEL_METHOD(3, NV04_CONTEXT_SURFACES_2D_OFFSET_DESTIN, 1);
            ctx->current[1] = destinOffset;
            ctx->current[2] = REF_SUBCHANNEL_METHOD(3, NV04_CONTEXT_SURFACES_2D_FORMAT, 2);
            ctx->current[3] = (bytesPerPixel == 4) ? NV04_CONTEXT_SURFACES_2D_FORMAT_A8R8G8B8 : ((bytesPerPixel == 2) ? NV04_CONTEXT_SURFACES_2D_FORMAT_X1R5G5B5_X1R5G5B5 : 0);
            ctx->current[4] = ((dstPitch << 16) | dstPitch);
            ctx->current[5] = REF_SUBCHANNEL_METHOD(6, NV01_IMAGE_FROM_CPU_OPERATION, 9);
            ctx->current[6] = GCM_TRANSFER_CONVERSION_TRUNCATE;
            ctx->current[7] = (bytesPerPixel == 4) ? NV01_IMAGE_FROM_CPU_COLOR_FORMAT_A8R8G8B8 : ((bytesPerPixel == 2) ? NV01_IMAGE_FROM_CPU_COLOR_FORMAT_A1R5G5B5 : 0);
            ctx->current[8] = GCM_TRANSFER_OPERATION_SRCCOPY;
            ctx->current[9] = (((y - dstTop) << 16) | (x - dstLeft));
            ctx->current[10] = ((dstBltHeight << 16) | dstBltWidth);
            ctx->current[11] = (((y - dstTop) << 16) | (x - dstLeft));
            ctx->current[12] = ((dstBltHeight << 16) | dstBltWidth);
            ctx->current[13] = (1 << 20);
            ctx->current[14] = (1 << 20);
            ctx->current[15] = REF_SUBCHANNEL_METHOD(6, NV03_SCALED_IMAGE_FROM_MEMORY_IMAGE_IN_SIZE, 4);
            ctx->current[16] = ((dstBltHeight << 16) | safeDstBltWidth);
            ctx->current[17] = (srcPitch | (GCM_TRANSFER_ORIGIN_CORNER << 16) | (GCM_TRANSFER_INTERPOLATOR_NEAREST << 24));
            ctx->current[18] = srcOffset + srcBlockOffset;
            ctx->current[19] = 0;
            ctx->current    += 20;

            x += dstBltWidth;
        }
        y += dstBltHeight;
    }
}
static void ref_safe_viewport(gcmContextData *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              f32 min, f32 max, const f32 scale[4], const f32 offset[4])
{
    ieee32 _min, _max; ieee32 _offset[4], _scale[4];
    _min.f = min; _max.f = max;
    _scale[0].f = scale[0]; _scale[1].f = scale[1]; _scale[2].f = scale[2]; _scale[3].f = scale[3];
    _offset[0].f = offset[0]; _offset[1].f = offset[1]; _offset[2].f = offset[2]; _offset[3].f = offset[3];
    if (ctx->current + 24 > ctx->end)
        if (ctx->callback && ctx->callback(ctx, 24) != 0) return;
    ctx->current[0]  = REF_METHOD(NV40TCL_VIEWPORT_HORIZ, 2);
    ctx->current[1]  = ((uint32_t)w << 16) | x;
    ctx->current[2]  = ((uint32_t)h << 16) | y;
    ctx->current[3]  = REF_METHOD(NV40TCL_DEPTH_RANGE, 2);
    ctx->current[4]  = _min.u;
    ctx->current[5]  = _max.u;
    ctx->current[6]  = REF_METHOD(NV40TCL_VIEWPORT_OFFSET, 8);
    ctx->current[7]  = _offset[0].u; ctx->current[8]  = _offset[1].u; ctx->current[9]  = _offset[2].u; ctx->current[10] = _offset[3].u;
    ctx->current[11] = _scale[0].u;  ctx->current[12] = _scale[1].u;  ctx->current[13] = _scale[2].u;  ctx->current[14] = _scale[3].u;
    ctx->current[15] = REF_METHOD(NV40TCL_VIEWPORT_OFFSET, 8);
    ctx->current[16] = _offset[0].u; ctx->current[17] = _offset[1].u; ctx->current[18] = _offset[2].u; ctx->current[19] = _offset[3].u;
    ctx->current[20] = _scale[0].u;  ctx->current[21] = _scale[1].u;  ctx->current[22] = _scale[2].u;  ctx->current[23] = _scale[3].u;
    ctx->current   += 24;
}
static void ref_unsafe_viewport(gcmContextData *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                f32 min, f32 max, const f32 scale[4], const f32 offset[4])
{
    ieee32 _min, _max; ieee32 _offset[4], _scale[4];
    _min.f = min; _max.f = max;
    _scale[0].f = scale[0]; _scale[1].f = scale[1]; _scale[2].f = scale[2]; _scale[3].f = scale[3];
    _offset[0].f = offset[0]; _offset[1].f = offset[1]; _offset[2].f = offset[2]; _offset[3].f = offset[3];
    ctx->current[0]  = REF_METHOD(NV40TCL_VIEWPORT_HORIZ, 2);
    ctx->current[1]  = ((uint32_t)w << 16) | x;
    ctx->current[2]  = ((uint32_t)h << 16) | y;
    ctx->current[3]  = REF_METHOD(NV40TCL_DEPTH_RANGE, 2);
    ctx->current[4]  = _min.u;
    ctx->current[5]  = _max.u;
    ctx->current[6]  = REF_METHOD(NV40TCL_VIEWPORT_OFFSET, 8);
    ctx->current[7]  = _offset[0].u; ctx->current[8]  = _offset[1].u; ctx->current[9]  = _offset[2].u; ctx->current[10] = _offset[3].u;
    ctx->current[11] = _scale[0].u;  ctx->current[12] = _scale[1].u;  ctx->current[13] = _scale[2].u;  ctx->current[14] = _scale[3].u;
    ctx->current[15] = REF_METHOD(NV40TCL_VIEWPORT_OFFSET, 8);
    ctx->current[16] = _offset[0].u; ctx->current[17] = _offset[1].u; ctx->current[18] = _offset[2].u; ctx->current[19] = _offset[3].u;
    ctx->current[20] = _scale[0].u;  ctx->current[21] = _scale[1].u;  ctx->current[22] = _scale[2].u;  ctx->current[23] = _scale[3].u;
    ctx->current   += 24;
}

/* -------------------------------------------------------------------- *
 */
static int failures = 0;
static void cmp(const char *label, const uint32_t *nw, const uint32_t *rf, size_t nwords)
{
    printf("  %-34s [", label);
    for (size_t i = 0; i < nwords; i++) printf("0x%08x ", nw[i]);
    printf("]");
    printf("   pre=[");
    for (size_t i = 0; i < nwords; i++) printf("0x%08x ", rf[i]);
    printf("]");
    if (memcmp(nw, rf, nwords * sizeof(*nw)) == 0) printf("  ok\n");
    else { printf("  MISMATCH\n"); failures++; }
}

static void cmp_mutant(const char *label, const uint32_t *nw, const uint32_t *rf_mut, size_t nwords)
{
    printf("  %-34s", label);
    if (memcmp(nw, rf_mut, nwords * sizeof(*nw)) != 0) {
        printf("  ok (swap mutant rejected, went red)\n");
    } else {
        printf("  FAIL (swap mutant accepted!)\n");
        failures++;
    }
}

/* safe/unsafe driver for the 2-word families. */
static void drive_uint(const char *name,
                       void (*s)(CellGcmContextData *, uint32_t),
                       void (*u)(CellGcmContextData *, uint32_t),
                       uint32_t method, uint32_t val)
{
    static uint32_t A[4], B[4];
    gcmContextData n, r;
    memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
    n.begin = n.current = A;  n.end = A + 4; n.callback = 0;
    r.begin = r.current = B;  r.end = B + 4; r.callback = 0;
    s((CellGcmContextData *)&n, val); ref_safe1(&r, method, val);
    cmp(name, A, B, 2);

    n.begin = n.current = A; r.begin = r.current = B;
    u((CellGcmContextData *)&n, val); ref_unsafe1(&r, method, val);
    cmp(name, A, B, 2);
}

/* -------------------------------------------------------------------- *
 * Ring-full / callback semantics (point 3 of the depth-group review):
 *   with end == current (zero room) and a recording callback,
 *   - SAFE   natives must invoke the callback exactly ONCE with the
 *            emitter's reserved count, then proceed to write;
 *   - UNSAFE natives must never invoke the callback (they write plainly).
 *
 * One callback recorder is shared between the safe and unsafe sub-tasks
 * for one native so we get a clean per-native report.
 * -------------------------------------------------------------------- */
static int g_spy_calls;
static uint32_t g_spy_count;
static int32_t spy_cb(gcmContextData *ctx, uint32_t count)
{
    g_spy_calls++;
    g_spy_count = count;
    ctx->end += count;          /* "compact": make room so the safe write proceeds */
    return 0;
}

static void report_ring(const char *label, char kind, uint32_t expected_count)
{
    char tag[128];
    if (kind == 's')
        snprintf(tag, sizeof tag,
                 "  ring-full %-28s safe   (calls=%d count=0x%08x, expect exactly 1 call of %u)",
                 label, g_spy_calls, g_spy_count, expected_count);
    else
        snprintf(tag, sizeof tag,
                 "  ring-full %-28s unsafe (calls=%d, expect 0)",
                 label, g_spy_calls);
    int pass = (kind == 's')
        ? (g_spy_calls == 1 && g_spy_count == expected_count)
        : (g_spy_calls == 0);
    printf("%s  %s\n", tag, pass ? "ok" : "MISMATCH");
    if (!pass) failures++;
}

/* Invoke `SAFE_N` once against a full ring, then `UNSAFE_N` once with
 * the given variadic args; check safe called the callback exactly once
 * with EXPECTED, unsafe called it zero times.  Real function pointers,
 * no void* casts — works the same for uint-pair and float-pair natives. */
#define RING_CHECK(SAFE_N, UNSAFE_N, LABEL, EXPECTED, ...) do { \
        g_spy_calls = 0; g_spy_count = 0; \
        { gcmContextData rf; memset(&rf, 0, sizeof rf); \
          uint32_t _rb[128]; \
          rf.begin = rf.current = _rb; rf.end = rf.current; rf.callback = spy_cb; \
          SAFE_N(&rf, ##__VA_ARGS__); \
          report_ring(LABEL, 's', EXPECTED); } \
        g_spy_calls = 0; g_spy_count = 0; \
        { gcmContextData rf; memset(&rf, 0, sizeof rf); \
          uint32_t _rb[128]; \
          rf.begin = rf.current = _rb; rf.end = rf.current; rf.callback = spy_cb; \
          UNSAFE_N(&rf, ##__VA_ARGS__); \
          report_ring(LABEL, 'u', 0); } \
    } while (0)

int main(void)
{
    printf("  [aarch64 host]  natives (safe+unsafe) vs pre-patch rsx* body\n\n");

    /* ---- logic-op family ---- */
    printf("  Logic-OP\n");
    for (uint32_t v = 0x1500; v <= 0x150f; ++v)
        drive_uint("  logic_op",
                   ps3tc_gcm_state_logic_op,
                   ps3tc_gcm_state_logic_op_unsafe,
                   NV40TCL_COLOR_LOGIC_OP, v);
    for (uint32_t v = 0; v <= 2; ++v)
        drive_uint("  logic_op_enable",
                   ps3tc_gcm_state_logic_op_enable,
                   ps3tc_gcm_state_logic_op_enable_unsafe,
                   NV40TCL_COLOR_LOGIC_OP_ENABLE, v);

    /* ---- depth family (2-word) ---- */
    printf("\n  Depth (2-word)\n");
    for (uint32_t v = 0; v <= 3; ++v)
        drive_uint("  depth_test_enable",
                   ps3tc_gcm_state_depth_test_enable,
                   ps3tc_gcm_state_depth_test_enable_unsafe,
                   NV40TCL_DEPTH_TEST_ENABLE, v);
    for (uint32_t v = 0; v <= 4; ++v)
        drive_uint("  depth_func",
                   ps3tc_gcm_state_depth_func,
                   ps3tc_gcm_state_depth_func_unsafe,
                   NV40TCL_DEPTH_FUNC, v);
    for (uint32_t v = 0; v <= 3; ++v)
        drive_uint("  depth_mask (write_enable)",
                   ps3tc_gcm_state_depth_write_enable,
                   ps3tc_gcm_state_depth_write_enable_unsafe,
                   NV40TCL_DEPTH_WRITE_ENABLE, v);
    for (uint32_t v = 0; v <= 2; ++v)
        drive_uint("  depth_bounds_test_enable",
                   ps3tc_gcm_state_depth_bounds_test_enable,
                   ps3tc_gcm_state_depth_bounds_test_enable_unsafe,
                   NV40TCL_DEPTH_BOUNDS_TEST_ENABLE, v);

    /* ---- depth_bounds (floats): compare Bit Patterns, not values ---- */
    printf("\n  Depth bounds (float bit-patterns)\n");
    /* depth_bounds (floats): compare the IEEE-32 bit patterns, not values.
       Use explicit bit patterns for NaN / inf / -0.0 / arbitrary. */
    struct { uint32_t mn; uint32_t mx; } raws[] = {
        { 0x3F800000u, 0x3F800000u },   /* (1.0, 1.0) */
        { 0x00000000u, 0x7F800000u },   /* (0.0, +inf) */
        { 0x80000000u, 0xBF800000u },   /* (-0.0, -1.0) */
        { 0x7FC00000u, 0x7D260000u },   /* (NaN, 2.0)        — distinct bit patterns */
        { 0x7FC00000u, 0x7FFFFFFFu },   /* (NaN, most-positive-NaN) */
        { 0x3DCCCCCDu, 0x40490FDBu },   /* (0.1, pi)         — 0x40490FDB == 3.14159265 */
    };
    static uint32_t A[8], B[8];
    gcmContextData n, r;
    f32 a, b;
    for (size_t i = 0; i < sizeof raws / sizeof raws[0]; ++i) {
        memcpy(&a, &raws[i].mn, 4); memcpy(&b, &raws[i].mx, 4);

        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_depth_bounds((CellGcmContextData *)&n, a, b);
        ref_safe_floats(&r, a, b);
        char lab[48]; snprintf(lab, sizeof lab, "  depth_bounds raw %u", (unsigned)i);
        cmp(lab, A, B, 3);

        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_depth_bounds_unsafe((CellGcmContextData *)&n, a, b);
        ref_unsafe_floats(&r, a, b);
        snprintf(lab, sizeof lab, "  depth_bounds raw %u", (unsigned)i);
        cmp(lab, A, B, 3);
    }

    /* ---- blend / dither family ---- */
    /* blend_enable + dither_enable: 2-word, same shape as depth enables. */
    printf("\n  Blend / dither (enable, 2-word)\n");
    for (uint32_t v = 0; v <= 2; ++v)
        drive_uint("  dither_enable",
                   ps3tc_gcm_state_dither_enable,
                   ps3tc_gcm_state_dither_enable_unsafe,
                   NV40TCL_DITHER_ENABLE, v);
    for (uint32_t v = 0; v <= 2; ++v)
        drive_uint("  blend_enable",
                   ps3tc_gcm_state_blend_enable,
                   ps3tc_gcm_state_blend_enable_unsafe,
                   NV40TCL_BLEND_ENABLE, v);

    /* blend_equation: 2 words, word1 = (alpha<<16)|color. */
    printf("\n  Blend equation (2-word, packed 16x2)\n");
    {
        const struct { uint16_t c, a; } eqs[] = {
            { 0x0000, 0x0000 }, { 0x0102, 0x0304 },
            { 0xFFFF, 0xFFFF }, { 0x1234, 0x5678 }, { 0x00FF, 0xFF00 },
        };
        for (size_t i = 0; i < sizeof eqs / sizeof eqs[0]; ++i) {
            uint16_t c = eqs[i].c, a = eqs[i].a;
            static uint32_t A[4], B[4];
            gcmContextData n, r;
            memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
            n.begin = n.current = A; n.end = A + 4; n.callback = 0;
            r.begin = r.current = B; r.end = B + 4; r.callback = 0;
            ps3tc_gcm_state_blend_equation((CellGcmContextData *)&n, c, a);
            ref_blend_equation(&r, c, a);
            char lab[48]; snprintf(lab, sizeof lab, "  blend_equation %u", (unsigned)i);
            cmp(lab, A, B, 2);
            n.begin = n.current = A; r.begin = r.current = B;
            ps3tc_gcm_state_blend_equation_unsafe((CellGcmContextData *)&n, c, a);
            ref_unsafe_blend_equation(&r, c, a);
            snprintf(lab, sizeof lab, "  blend_equation %u", (unsigned)i);
            cmp(lab, A, B, 2);
        }
    }

    /* blend_func: 3 words, word1=(sa<<16)|sc, word2=(da<<16)|dc. */
    printf("\n  Blend func (3-word, packed 16x4)\n");
    {
        const struct { uint16_t sc, dc, sa, da; } fcs[] = {
            { 0x0000, 0x0000, 0x0000, 0x0000 },
            { 0x0102, 0x0304, 0x0607, 0x0809 },
            { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF },
            { 0x1234, 0x5678, 0x9ABC, 0xDE00 },
        };
        for (size_t i = 0; i < sizeof fcs / sizeof fcs[0]; ++i) {
            uint16_t sc=fcs[i].sc, dc=fcs[i].dc, sa=fcs[i].sa, da=fcs[i].da;
            static uint32_t A[8], B[8];
            gcmContextData n, r;
            memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
            n.begin = n.current = A; n.end = A + 8; n.callback = 0;
            r.begin = r.current = B; r.end = B + 8; r.callback = 0;
            ps3tc_gcm_state_blend_func((CellGcmContextData *)&n, sc, dc, sa, da);
            ref_blend_func(&r, sc, dc, sa, da);
            char lab[48]; snprintf(lab, sizeof lab, "  blend_func %u", (unsigned)i);
            cmp(lab, A, B, 3);
            n.begin = n.current = A; r.begin = r.current = B;
            ps3tc_gcm_state_blend_func_unsafe((CellGcmContextData *)&n, sc, dc, sa, da);
            ref_unsafe_blend_func(&r, sc, dc, sa, da);
            snprintf(lab, sizeof lab, "  blend_func %u", (unsigned)i);
            cmp(lab, A, B, 3);
        }
    }

    /* blend_color: 4 words — TWO method packets. */
    printf("\n  Blend color (4-word, two method packets)\n");
    {
        const uint32_t c0s[] = { 0x11111111u, 0x00000000u, 0xFFFFFFFFu, 0x80000000u };
        const uint32_t c1s[] = { 0x22222222u, 0xDEADBEEFu, 0x000000FFu, 0x7F800000u };
        for (size_t i = 0; i < sizeof c0s / sizeof c0s[0]; ++i) {
            uint32_t c0 = c0s[i], c1 = c1s[i];
            static uint32_t A[8], B[8];
            gcmContextData n, r;
            memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
            n.begin = n.current = A; n.end = A + 8; n.callback = 0;
            r.begin = r.current = B; r.end = B + 8; r.callback = 0;
            ps3tc_gcm_state_blend_color((CellGcmContextData *)&n, c0, c1);
            ref_blend_color(&r, c0, c1);
            char lab[48]; snprintf(lab, sizeof lab, "  blend_color %u", (unsigned)i);
            cmp(lab, A, B, 4);
            n.begin = n.current = A; r.begin = r.current = B;
            ps3tc_gcm_state_blend_color_unsafe((CellGcmContextData *)&n, c0, c1);
            ref_unsafe_blend_color(&r, c0, c1);
            snprintf(lab, sizeof lab, "  blend_color %u", (unsigned)i);
            cmp(lab, A, B, 4);
        }
    }

    /* ---- stencil group ----
     * 2-word (count=1) and 4-word (count=3 under one header) families.
     * Register names follow rsx/nv40.h; the mapping is unchanged from the
     * pre-patch rsx* bodies. */
    printf("\n  Stencil (2-word enables/mask)\n");
    for (uint32_t v = 0; v <= 2; ++v)
        drive_uint("  stencil_test_enable",
                   ps3tc_gcm_state_stencil_test_enable,
                   ps3tc_gcm_state_stencil_test_enable_unsafe,
                   NV40TCL_STENCIL_BACK_ENABLE, v);
    for (uint32_t v = 0; v <= 2; ++v)
        drive_uint("  two_sided_stencil_enable",
                   ps3tc_gcm_state_two_sided_stencil_test_enable,
                   ps3tc_gcm_state_two_sided_stencil_test_enable_unsafe,
                   NV40TCL_STENCIL_FRONT_ENABLE, v);
    for (uint32_t v = 0x00u; v <= 0x06u; ++v)
        drive_uint("  stencil_mask",
                   ps3tc_gcm_state_stencil_mask,
                   ps3tc_gcm_state_stencil_mask_unsafe,
                   NV40TCL_STENCIL_BACK_MASK, (v ^ 0x55) * 0x11111111u);
    for (uint32_t v = 0x00u; v <= 0x06u; ++v)
        drive_uint("  back_stencil_mask",
                   ps3tc_gcm_state_back_stencil_mask,
                   ps3tc_gcm_state_back_stencil_mask_unsafe,
                   NV40TCL_STENCIL_FRONT_MASK, (v ^ 0x55) * 0x11111111u);

    /* stencil_func / stencil_op / back_stencil_*: 4 words, count=3 header. */
    printf("\n  Stencil func/op (4-word, count=3 under one header)\n");
    {
        const struct { uint32_t x, y, z; } tris[] = {
            { 0x0000, 0x0000, 0x0000 },
            { 0x0001, 0x0002, 0x0003 },
            { 0x150a, 0x1e01, 0x1e02 },   /* INVERT / REPLACE / INCR — realistic op values */
            { 0x1234, 0x5678, 0xABCD },
            { 0xFFFF, 0xFFFF, 0xFFFF },
        };
        for (size_t i = 0; i < sizeof tris / sizeof tris[0]; ++i) {
            uint32_t a = tris[i].x, b = tris[i].y, c = tris[i].z;
            static uint32_t A[8], B[8];
            gcmContextData n, r; char lab[48];
            /* stencil_func (BACK_FUNC_FUNC) */
            memset(&n,0,sizeof n); memset(&r,0,sizeof r);
            n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
            ps3tc_gcm_state_stencil_func((CellGcmContextData *)&n, a, b, c);
            ref_safe_triple(&r, NV40TCL_STENCIL_BACK_FUNC_FUNC, a, b, c);
            snprintf(lab,sizeof lab,"  stencil_func %u",(unsigned)i); cmp(lab,A,B,4);
            n.begin=n.current=A; r.begin=r.current=B;
            ps3tc_gcm_state_stencil_func_unsafe((CellGcmContextData *)&n, a, b, c);
            ref_unsafe_triple(&r, NV40TCL_STENCIL_BACK_FUNC_FUNC, a, b, c);
            cmp(lab,A,B,4);
            /* stencil_op (BACK_OP_FAIL) */
            memset(&n,0,sizeof n); memset(&r,0,sizeof r);
            n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
            ps3tc_gcm_state_stencil_op((CellGcmContextData *)&n, a, b, c);
            ref_safe_triple(&r, NV40TCL_STENCIL_BACK_OP_FAIL, a, b, c);
            snprintf(lab,sizeof lab,"  stencil_op %u",(unsigned)i); cmp(lab,A,B,4);
            n.begin=n.current=A; r.begin=r.current=B;
            ps3tc_gcm_state_stencil_op_unsafe((CellGcmContextData *)&n, a, b, c);
            ref_unsafe_triple(&r, NV40TCL_STENCIL_BACK_OP_FAIL, a, b, c);
            cmp(lab,A,B,4);
            /* back_stencil_func (FRONT_FUNC_FUNC) */
            memset(&n,0,sizeof n); memset(&r,0,sizeof r);
            n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
            ps3tc_gcm_state_back_stencil_func((CellGcmContextData *)&n, a, b, c);
            ref_safe_triple(&r, NV40TCL_STENCIL_FRONT_FUNC_FUNC, a, b, c);
            snprintf(lab,sizeof lab,"  back_stencil_func %u",(unsigned)i); cmp(lab,A,B,4);
            n.begin=n.current=A; r.begin=r.current=B;
            ps3tc_gcm_state_back_stencil_func_unsafe((CellGcmContextData *)&n, a, b, c);
            ref_unsafe_triple(&r, NV40TCL_STENCIL_FRONT_FUNC_FUNC, a, b, c);
            cmp(lab,A,B,4);
            /* back_stencil_op (FRONT_OP_FAIL) */
            memset(&n,0,sizeof n); memset(&r,0,sizeof r);
            n.begin=n.current=A; n.end=A+8; r.begin=r.current=B; r.end=B+8;
            ps3tc_gcm_state_back_stencil_op((CellGcmContextData *)&n, a, b, c);
            ref_safe_triple(&r, NV40TCL_STENCIL_FRONT_OP_FAIL, a, b, c);
            snprintf(lab,sizeof lab,"  back_stencil_op %u",(unsigned)i); cmp(lab,A,B,4);
            n.begin=n.current=A; r.begin=r.current=B;
             ps3tc_gcm_state_back_stencil_op_unsafe((CellGcmContextData *)&n, a, b, c);
             ref_unsafe_triple(&r, NV40TCL_STENCIL_FRONT_OP_FAIL, a, b, c);
             cmp(lab,A,B,4);
         }
     }

    /* ---- raster state — 2-word natives (12 of 14) ---- */
    printf("\n  Raster state (2-word, 32-bit value)\n");
    {
        const uint32_t vals[] = { 0x00u, 0x01u, 0x5Fu, 0xFFu, 0x1234u, 0xABC5u, 0xFFFEu, 0xFFFFu };
        for (size_t vi = 0; vi < sizeof vals / sizeof vals[0]; ++vi) {
            uint32_t v = vals[vi];
            drive_uint("color_mask",  ps3tc_gcm_state_color_mask,  ps3tc_gcm_state_color_mask_unsafe,  NV40TCL_COLOR_MASK,  v);
            drive_uint("color_mask_mrt", ps3tc_gcm_state_color_mask_mrt, ps3tc_gcm_state_color_mask_mrt_unsafe, NV40TCL_MRT_COLOR_MASK, v);
            drive_uint("shade_model", ps3tc_gcm_state_shade_model,  ps3tc_gcm_state_shade_model_unsafe,  NV40TCL_SHADE_MODEL, v);
            drive_uint("cull_face_enable", ps3tc_gcm_state_cull_face_enable, ps3tc_gcm_state_cull_face_enable_unsafe, NV40TCL_CULL_FACE_ENABLE, v);
            drive_uint("cull_face", ps3tc_gcm_state_cull_face,          ps3tc_gcm_state_cull_face_unsafe,          NV40TCL_CULL_FACE,      v);
            drive_uint("front_face",ps3tc_gcm_state_front_face,         ps3tc_gcm_state_front_face_unsafe,         NV40TCL_FRONT_FACE,     v);
            drive_uint("front_polygon_mode", ps3tc_gcm_state_front_polygon_mode, ps3tc_gcm_state_front_polygon_mode_unsafe, NV40TCL_POLYGON_MODE_FRONT, v);
            drive_uint("back_polygon_mode",  ps3tc_gcm_state_back_polygon_mode,  ps3tc_gcm_state_back_polygon_mode_unsafe,  NV40TCL_POLYGON_MODE_BACK, v);
            drive_uint("polygon_offset_fill_enable", ps3tc_gcm_state_polygon_offset_fill_enable, ps3tc_gcm_state_polygon_offset_fill_enable_unsafe, NV40TCL_POLYGON_OFFSET_FILL_ENABLE, v);
            drive_uint("line_smooth_enable", ps3tc_gcm_state_line_smooth_enable, ps3tc_gcm_state_line_smooth_enable_unsafe, NV40TCL_LINE_SMOOTH_ENABLE, v);
            drive_uint("polygon_smooth_enable", ps3tc_gcm_state_polygon_smooth_enable, ps3tc_gcm_state_polygon_smooth_enable_unsafe, NV40TCL_POLYGON_SMOOTH_ENABLE, v);
            drive_uint("alpha_test_enable", ps3tc_gcm_state_alpha_test_enable, ps3tc_gcm_state_alpha_test_enable_unsafe, NV40TCL_ALPHA_TEST_ENABLE, v);
        }
    }

    /* alpha_func (2-word, 3 values) */
    printf("\n  Alpha func (3 words, header + 2 data)\n");
    {
        const struct { uint32_t f, r; } afs[] = {
            { 0x0000, 0x0000 }, { 0x0001, 0x80u }, { 0x0210, 0xFFu },
            { 0x0300, 0x1234u }, { 0x0700, 0xFFFFu },
        };
        for (size_t i = 0; i < sizeof afs / sizeof afs[0]; ++i) {
            uint32_t f = afs[i].f, r = afs[i].r;
            static uint32_t A[4], B[4];
            gcmContextData n, ctx;
            memset(&n,0,sizeof n); memset(&ctx,0,sizeof ctx);
            n.begin=n.current=A; n.end=A+4; n.callback=0;
            ctx.begin=ctx.current=B; ctx.end=B+4; ctx.callback=0;
            ps3tc_gcm_state_alpha_func((CellGcmContextData *)&n, f, r);
            ref_safe_pair3(&ctx, NV40TCL_ALPHA_TEST_FUNC, f, r);
            char lab[48]; snprintf(lab, sizeof lab, "  alpha_func %u", (unsigned)i);
            cmp(lab, A, B, 3);
            n.begin=n.current=A; ctx.begin=ctx.current=B;
            ps3tc_gcm_state_alpha_func_unsafe((CellGcmContextData *)&n, f, r);
            ref_unsafe_pair3(&ctx, NV40TCL_ALPHA_TEST_FUNC, f, r);
            cmp(lab, A, B, 3);
        }
    }

    /* polygon_offset (3 words, floats as bit patterns) */
    printf("\n  Polygon offset (3 words, float bit patterns)\n");
    {
        const uint32_t foats[] = {
            0x3F800000u, /* 1.0   */
            0x00000000u, /* 0.0   */
            0x7F800000u, /* +inf  */
            0x7FC00000u, /* NaN   */
            0x40490FDBu, /* ~ pi  */
            0x3DCCCCCDu, /* ~0.1  */
        };
        for (size_t i = 0; i < sizeof foats / sizeof foats[0]; ++i) {
            f32 fa, fb;
            memcpy(&fa, &foats[i], 4);
            memcpy(&fb, &foats[(i+1) % (sizeof foats / sizeof foats[0])], 4);
            static uint32_t A[4], B[4];
            gcmContextData n, rf;
            ieee32 a, b;
            a.f = fa; b.f = fb;
            memset(&n, 0, sizeof n); memset(&rf, 0, sizeof rf);
            n.begin=n.current=A; n.end=A+4; n.callback=0;
            rf.begin=rf.current=B; rf.end=B+4; rf.callback=0;
            ps3tc_gcm_state_polygon_offset((CellGcmContextData *)&n, fa, fb);
            ref_safe_pair3(&rf, NV40TCL_POLYGON_OFFSET_FACTOR, a.u, b.u);
            char lab[48]; snprintf(lab, sizeof lab, "  polygon_offset %u", (unsigned)i);
            cmp(lab, A, B, 3);
            n.begin=n.current=A; rf.begin=rf.current=B;
            ps3tc_gcm_state_polygon_offset_unsafe((CellGcmContextData *)&n, fa, fb);
            ref_unsafe_pair3(&rf, NV40TCL_POLYGON_OFFSET_FACTOR, a.u, b.u);
            cmp(lab, A, B, 3);
        }
    }

    /* ---- fog mode (2-word, raw value) ---- */
    printf("\n  Fog mode (2-word, raw value)\n");
    for (uint32_t v = 0; v <= 3; ++v)
        drive_uint("  fog_mode",
                   ps3tc_gcm_state_fog_mode,
                   ps3tc_gcm_state_fog_mode_unsafe,
                   NV40TCL_FOG_MODE, v);

    /* ---- fog params (3-word, two floats, IEEE-32 bit-pattern sweep) ---- */
    printf("\n  Fog params (3-word, float bit patterns)\n");
    {
        const struct { uint32_t p0, p1; } raws[] = {
            { 0x00000000u, 0x00000000u },   /* (0.0, 0.0) */
            { 0x3F800000u, 0x3F800000u },   /* (1.0, 1.0) */
            { 0x40490FDBu, 0x40490FDBu },   /* (~ pi, ~ pi) */
            { 0x80000000u, 0x00000000u },   /* (-0.0, 0.0) */
            { 0xDEADBEEFu, 0xDEADBEEFu },   /* (arbitrary, arbitrary) */
        };
        for (size_t i = 0; i < sizeof raws / sizeof raws[0]; ++i) {
            f32 a, b;
            memcpy(&a, &raws[i].p0, 4); memcpy(&b, &raws[i].p1, 4);
            static uint32_t A[8], B[8];
            gcmContextData n, r;
            memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
            n.begin = n.current = A; n.end = A + 8; n.callback = 0;
            r.begin = r.current = B; r.end = B + 8; r.callback = 0;
            ps3tc_gcm_state_fog_params((CellGcmContextData *)&n, a, b);
            ref_safe_fog_params(&r, a, b);
            char lab[48]; snprintf(lab, sizeof lab, "  fog_params %u", (unsigned)i);
            cmp(lab, A, B, 3);
            n.begin = n.current = A; r.begin = r.current = B;
            ps3tc_gcm_state_fog_params_unsafe((CellGcmContextData *)&n, a, b);
            ref_unsafe_fog_params(&r, a, b);
            snprintf(lab, sizeof lab, "  fog_params %u", (unsigned)i);
            cmp(lab, A, B, 3);
        }
    }

    /* ---- anti-aliasing control (2-word, bit-packed 4 values) ---- */
    printf("\n  Anti-aliasing control (2-word, packed 4 values)\n");
    {
        const struct { uint32_t en, a2c, a2o, sm; } aas[] = {
            { 0,   0,   0,   0x00   },
            { 1,   2,   3,   0xFF   },
            { 0xF, 0xF, 0xF, 0xFFFF },
            { 1,   0,   1,   0x100  },
        };
        for (size_t i = 0; i < sizeof aas / sizeof aas[0]; ++i) {
            uint32_t en = aas[i].en, a2c = aas[i].a2c, a2o = aas[i].a2o, sm = aas[i].sm;
            static uint32_t A[4], B[4];
            gcmContextData n, r;
            memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
            n.begin = n.current = A; n.end = A + 4; n.callback = 0;
            r.begin = r.current = B; r.end = B + 4; r.callback = 0;
            ps3tc_gcm_state_anti_aliasing_control((CellGcmContextData *)&n, en, a2c, a2o, sm);
            ref_safe_aa(&r, en, a2c, a2o, sm);
            char lab[48]; snprintf(lab, sizeof lab, "  anti_aliasing %u", (unsigned)i);
            cmp(lab, A, B, 2);
            n.begin = n.current = A; r.begin = r.current = B;
            ps3tc_gcm_state_anti_aliasing_control_unsafe((CellGcmContextData *)&n, en, a2c, a2o, sm);
            ref_unsafe_aa(&r, en, a2c, a2o, sm);
            snprintf(lab, sizeof lab, "  anti_aliasing %u", (unsigned)i);
            cmp(lab, A, B, 2);
        }
    }

    /* ---- user clip plane control (2-word, 6 planes x 4 bits) ---- */
    printf("\n  User clip plane (2-word, 6 planes x 4 bits)\n");
    {
        const struct { uint32_t p0,p1,p2,p3,p4,p5; } cps[] = {
            { 0,   0,   0,   0,   0,   0   },
            { 1,   0,   1,   0,   1,   0   },
            { 1,   1,   1,   1,   1,   1   },
            { 0xF, 0x0, 0xF, 0x0, 0xF, 0xF },
        };
        for (size_t i = 0; i < sizeof cps / sizeof cps[0]; ++i) {
            uint32_t p0=cps[i].p0,p1=cps[i].p1,p2=cps[i].p2,p3=cps[i].p3,p4=cps[i].p4,p5=cps[i].p5;
            static uint32_t A[4], B[4];
            gcmContextData n, r;
            memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
            n.begin = n.current = A; n.end = A + 4; n.callback = 0;
            r.begin = r.current = B; r.end = B + 4; r.callback = 0;
            ps3tc_gcm_state_user_clip_plane_control((CellGcmContextData *)&n, p0,p1,p2,p3,p4,p5);
            ref_safe_clip(&r, p0,p1,p2,p3,p4,p5);
            char lab[48]; snprintf(lab, sizeof lab, "  user_clip %u", (unsigned)i);
            cmp(lab, A, B, 2);
            n.begin = n.current = A; r.begin = r.current = B;
            ps3tc_gcm_state_user_clip_plane_control_unsafe((CellGcmContextData *)&n, p0,p1,p2,p3,p4,p5);
            ref_unsafe_clip(&r, p0,p1,p2,p3,p4,p5);
            snprintf(lab, sizeof lab, "  user_clip %u", (unsigned)i);
            cmp(lab, A, B, 2);
        }
    }

    /* ---- fragment program gamma enable (2-word, raw value) ---- */
    printf("\n  FP gamma enable (2-word, raw value)\n");
    for (uint32_t v = 0; v <= 2; ++v)
        drive_uint("  fp_gamma_enable",
                   ps3tc_gcm_state_fragment_program_gamma_enable,
                   ps3tc_gcm_state_fragment_program_gamma_enable_unsafe,
                   NV40TCL_FP_PACKER, v);

    /* ---- zminmax control (2-word, bit-packed 3 values) ---- */
    printf("\n  Z-minmax control (2-word, packed 3 values)\n");
    {
        const struct { uint32_t cnf, zc, ciw; } zms[] = {
            { 0,    0,    0    },
            { 1,    1,    1    },
            { 0x0F, 0x0F, 0x0F },
            { 1,    0,    1    },
        };
        for (size_t i = 0; i < sizeof zms / sizeof zms[0]; ++i) {
            uint32_t cnf = zms[i].cnf, zc = zms[i].zc, ciw = zms[i].ciw;
            static uint32_t A[4], B[4];
            gcmContextData n, r;
            memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
            n.begin = n.current = A; n.end = A + 4; n.callback = 0;
            r.begin = r.current = B; r.end = B + 4; r.callback = 0;
            ps3tc_gcm_state_zminmax_control((CellGcmContextData *)&n, cnf, zc, ciw);
            ref_safe_zminmax(&r, cnf, zc, ciw);
            char lab[48]; snprintf(lab, sizeof lab, "  zminmax %u", (unsigned)i);
            cmp(lab, A, B, 2);
            n.begin = n.current = A; r.begin = r.current = B;
            ps3tc_gcm_state_zminmax_control_unsafe((CellGcmContextData *)&n, cnf, zc, ciw);
            ref_unsafe_zminmax(&r, cnf, zc, ciw);
            snprintf(lab, sizeof lab, "  zminmax %u", (unsigned)i);
            cmp(lab, A, B, 2);
        }
    }

    /* ---- render_enable: TWO shapes (unconditional 2-word, conditional 4-word) ---- */
    printf("  render_enable (unconditional, 2-word)\n");
    for (uint32_t index = 0; index <= 3; ++index) {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_render_enable((CellGcmContextData *)&n, 0, index);
        ref_safe_render(&r, 0, index);
        char lab[48]; snprintf(lab, sizeof lab, "  render_enable (uncond) idx %u", (unsigned)index);
        cmp(lab, A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_render_enable_unsafe((CellGcmContextData *)&n, 0, index);
        ref_unsafe_render(&r, 0, index);
        cmp(lab, A, B, 2);
    }
    printf("  render_enable (conditional, 4-word)\n");
    for (uint32_t index = 0; index <= 3; ++index) {
        static uint32_t A[8], B[8];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 8; n.callback = 0;
        r.begin = r.current = B; r.end = B + 8; r.callback = 0;
        ps3tc_gcm_state_render_enable((CellGcmContextData *)&n, 2, index);
        ref_safe_render(&r, 2, index);
        char lab[48]; snprintf(lab, sizeof lab, "  render_enable (cond) idx %u", (unsigned)index);
        cmp(lab, A, B, 4);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_render_enable_unsafe((CellGcmContextData *)&n, 2, index);
        ref_unsafe_render(&r, 2, index);
        cmp(lab, A, B, 4);
    }

    /* ---- frequency divider (2-word, u16 value) ---- */
    printf("\n  Frequency divider (2-word, u16 value)\n");
    for (uint32_t v = 0; v <= 0xFFFF; v += 0x5555) {
        uint16_t op = (uint16_t)v;
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_frequency_divider_operation((CellGcmContextData *)&n, op);
        ref_safe1(&r, NV40TCL_DIVIDER_FREQUENCY_OP, op);
        char lab[48]; snprintf(lab, sizeof lab, "  freq_divider 0x%04x", (unsigned)op);
        cmp(lab, A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_frequency_divider_operation_unsafe((CellGcmContextData *)&n, op);
        ref_unsafe1(&r, NV40TCL_DIVIDER_FREQUENCY_OP, op);
        cmp(lab, A, B, 2);
    }

    /* ---- viewport / scissor / clear / zcull / scull group ---- */
    printf("\n  Viewport / scissor / clear / zcull / scull\n");

    /* viewport (24 words, 3 methods) */
    {
        const f32 vp_scale[4]  = { 1.5f, 2.5f, 3.5f, 4.5f };
        const f32 vp_offset[4] = { -1.0f, -2.0f, -3.0f, -4.0f };
        static uint32_t A[32], B[32];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 32; n.callback = 0;
        r.begin = r.current = B; r.end = B + 32; r.callback = 0;
        ps3tc_gcm_state_viewport((CellGcmContextData *)&n, 8, 12, 256, 144, 0.5f, 8.25f, vp_scale, vp_offset);
        ref_safe_viewport(&r, 8, 12, 256, 144, 0.5f, 8.25f, vp_scale, vp_offset);
        cmp("  viewport (safe)", A, B, 24);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_viewport_unsafe((CellGcmContextData *)&n, 8, 12, 256, 144, 0.5f, 8.25f, vp_scale, vp_offset);
        ref_unsafe_viewport(&r, 8, 12, 256, 144, 0.5f, 8.25f, vp_scale, vp_offset);
        cmp("  viewport (unsafe)", A, B, 24);
    }

    /* scissor (3 words) */
    {
        static uint32_t A[8], B[8];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 8; n.callback = 0;
        r.begin = r.current = B; r.end = B + 8; r.callback = 0;
        ps3tc_gcm_state_scissor((CellGcmContextData *)&n, 32, 48, 320, 240);
        ref_safe_scissor(&r, 32, 48, 320, 240);
        cmp("  scissor (safe)", A, B, 3);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_scissor_unsafe((CellGcmContextData *)&n, 32, 48, 320, 240);
        ref_unsafe_scissor(&r, 32, 48, 320, 240);
        cmp("  scissor (unsafe)", A, B, 3);
    }

    /* clear_color (2 words, single u32) */
    drive_uint("  clear_color",
               ps3tc_gcm_state_clear_color,
               ps3tc_gcm_state_clear_color_unsafe,
               NV40TCL_CLEAR_VALUE_COLOR, 0xFF00FF00u);

    /* clear_depth_stencil (2 words, single u32) */
    drive_uint("  clear_depth_stencil",
               ps3tc_gcm_state_clear_depth_stencil,
               ps3tc_gcm_state_clear_depth_stencil_unsafe,
               NV40TCL_CLEAR_VALUE_DEPTH, 0x3F800000u);

    /* zcull_control (2 words, (fmt<<4)|dir) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_zcull_control((CellGcmContextData *)&n, 3, 2);
        ref_safe_zcull_ctrl(&r, 3, 2);
        cmp("  zcull_control (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_zcull_control_unsafe((CellGcmContextData *)&n, 3, 2);
        ref_unsafe_zcull_ctrl(&r, 3, 2);
        cmp("  zcull_control (unsafe)", A, B, 2);
    }

    /* zcull_enable (2 words, (stencil<<1)|depth) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_zcull_enable((CellGcmContextData *)&n, 1, 0);
        ref_safe_zcull_en(&r, 1, 0);
        cmp("  zcull_enable (safe 1,0)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_zcull_enable_unsafe((CellGcmContextData *)&n, 1, 0);
        ref_unsafe_zcull_en(&r, 1, 0);
        cmp("  zcull_enable (unsafe 1,0)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_zcull_enable((CellGcmContextData *)&n, 0, 1);
        ref_safe_zcull_en(&r, 0, 1);
        cmp("  zcull_enable (safe 0,1)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_zcull_enable_unsafe((CellGcmContextData *)&n, 0, 1);
        ref_unsafe_zcull_en(&r, 0, 1);
        cmp("  zcull_enable (unsafe 0,1)", A, B, 2);
    }

    /* zcull_limit (2 words, (mfl<<16)|pb) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_zcull_limit((CellGcmContextData *)&n, 0x1111, 0x2222);
        ref_safe_zcull_lim(&r, 0x1111, 0x2222);
        cmp("  zcull_limit (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_zcull_limit_unsafe((CellGcmContextData *)&n, 0x1111, 0x2222);
        ref_unsafe_zcull_lim(&r, 0x1111, 0x2222);
        cmp("  zcull_limit (unsafe)", A, B, 2);
    }

    /* zcull_stats_enable (2 words, single u32) */
    drive_uint("  zcull_stats_enable",
               ps3tc_gcm_state_zcull_stats_enable,
               ps3tc_gcm_state_zcull_stats_enable_unsafe,
               NV40TCL_ZCULL_STATS_ENABLE, 1);

    /* scull_control (2 words, (mask<<24)|(ref<<16)|func) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_scull_control((CellGcmContextData *)&n, 0x21, 0x80, 0x03);
        ref_safe_scull(&r, 0x21, 0x80, 0x03);
        cmp("  scull_control (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_scull_control_unsafe((CellGcmContextData *)&n, 0x21, 0x80, 0x03);
        ref_unsafe_scull(&r, 0x21, 0x80, 0x03);
        cmp("  scull_control (unsafe)", A, B, 2);
    }

        /* invalidate_zcull (2 words, value 0; takes no value arg) */
        {
            static uint32_t A[4], B[4];
            gcmContextData n, r;
            memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
            n.begin = n.current = A; n.end = A + 4; n.callback = 0;
            r.begin = r.current = B; r.end = B + 4; r.callback = 0;
            ps3tc_gcm_state_invalidate_zcull((CellGcmContextData *)&n);
            ref_safe_invalz(&r);
            cmp("  invalidate_zcull (safe)", A, B, 2);
            n.begin = n.current = A; r.begin = r.current = B;
            ps3tc_gcm_state_invalidate_zcull_unsafe((CellGcmContextData *)&n);
            ref_unsafe_invalz(&r);
            cmp("  invalidate_zcull (unsafe)", A, B, 2);
        }

    /* ---- texture-border / attrib-mask / zpass / clear-zcull / ref-cmd / caches / vertex-4f / report ---- */
    printf("\n  Texture-border / attrib-mask / zpass / clear-zcull / ref / caches / vertex-4f / report\n");

    /* texture_border_color (2 words, index + color) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_texture_border_color((CellGcmContextData *)&n, 0x05, 0xA5D1B2C3u);
        ref_safe_texborder(&r, 0x05, 0xA5D1B2C3u);
        cmp("  texture_border_color (safe, idx 0x05)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_texture_border_color_unsafe((CellGcmContextData *)&n, 0x05, 0xA5D1B2C3u);
        ref_unsafe_texborder(&r, 0x05, 0xA5D1B2C3u);
        cmp("  texture_border_color (unsafe, idx 0x05)", A, B, 2);
    }
    /* texture_border_color — index=0x2A (different) to catch idx/color swap */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_texture_border_color((CellGcmContextData *)&n, 0x2A, 0x00C0FFB0u);
        ref_safe_texborder(&r, 0x2A, 0x00C0FFB0u);
        cmp("  texture_border_color (safe, idx 0x2A)", A, B, 2);
    }

    /* vertex_attrib_output_mask (2 words) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_vertex_attrib_output_mask((CellGcmContextData *)&n, 0x0C3A7751u);
        ref_safe_vattrmask(&r, 0x0C3A7751u);
        cmp("  vertex_attrib_output_mask (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_attrib_output_mask_unsafe((CellGcmContextData *)&n, 0x0C3A7751u);
        ref_unsafe_vattrmask(&r, 0x0C3A7751u);
        cmp("  vertex_attrib_output_mask (unsafe)", A, B, 2);
    }

    /* zpass_pixel_count_enable (2 words) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_zpass_pixel_count_enable((CellGcmContextData *)&n, 0x64B2E910u);
        ref_safe_zpass(&r, 0x64B2E910u);
        cmp("  zpass_pixel_count_enable (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_zpass_pixel_count_enable_unsafe((CellGcmContextData *)&n, 0x64B2E910u);
        ref_unsafe_zpass(&r, 0x64B2E910u);
        cmp("  zpass_pixel_count_enable (unsafe)", A, B, 2);
    }

    /* clear_zcull_surface (2 words, (stencil<<1)|depth) — two distinct values */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_clear_zcull_surface((CellGcmContextData *)&n, 0x5A, 0x27);
        ref_safe_clearzcull(&r, 0x5A, 0x27);
        cmp("  clear_zcull_surface (safe, 0x5A/0x27)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_clear_zcull_surface_unsafe((CellGcmContextData *)&n, 0x5A, 0x27);
        ref_unsafe_clearzcull(&r, 0x5A, 0x27);
        cmp("  clear_zcull_surface (unsafe, 0x5A/0x27)", A, B, 2);
    }
    /* clear_zcull_surface — swappable values to catch a depth/stencil mix-up */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_clear_zcull_surface((CellGcmContextData *)&n, 0x81, 0x08);
        ref_safe_clearzcull(&r, 0x81, 0x08);
        cmp("  clear_zcull_surface (safe, 0x81/0x08)", A, B, 2);
    }

    /* reference_command (2 words) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_reference_command((CellGcmContextData *)&n, 0x1B8F42A9u);
        ref_safe_refcmd(&r, 0x1B8F42A9u);
        cmp("  reference_command (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_reference_command_unsafe((CellGcmContextData *)&n, 0x1B8F42A9u);
        ref_unsafe_refcmd(&r, 0x1B8F42A9u);
        cmp("  reference_command (unsafe)", A, B, 2);
    }

    /* invalidate_vertex_cache (8 words) */
    {
        static uint32_t A[12], B[12];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 12; n.callback = 0;
        r.begin = r.current = B; r.end = B + 12; r.callback = 0;
        ps3tc_gcm_state_invalidate_vertex_cache((CellGcmContextData *)&n);
        ref_safe_invvert(&r);
        cmp("  invalidate_vertex_cache (safe)", A, B, 8);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_invalidate_vertex_cache_unsafe((CellGcmContextData *)&n);
        ref_unsafe_invvert(&r);
        cmp("  invalidate_vertex_cache (unsafe)", A, B, 8);
    }

    /* invalidate_texture_cache (2 words) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_invalidate_texture_cache((CellGcmContextData *)&n, 0x43CD8E02u);
        ref_safe_invtex(&r, 0x43CD8E02u);
        cmp("  invalidate_texture_cache (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_invalidate_texture_cache_unsafe((CellGcmContextData *)&n, 0x43CD8E02u);
        ref_unsafe_invtex(&r, 0x43CD8E02u);
        cmp("  invalidate_texture_cache (unsafe)", A, B, 2);
    }

    /* vertex_data_4f (5 words, v[4] IEEE-32) */
    {
        const f32 v[4] = { 0.25f, -1.5f, 3.0f, 1000.0f };
        static uint32_t A[8], B[8];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 8; n.callback = 0;
        r.begin = r.current = B; r.end = B + 8; r.callback = 0;
        ps3tc_gcm_state_vertex_data_4f((CellGcmContextData *)&n, 0x2A, v);
        ref_safe_vtx4f(&r, 0x2A, v);
        cmp("  vertex_data_4f (safe, idx 0x2A)", A, B, 5);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_data_4f_unsafe((CellGcmContextData *)&n, 0x2A, v);
        ref_unsafe_vtx4f(&r, 0x2A, v);
        cmp("  vertex_data_4f (unsafe, idx 0x2A)", A, B, 5);
    }
    /* vertex_data_4f — index=0x07 with a different value vector */
    {
        const f32 v[4] = { -2.5f, 0.125f, 42.0f, -0.03125f };
        static uint32_t A[8], B[8];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 8; n.callback = 0;
        r.begin = r.current = B; r.end = B + 8; r.callback = 0;
        ps3tc_gcm_state_vertex_data_4f((CellGcmContextData *)&n, 0x07, v);
        ref_safe_vtx4f(&r, 0x07, v);
        cmp("  vertex_data_4f (safe, idx 0x07)", A, B, 5);
    }

    /* clear_report (2 words) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_clear_report((CellGcmContextData *)&n, 0x88C42A3Du);
        ref_safe_clearrep(&r, 0x88C42A3Du);
        cmp("  clear_report (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_clear_report_unsafe((CellGcmContextData *)&n, 0x88C42A3Du);
        ref_unsafe_clearrep(&r, 0x88C42A3Du);
        cmp("  clear_report (unsafe)", A, B, 2);
    }

    /* ---- surface / vertex-data-array / VP constants / update FP / nop / report location ---- */
    /* surface (32 words) */
    {
        static uint32_t A[64], B[64];
        const CellGcmSurface surf = {
            .type = 0x01,
            .antiAlias = 0x02,
            .colorFormat = 0x03,
            .colorTarget = 0x04,
            .colorLocation = { 0x00, 0x01, 0x02, 0x03 },
            .colorOffset = { 0x1000, 0x2000, 0x3000, 0x4000 },
            .colorPitch = { 0x0100, 0x0200, 0x0300, 0x0400 },
            .depthFormat = 0x05,
            .depthLocation = 0x01,
            ._pad = { 0, 0 },
            .depthOffset = 0x5000,
            .depthPitch = 0x0500,
            .width = 1024,
            .height = 512,
            .x = 16,
            .y = 32
        };
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 64; n.callback = 0;
        r.begin = r.current = B; r.end = B + 64; r.callback = 0;
        ps3tc_gcm_state_surface((CellGcmContextData *)&n, &surf);
        ref_safe_surface(&r, &surf);
        cmp("  surface (safe)", A, B, 32);

        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_surface_unsafe((CellGcmContextData *)&n, &surf);
        ref_unsafe_surface(&r, &surf);
        cmp("  surface (unsafe)", A, B, 32);

        /* Swap mutant: surface with x and y swapped must go red */
        CellGcmSurface surf_mut = surf;
        surf_mut.x = surf.y;
        surf_mut.y = surf.x;
        r.begin = r.current = B;
        ref_safe_surface(&r, &surf_mut);
        cmp_mutant("  surface (swap mutant x/y)", A, B, 32);
    }

    /* vertex_data_array (4 words) */
    {
        static uint32_t A[8], B[8];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 8; n.callback = 0;
        r.begin = r.current = B; r.end = B + 8; r.callback = 0;
        ps3tc_gcm_state_vertex_data_array((CellGcmContextData *)&n,
                                          0x07, 0x1234, 0x20, 0x04, 0x02, 0x01, 0x00567890);
        ref_safe_vtxarray(&r, 0x07, 0x1234, 0x20, 0x04, 0x02, 0x01, 0x00567890);
        cmp("  vertex_data_array (safe)", A, B, 4);

        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_data_array_unsafe((CellGcmContextData *)&n,
                                                 0x07, 0x1234, 0x20, 0x04, 0x02, 0x01, 0x00567890);
        ref_unsafe_vtxarray(&r, 0x07, 0x1234, 0x20, 0x04, 0x02, 0x01, 0x00567890);
        cmp("  vertex_data_array (unsafe)", A, B, 4);

        /* Case 2 with different arguments */
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_data_array((CellGcmContextData *)&n,
                                          0x03, 0x0001, 0x10, 0x02, 0x01, 0x00, 0x00ABCDEF);
        ref_safe_vtxarray(&r, 0x03, 0x0001, 0x10, 0x02, 0x01, 0x00, 0x00ABCDEF);
        cmp("  vertex_data_array (case 2)", A, B, 4);

        /* Swap mutant: stride and size swapped must go red */
        r.begin = r.current = B;
        ref_safe_vtxarray(&r, 0x07, 0x1234, 0x04, 0x20, 0x02, 0x01, 0x00567890);
        cmp_mutant("  vertex_data_array (swap mutant)", A, B, 4);
    }

    /* vertex_program_constants (lengths 1, 4, 32, and 36 crossing the 32-float split) */
    {
        static uint32_t A[64], B[64];
        gcmContextData n, r;

        /* len = 1 (3 words) */
        const f32 c1[1] = { 1.25f };
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 64; n.callback = 0;
        r.begin = r.current = B; r.end = B + 64; r.callback = 0;
        ps3tc_gcm_state_vertex_program_constants((CellGcmContextData *)&n, 0x10, 1, c1);
        ref_safe_vp_consts(&r, 0x10, 1, c1);
        cmp("  vertex_program_constants (len 1, safe)", A, B, 3);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_program_constants_unsafe((CellGcmContextData *)&n, 0x10, 1, c1);
        ref_unsafe_vp_consts(&r, 0x10, 1, c1);
        cmp("  vertex_program_constants (len 1, unsafe)", A, B, 3);

        /* len = 4 (6 words) */
        const f32 c4[4] = { 1.5f, -2.5f, 3.25f, -0.125f };
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_program_constants((CellGcmContextData *)&n, 0x20, 4, c4);
        ref_safe_vp_consts(&r, 0x20, 4, c4);
        cmp("  vertex_program_constants (len 4, safe)", A, B, 6);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_program_constants_unsafe((CellGcmContextData *)&n, 0x20, 4, c4);
        ref_unsafe_vp_consts(&r, 0x20, 4, c4);
        cmp("  vertex_program_constants (len 4, unsafe)", A, B, 6);

        /* Swap mutant: c4[0] and c4[1] swapped must go red */
        const f32 c4_mut[4] = { -2.5f, 1.5f, 3.25f, -0.125f };
        r.begin = r.current = B;
        ref_safe_vp_consts(&r, 0x20, 4, c4_mut);
        cmp_mutant("  vertex_program_constants (swap mutant)", A, B, 6);

        /* len = 32 (34 words) */
        f32 c32[32];
        for (int i = 0; i < 32; i++) c32[i] = (float)(i * 2.5f - 10.0f);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_program_constants((CellGcmContextData *)&n, 0x30, 32, c32);
        ref_safe_vp_consts(&r, 0x30, 32, c32);
        cmp("  vertex_program_constants (len 32, safe)", A, B, 34);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_program_constants_unsafe((CellGcmContextData *)&n, 0x30, 32, c32);
        ref_unsafe_vp_consts(&r, 0x30, 32, c32);
        cmp("  vertex_program_constants (len 32, unsafe)", A, B, 34);

        /* len = 36 (crosses method-count split, 40 words) */
        f32 c36[36];
        for (int i = 0; i < 36; i++) c36[i] = (float)(i * 1.5f + 0.25f);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_program_constants((CellGcmContextData *)&n, 0x40, 36, c36);
        ref_safe_vp_consts(&r, 0x40, 36, c36);
        cmp("  vertex_program_constants (len 36, safe)", A, B, 40);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_vertex_program_constants_unsafe((CellGcmContextData *)&n, 0x40, 36, c36);
        ref_unsafe_vp_consts(&r, 0x40, 36, c36);
        cmp("  vertex_program_constants (len 36, unsafe)", A, B, 40);
    }

    /* update_fragment_program_parameter (2 words) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_update_fragment_program_parameter((CellGcmContextData *)&n, 0x01ABCDEFu);
        ref_safe_upd_fp(&r, 0x01ABCDEFu);
        cmp("  update_fp_parameter (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_update_fragment_program_parameter_unsafe((CellGcmContextData *)&n, 0x01ABCDEFu);
        ref_unsafe_upd_fp(&r, 0x01ABCDEFu);
        cmp("  update_fp_parameter (unsafe)", A, B, 2);

        /* Case 2 */
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_update_fragment_program_parameter((CellGcmContextData *)&n, 0x12345678u);
        ref_safe_upd_fp(&r, 0x12345678u);
        cmp("  update_fp_parameter (case 2)", A, B, 2);
    }

    /* nop_command (0, 1 and large count) */
    {
        static uint32_t A[32], B[32];
        gcmContextData n, r;

        /* count = 0 (0 words) */
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 32; n.callback = 0;
        r.begin = r.current = B; r.end = B + 32; r.callback = 0;
        ps3tc_gcm_state_nop_command((CellGcmContextData *)&n, 0);
        ref_safe_nop(&r, 0);
        cmp("  nop_command (count 0, safe)", A, B, 0);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_nop_command_unsafe((CellGcmContextData *)&n, 0);
        ref_unsafe_nop(&r, 0);
        cmp("  nop_command (count 0, unsafe)", A, B, 0);

        /* count = 1 (1 word) */
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_nop_command((CellGcmContextData *)&n, 1);
        ref_safe_nop(&r, 1);
        cmp("  nop_command (count 1, safe)", A, B, 1);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_nop_command_unsafe((CellGcmContextData *)&n, 1);
        ref_unsafe_nop(&r, 1);
        cmp("  nop_command (count 1, unsafe)", A, B, 1);

        /* count = 16 (large count, 16 words) */
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_nop_command((CellGcmContextData *)&n, 16);
        ref_safe_nop(&r, 16);
        cmp("  nop_command (count 16, safe)", A, B, 16);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_nop_command_unsafe((CellGcmContextData *)&n, 16);
        ref_unsafe_nop(&r, 16);
        cmp("  nop_command (count 16, unsafe)", A, B, 16);
    }

    /* report_location (2 words, cell and rsx location) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_report_location((CellGcmContextData *)&n, GCM_LOCATION_CELL);
        ref_safe_report_loc(&r, GCM_LOCATION_CELL);
        cmp("  report_location (CELL, safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_report_location_unsafe((CellGcmContextData *)&n, GCM_LOCATION_CELL);
        ref_unsafe_report_loc(&r, GCM_LOCATION_CELL);
        cmp("  report_location (CELL, unsafe)", A, B, 2);

        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_report_location((CellGcmContextData *)&n, GCM_LOCATION_RSX);
        ref_safe_report_loc(&r, GCM_LOCATION_RSX);
        cmp("  report_location (RSX, safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_report_location_unsafe((CellGcmContextData *)&n, GCM_LOCATION_RSX);
        ref_unsafe_report_loc(&r, GCM_LOCATION_RSX);
        cmp("  report_location (RSX, unsafe)", A, B, 2);
    }

    /* report (type and index) */
    {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_report((CellGcmContextData *)&n, 0x5A, 0x00001234u);
        ref_safe_report(&r, 0x5A, 0x00001234u);
        cmp("  report (safe)", A, B, 2);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_report_unsafe((CellGcmContextData *)&n, 0x5A, 0x00001234u);
        ref_unsafe_report(&r, 0x5A, 0x00001234u);
        cmp("  report (unsafe)", A, B, 2);

        /* Swap mutant: type and index swapped must go red */
        r.begin = r.current = B;
        ref_safe_report(&r, 0x34, 0x0000005Au);
        cmp_mutant("  report (swap mutant)", A, B, 2);
    }

    /* transfer_data_mode (3 words, sweep modes 0..3) */
    printf("\n  Transfer data mode (3-word, modes 0..3)\n");
    for (uint8_t m = 0; m <= 3; ++m) {
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_transfer_data_mode((CellGcmContextData *)&n, m);
        ref_safe_transfer_data_mode(&r, m);
        char lab[48]; snprintf(lab, sizeof lab, "  transfer_data_mode m=%u (safe)", m);
        cmp(lab, A, B, 3);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_data_mode_unsafe((CellGcmContextData *)&n, m);
        ref_unsafe_transfer_data_mode(&r, m);
        snprintf(lab, sizeof lab, "  transfer_data_mode m=%u (unsafe)", m);
        cmp(lab, A, B, 3);
    }
    {
        /* Mode mutant: compare mode 1 against mode 2 */
        static uint32_t A[4], B[4];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 4; n.callback = 0;
        r.begin = r.current = B; r.end = B + 4; r.callback = 0;
        ps3tc_gcm_state_transfer_data_mode((CellGcmContextData *)&n, 1);
        ref_safe_transfer_data_mode(&r, 2);
        cmp_mutant("  transfer_data_mode (mode mutant)", A, B, 3);
    }

    /* transfer_data_offset (8 words) */
    printf("\n  Transfer data offset (8-word)\n");
    {
        static uint32_t A[16], B[16];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 16; n.callback = 0;
        r.begin = r.current = B; r.end = B + 16; r.callback = 0;
        ps3tc_gcm_state_transfer_data_offset((CellGcmContextData *)&n, 0x12345678, 0x87654321);
        ref_safe_transfer_data_offset(&r, 0x12345678, 0x87654321);
        cmp("  transfer_data_offset (safe)", A, B, 8);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_data_offset_unsafe((CellGcmContextData *)&n, 0x12345678, 0x87654321);
        ref_unsafe_transfer_data_offset(&r, 0x12345678, 0x87654321);
        cmp("  transfer_data_offset (unsafe)", A, B, 8);

        /* Swap mutant: dst and src swapped */
        r.begin = r.current = B;
        ref_safe_transfer_data_offset(&r, 0x87654321, 0x12345678);
        cmp_mutant("  transfer_data_offset (swap mutant)", A, B, 8);
    }

    /* transfer_data_format (6 words) */
    printf("\n  Transfer data format (6-word)\n");
    {
        static uint32_t A[16], B[16];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 16; n.callback = 0;
        r.begin = r.current = B; r.end = B + 16; r.callback = 0;
        ps3tc_gcm_state_transfer_data_format((CellGcmContextData *)&n, 1000, 2000, 100, 50, 2, 4);
        ref_safe_transfer_data_format(&r, 1000, 2000, 100, 50, 2, 4);
        cmp("  transfer_data_format (safe)", A, B, 6);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_data_format_unsafe((CellGcmContextData *)&n, 1000, 2000, 100, 50, 2, 4);
        ref_unsafe_transfer_data_format(&r, 1000, 2000, 100, 50, 2, 4);
        cmp("  transfer_data_format (unsafe)", A, B, 6);

        /* Swap mutant: inpitch and outpitch swapped */
        r.begin = r.current = B;
        ref_safe_transfer_data_format(&r, 2000, 1000, 100, 50, 2, 4);
        cmp_mutant("  transfer_data_format (swap mutant)", A, B, 6);
    }

    /* transfer_scale_mode (6 words) */
    printf("\n  Transfer scale mode (6-word)\n");
    for (uint8_t surf = 0; surf <= 1; ++surf) {
        for (uint8_t m = 0; m <= 3; ++m) {
            static uint32_t A[8], B[8];
            gcmContextData n, r;
            memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
            n.begin = n.current = A; n.end = A + 8; n.callback = 0;
            r.begin = r.current = B; r.end = B + 8; r.callback = 0;
            ps3tc_gcm_state_transfer_scale_mode((CellGcmContextData *)&n, m, surf);
            ref_safe_transfer_scale_mode(&r, m, surf);
            char lab[64]; snprintf(lab, sizeof lab, "  transfer_scale_mode m=%u s=%u (safe)", m, surf);
            cmp(lab, A, B, 6);
            n.begin = n.current = A; r.begin = r.current = B;
            ps3tc_gcm_state_transfer_scale_mode_unsafe((CellGcmContextData *)&n, m, surf);
            ref_unsafe_transfer_scale_mode(&r, m, surf);
            snprintf(lab, sizeof lab, "  transfer_scale_mode m=%u s=%u (unsafe)", m, surf);
            cmp(lab, A, B, 6);
        }
    }
    {
        /* Mode mutant: surf 0 vs 1 */
        static uint32_t A[8], B[8];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 8; n.callback = 0;
        r.begin = r.current = B; r.end = B + 8; r.callback = 0;
        ps3tc_gcm_state_transfer_scale_mode((CellGcmContextData *)&n, 1, 0);
        ref_safe_transfer_scale_mode(&r, 1, 1);
        cmp_mutant("  transfer_scale_mode (surf mutant)", A, B, 6);
    }

    /* transfer_scale_surface (20 words) */
    printf("\n  Transfer scale surface (20-word)\n");
    {
        const gcmTransferScale scale = {
            .conversion = 1,
            .format = 2,
            .operation = 3,
            .clipX = 10, .clipY = 20, .clipW = 100, .clipH = 200,
            .outX = 30, .outY = 40, .outW = 300, .outH = 400,
            .ratioX = 0x12345, .ratioY = 0x54321,
            .inW = 50, .inH = 60, .pitch = 512,
            .origin = 1, .interp = 2,
            .offset = 0x12345000,
            .inX = 5, .inY = 15
        };
        const gcmTransferSurface surf = {
            .format = 4,
            .pitch = 1024,
            ._pad0 = {0, 0},
            .offset = 0x56780000
        };
        static uint32_t A[32], B[32];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 32; n.callback = 0;
        r.begin = r.current = B; r.end = B + 32; r.callback = 0;
        ps3tc_gcm_state_transfer_scale_surface((CellGcmContextData *)&n, &scale, &surf);
        ref_safe_transfer_scale_surface(&r, &scale, &surf);
        cmp("  transfer_scale_surface (safe)", A, B, 20);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_scale_surface_unsafe((CellGcmContextData *)&n, &scale, &surf);
        ref_unsafe_transfer_scale_surface(&r, &scale, &surf);
        cmp("  transfer_scale_surface (unsafe)", A, B, 20);

        /* Swap mutant: clipX and clipY swapped */
        gcmTransferScale scale_mut = scale;
        scale_mut.clipX = 20; scale_mut.clipY = 10;
        r.begin = r.current = B;
        ref_safe_transfer_scale_surface(&r, &scale_mut, &surf);
        cmp_mutant("  transfer_scale_surface (clip swap mutant)", A, B, 20);

        /* Negative coordinates: verify 16-bit clip/out packing avoids sign-extension smearing.
         * Checked against independently hand-computed expected words (not a duplicate cast):
         * Case 1: clipX=-1, clipY=2 -> (uint16_t)-1 = 0xffff, (uint16_t)2 = 0x0002 -> word 9 = 0x0002ffff
         *         outX=-3,  outY=4  -> (uint16_t)-3 = 0xfffd, (uint16_t)4 = 0x0004 -> word 11 = 0x0004fffd
         *         (The buggy unmasked code sign-extended into Y, writing 0xffffffff and 0xfffffffd)
         * Case 2: clipX=-100, clipY=-200 -> (uint16_t)-100 = 0xff9c, (uint16_t)-200 = 0xff38 -> word 9 = 0xff38ff9c
         *         outX=500,   outY=-50   -> (uint16_t)500  = 0x01f4, (uint16_t)-50   = 0xffce -> word 11 = 0xffce01f4
         */
        {
            gcmTransferScale neg1 = scale;
            neg1.clipX = -1; neg1.clipY = 2;
            neg1.outX = -3; neg1.outY = 4;
            n.begin = n.current = A;
            ps3tc_gcm_state_transfer_scale_surface((CellGcmContextData *)&n, &neg1, &surf);
            if (A[9] != 0x0002ffffu || A[11] != 0x0004fffdu) {
                printf("FAIL: transfer_scale_surface negative case 1 (safe): clip=0x%08x (exp 0x0002ffff), out=0x%08x (exp 0x0004fffd)\n",
                       A[9], A[11]);
                failures++;
            } else {
                printf("  PASS: transfer_scale_surface negative case 1 (safe: 0x0002ffff, 0x0004fffd)\n");
            }

            n.begin = n.current = A;
            ps3tc_gcm_state_transfer_scale_surface_unsafe((CellGcmContextData *)&n, &neg1, &surf);
            if (A[9] != 0x0002ffffu || A[11] != 0x0004fffdu) {
                printf("FAIL: transfer_scale_surface negative case 1 (unsafe): clip=0x%08x (exp 0x0002ffff), out=0x%08x (exp 0x0004fffd)\n",
                       A[9], A[11]);
                failures++;
            } else {
                printf("  PASS: transfer_scale_surface negative case 1 (unsafe: 0x0002ffff, 0x0004fffd)\n");
            }

            gcmTransferScale neg2 = scale;
            neg2.clipX = -100; neg2.clipY = -200;
            neg2.outX = 500; neg2.outY = -50;
            n.begin = n.current = A;
            ps3tc_gcm_state_transfer_scale_surface((CellGcmContextData *)&n, &neg2, &surf);
            if (A[9] != 0xff38ff9cu || A[11] != 0xffce01f4u) {
                printf("FAIL: transfer_scale_surface negative case 2 (safe): clip=0x%08x (exp 0xff38ff9c), out=0x%08x (exp 0xffce01f4)\n",
                       A[9], A[11]);
                failures++;
            } else {
                printf("  PASS: transfer_scale_surface negative case 2 (safe: 0xff38ff9c, 0xffce01f4)\n");
            }

            n.begin = n.current = A;
            ps3tc_gcm_state_transfer_scale_surface_unsafe((CellGcmContextData *)&n, &neg2, &surf);
            if (A[9] != 0xff38ff9cu || A[11] != 0xffce01f4u) {
                printf("FAIL: transfer_scale_surface negative case 2 (unsafe): clip=0x%08x (exp 0xff38ff9c), out=0x%08x (exp 0xffce01f4)\n",
                       A[9], A[11]);
                failures++;
            } else {
                printf("  PASS: transfer_scale_surface negative case 2 (unsafe: 0xff38ff9c, 0xffce01f4)\n");
            }
        }
    }

    /* transfer_scale_swizzle (18 words) */
    printf("\n  Transfer scale swizzle (18-word)\n");
    {
        const gcmTransferScale scale = {
            .conversion = 1,
            .format = 2,
            .operation = 3,
            .clipX = 10, .clipY = 20, .clipW = 100, .clipH = 200,
            .outX = 30, .outY = 40, .outW = 300, .outH = 400,
            .ratioX = 0x12345, .ratioY = 0x54321,
            .inW = 50, .inH = 60, .pitch = 512,
            .origin = 1, .interp = 2,
            .offset = 0x12345000,
            .inX = 5, .inY = 15
        };
        const gcmTransferSwizzle swiz = {
            .format = 5,
            .width = 6,
            .height = 7,
            .offset = 0x9ABC0000
        };
        static uint32_t A[32], B[32];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 32; n.callback = 0;
        r.begin = r.current = B; r.end = B + 32; r.callback = 0;
        ps3tc_gcm_state_transfer_scale_swizzle((CellGcmContextData *)&n, &scale, &swiz);
        ref_safe_transfer_scale_swizzle(&r, &scale, &swiz);
        cmp("  transfer_scale_swizzle (safe)", A, B, 18);
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_scale_swizzle_unsafe((CellGcmContextData *)&n, &scale, &swiz);
        ref_unsafe_transfer_scale_swizzle(&r, &scale, &swiz);
        cmp("  transfer_scale_swizzle (unsafe)", A, B, 18);

        /* Swap mutant: width and height swapped */
        gcmTransferSwizzle swiz_mut = swiz;
        swiz_mut.width = 7; swiz_mut.height = 6;
        r.begin = r.current = B;
        ref_safe_transfer_scale_swizzle(&r, &scale, &swiz_mut);
        cmp_mutant("  transfer_scale_swizzle (dim swap mutant)", A, B, 18);

        /* Negative coordinates: verify 16-bit clip/out packing avoids sign-extension smearing.
         * Checked against independently hand-computed expected words (not a duplicate cast):
         * Case 1: clipX=-1, clipY=2 -> (uint16_t)-1 = 0xffff, (uint16_t)2 = 0x0002 -> word 7 = 0x0002ffff
         *         outX=-3,  outY=4  -> (uint16_t)-3 = 0xfffd, (uint16_t)4 = 0x0004 -> word 9 = 0x0004fffd
         *         (The buggy unmasked code sign-extended into Y, writing 0xffffffff and 0xfffffffd)
         * Case 2: clipX=-100, clipY=-200 -> (uint16_t)-100 = 0xff9c, (uint16_t)-200 = 0xff38 -> word 7 = 0xff38ff9c
         *         outX=500,   outY=-50   -> (uint16_t)500  = 0x01f4, (uint16_t)-50   = 0xffce -> word 9 = 0xffce01f4
         */
        {
            gcmTransferScale neg1 = scale;
            neg1.clipX = -1; neg1.clipY = 2;
            neg1.outX = -3; neg1.outY = 4;
            n.begin = n.current = A;
            ps3tc_gcm_state_transfer_scale_swizzle((CellGcmContextData *)&n, &neg1, &swiz);
            if (A[7] != 0x0002ffffu || A[9] != 0x0004fffdu) {
                printf("FAIL: transfer_scale_swizzle negative case 1 (safe): clip=0x%08x (exp 0x0002ffff), out=0x%08x (exp 0x0004fffd)\n",
                       A[7], A[9]);
                failures++;
            } else {
                printf("  PASS: transfer_scale_swizzle negative case 1 (safe: 0x0002ffff, 0x0004fffd)\n");
            }

            n.begin = n.current = A;
            ps3tc_gcm_state_transfer_scale_swizzle_unsafe((CellGcmContextData *)&n, &neg1, &swiz);
            if (A[7] != 0x0002ffffu || A[9] != 0x0004fffdu) {
                printf("FAIL: transfer_scale_swizzle negative case 1 (unsafe): clip=0x%08x (exp 0x0002ffff), out=0x%08x (exp 0x0004fffd)\n",
                       A[7], A[9]);
                failures++;
            } else {
                printf("  PASS: transfer_scale_swizzle negative case 1 (unsafe: 0x0002ffff, 0x0004fffd)\n");
            }

            gcmTransferScale neg2 = scale;
            neg2.clipX = -100; neg2.clipY = -200;
            neg2.outX = 500; neg2.outY = -50;
            n.begin = n.current = A;
            ps3tc_gcm_state_transfer_scale_swizzle((CellGcmContextData *)&n, &neg2, &swiz);
            if (A[7] != 0xff38ff9cu || A[9] != 0xffce01f4u) {
                printf("FAIL: transfer_scale_swizzle negative case 2 (safe): clip=0x%08x (exp 0xff38ff9c), out=0x%08x (exp 0xffce01f4)\n",
                       A[7], A[9]);
                failures++;
            } else {
                printf("  PASS: transfer_scale_swizzle negative case 2 (safe: 0xff38ff9c, 0xffce01f4)\n");
            }

            n.begin = n.current = A;
            ps3tc_gcm_state_transfer_scale_swizzle_unsafe((CellGcmContextData *)&n, &neg2, &swiz);
            if (A[7] != 0xff38ff9cu || A[9] != 0xffce01f4u) {
                printf("FAIL: transfer_scale_swizzle negative case 2 (unsafe): clip=0x%08x (exp 0xff38ff9c), out=0x%08x (exp 0xffce01f4)\n",
                       A[7], A[9]);
                failures++;
            } else {
                printf("  PASS: transfer_scale_swizzle negative case 2 (unsafe: 0xff38ff9c, 0xffce01f4)\n");
            }
        }
    }

    /* transfer_data (12-word single-packet and 21-word multi-packet) */
    printf("\n  Transfer data (12-word single-packet and 21-word multi-packet)\n");
    {
        static uint32_t A[64], B[64];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 64; n.callback = 0;
        r.begin = r.current = B; r.end = B + 64; r.callback = 0;

        /* Single-packet case (12 words) */
        ps3tc_gcm_state_transfer_data((CellGcmContextData *)&n, 1, 0x1000, 256, 0x2000, 256, 128, 2);
        ref_safe_transfer_data(&r, 1, 0x1000, 256, 0x2000, 256, 128, 2);
        cmp("  transfer_data single (safe)", A, B, 12);

        /* Swap mutant: dstOffset and srcOffset swapped */
        r.begin = r.current = B;
        ref_safe_transfer_data(&r, 1, 0x2000, 256, 0x1000, 256, 128, 2);
        cmp_mutant("  transfer_data (offset swap mutant)", A, B, 12);

        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_data_unsafe((CellGcmContextData *)&n, 1, 0x1000, 256, 0x2000, 256, 128, 2);
        ref_unsafe_transfer_data(&r, 1, 0x1000, 256, 0x2000, 256, 128, 2);
        cmp("  transfer_data single (unsafe)", A, B, 12);

        /* Multi-packet pitch fallback case (21 words) */
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_data((CellGcmContextData *)&n, 3, 0x4000, 50000, 0x8000, 50000, 100, 2);
        ref_safe_transfer_data(&r, 3, 0x4000, 50000, 0x8000, 50000, 100, 2);
        cmp("  transfer_data multi-pitch (safe)", A, B, 21);

        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_data_unsafe((CellGcmContextData *)&n, 3, 0x4000, 50000, 0x8000, 50000, 100, 2);
        ref_unsafe_transfer_data(&r, 3, 0x4000, 50000, 0x8000, 50000, 100, 2);
        cmp("  transfer_data multi-pitch (unsafe)", A, B, 21);
    }

    /* transfer_image (26-word single-block and 46-word multi-block) */
    printf("\n  Transfer image (26-word single-block and 46-word multi-block)\n");
    {
        static uint32_t A[64], B[64];
        gcmContextData n, r;
        memset(&n, 0, sizeof n); memset(&r, 0, sizeof r);
        n.begin = n.current = A; n.end = A + 64; n.callback = 0;
        r.begin = r.current = B; r.end = B + 64; r.callback = 0;

        /* Single-block case (26 words) */
        ps3tc_gcm_state_transfer_image((CellGcmContextData *)&n, 2,
                                       0x10000, 1024, 10, 20,
                                       0x20000, 1024, 30, 40,
                                       100, 50, 4);
        ref_safe_transfer_image(&r, 2,
                                0x10000, 1024, 10, 20,
                                0x20000, 1024, 30, 40,
                                100, 50, 4);
        cmp("  transfer_image single (safe)", A, B, 26);

        /* Swap mutant: dstX and srcX swapped */
        r.begin = r.current = B;
        ref_safe_transfer_image(&r, 2,
                                0x10000, 1024, 30, 20,
                                0x20000, 1024, 10, 40,
                                100, 50, 4);
        cmp_mutant("  transfer_image (coord swap mutant)", A, B, 26);

        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_image_unsafe((CellGcmContextData *)&n, 2,
                                              0x10000, 1024, 10, 20,
                                              0x20000, 1024, 30, 40,
                                              100, 50, 4);
        ref_unsafe_transfer_image(&r, 2,
                                  0x10000, 1024, 10, 20,
                                  0x20000, 1024, 30, 40,
                                  100, 50, 4);
        cmp("  transfer_image single (unsafe)", A, B, 26);

        /* Multi-block boundary-crossing case (46 words) */
        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_image((CellGcmContextData *)&n, 1,
                                       0x30000, 2048, 1020, 10,
                                       0x40000, 2048, 0, 0,
                                       8, 4, 4);
        ref_safe_transfer_image(&r, 1,
                                0x30000, 2048, 1020, 10,
                                0x40000, 2048, 0, 0,
                                8, 4, 4);
        cmp("  transfer_image multi-block (safe)", A, B, 46);

        n.begin = n.current = A; r.begin = r.current = B;
        ps3tc_gcm_state_transfer_image_unsafe((CellGcmContextData *)&n, 1,
                                              0x30000, 2048, 1020, 10,
                                              0x40000, 2048, 0, 0,
                                              8, 4, 4);
        ref_unsafe_transfer_image(&r, 1,
                                  0x30000, 2048, 1020, 10,
                                  0x40000, 2048, 0, 0,
                                  8, 4, 4);
        cmp("  transfer_image multi-block (unsafe)", A, B, 46);
    }

    /* ---- Ring-full / callback: one case per native (point 3) ---- */
    printf("\n  Ring-full / callback (end==current, recording callback)\n");
    RING_CHECK(ps3tc_gcm_state_logic_op,
               ps3tc_gcm_state_logic_op_unsafe,
               "logic_op", 2, 0x1504);
    RING_CHECK(ps3tc_gcm_state_logic_op_enable,
               ps3tc_gcm_state_logic_op_enable_unsafe,
               "logic_op_enable", 2, 1);
    RING_CHECK(ps3tc_gcm_state_depth_func,
               ps3tc_gcm_state_depth_func_unsafe,
               "depth_func", 2, 3);
    RING_CHECK(ps3tc_gcm_state_depth_test_enable,
               ps3tc_gcm_state_depth_test_enable_unsafe,
               "depth_test_enable", 2, 1);
    RING_CHECK(ps3tc_gcm_state_depth_write_enable,
               ps3tc_gcm_state_depth_write_enable_unsafe,
               "depth_mask", 2, 1);
    RING_CHECK(ps3tc_gcm_state_depth_bounds_test_enable,
               ps3tc_gcm_state_depth_bounds_test_enable_unsafe,
               "depth_bounds_test_enable", 2, 1);
    RING_CHECK(ps3tc_gcm_state_depth_bounds,
               ps3tc_gcm_state_depth_bounds_unsafe,
               "depth_bounds(floats)", 3, 1.0f, 2.0f);
    RING_CHECK(ps3tc_gcm_state_blend_enable,
               ps3tc_gcm_state_blend_enable_unsafe,
               "blend_enable", 2, 1);
    RING_CHECK(ps3tc_gcm_state_blend_func,
               ps3tc_gcm_state_blend_func_unsafe,
               "blend_func (4x16)", 3, 0x01, 0x02, 0x03, 0x04);
    RING_CHECK(ps3tc_gcm_state_blend_equation,
               ps3tc_gcm_state_blend_equation_unsafe,
               "blend_equation", 2, 0x02, 0x04);
    RING_CHECK(ps3tc_gcm_state_blend_color,
               ps3tc_gcm_state_blend_color_unsafe,
               "blend_color (2 pkts)", 4, 0xA, 0xB);
    RING_CHECK(ps3tc_gcm_state_dither_enable,
               ps3tc_gcm_state_dither_enable_unsafe,
               "dither_enable", 2, 1);
    RING_CHECK(ps3tc_gcm_state_stencil_test_enable,
               ps3tc_gcm_state_stencil_test_enable_unsafe,
               "stencil_test_enable", 2, 1);
    RING_CHECK(ps3tc_gcm_state_two_sided_stencil_test_enable,
               ps3tc_gcm_state_two_sided_stencil_test_enable_unsafe,
               "two_sided_stencil_enable", 2, 1);
    RING_CHECK(ps3tc_gcm_state_stencil_mask,
               ps3tc_gcm_state_stencil_mask_unsafe,
               "stencil_mask", 2, 0xFF);
    RING_CHECK(ps3tc_gcm_state_back_stencil_mask,
               ps3tc_gcm_state_back_stencil_mask_unsafe,
               "back_stencil_mask", 2, 0xFF);
    RING_CHECK(ps3tc_gcm_state_stencil_func,
               ps3tc_gcm_state_stencil_func_unsafe,
               "stencil_func (c3)", 4, 0x03, 0x05, 0xFF);
    RING_CHECK(ps3tc_gcm_state_stencil_op,
               ps3tc_gcm_state_stencil_op_unsafe,
               "stencil_op (c3)", 4, 0x150a, 0x1e01, 0x1e02);
    RING_CHECK(ps3tc_gcm_state_back_stencil_func,
               ps3tc_gcm_state_back_stencil_func_unsafe,
               "back_stencil_func (c3)", 4, 0x03, 0x05, 0xFF);
     RING_CHECK(ps3tc_gcm_state_back_stencil_op,
               ps3tc_gcm_state_back_stencil_op_unsafe,
               "back_stencil_op (c3)", 4, 0x150a, 0x1e01, 0x1e02);
     RING_CHECK(ps3tc_gcm_state_color_mask,
               ps3tc_gcm_state_color_mask_unsafe,
               "color_mask", 2, 0x0F);
     RING_CHECK(ps3tc_gcm_state_color_mask_mrt,
               ps3tc_gcm_state_color_mask_mrt_unsafe,
               "color_mask_mrt", 2, 0x0F);
     RING_CHECK(ps3tc_gcm_state_shade_model,
               ps3tc_gcm_state_shade_model_unsafe,
               "shade_model", 2, 0x1d00);
     RING_CHECK(ps3tc_gcm_state_cull_face_enable,
               ps3tc_gcm_state_cull_face_enable_unsafe,
               "cull_face_enable", 2, 1);
     RING_CHECK(ps3tc_gcm_state_cull_face,
               ps3tc_gcm_state_cull_face_unsafe,
               "cull_face", 2, 0x03);
     RING_CHECK(ps3tc_gcm_state_front_face,
               ps3tc_gcm_state_front_face_unsafe,
               "front_face", 2, 0x01);
     RING_CHECK(ps3tc_gcm_state_front_polygon_mode,
               ps3tc_gcm_state_front_polygon_mode_unsafe,
               "front_polygon_mode", 2, 0x02);
     RING_CHECK(ps3tc_gcm_state_back_polygon_mode,
               ps3tc_gcm_state_back_polygon_mode_unsafe,
               "back_polygon_mode", 2, 0x02);
     RING_CHECK(ps3tc_gcm_state_polygon_offset_fill_enable,
               ps3tc_gcm_state_polygon_offset_fill_enable_unsafe,
               "polygon_offset_fill_enable", 2, 1);
     RING_CHECK(ps3tc_gcm_state_line_smooth_enable,
               ps3tc_gcm_state_line_smooth_enable_unsafe,
               "line_smooth_enable", 2, 1);
     RING_CHECK(ps3tc_gcm_state_polygon_smooth_enable,
               ps3tc_gcm_state_polygon_smooth_enable_unsafe,
               "polygon_smooth_enable", 2, 1);
     RING_CHECK(ps3tc_gcm_state_alpha_test_enable,
               ps3tc_gcm_state_alpha_test_enable_unsafe,
               "alpha_test_enable", 2, 1);
     RING_CHECK(ps3tc_gcm_state_alpha_func,
               ps3tc_gcm_state_alpha_func_unsafe,
               "alpha_func (2 data)", 3, 0x0210, 0x80);
      RING_CHECK(ps3tc_gcm_state_polygon_offset,
                ps3tc_gcm_state_polygon_offset_unsafe,
                "polygon_offset (floats)", 3, 1.0f, 2.0f);
    RING_CHECK(ps3tc_gcm_state_fog_mode,
               ps3tc_gcm_state_fog_mode_unsafe,
               "fog_mode", 2, 1);
    RING_CHECK(ps3tc_gcm_state_fog_params,
               ps3tc_gcm_state_fog_params_unsafe,
               "fog_params (2 floats)", 3, 1.0f, 2.0f);
    RING_CHECK(ps3tc_gcm_state_anti_aliasing_control,
               ps3tc_gcm_state_anti_aliasing_control_unsafe,
               "anti_aliasing", 2, 1, 1, 1, 0xFF);
    RING_CHECK(ps3tc_gcm_state_user_clip_plane_control,
               ps3tc_gcm_state_user_clip_plane_control_unsafe,
               "user_clip", 2, 1,1,1,1,1,1);
    RING_CHECK(ps3tc_gcm_state_fragment_program_gamma_enable,
               ps3tc_gcm_state_fragment_program_gamma_enable_unsafe,
               "fp_gamma", 2, 1);
    RING_CHECK(ps3tc_gcm_state_zminmax_control,
               ps3tc_gcm_state_zminmax_control_unsafe,
               "zminmax", 2, 1,1,1);
    RING_CHECK(ps3tc_gcm_state_render_enable,
               ps3tc_gcm_state_render_enable_unsafe,
               "render_enable (uncond)", 2, 0, 3);
    RING_CHECK(ps3tc_gcm_state_render_enable,
               ps3tc_gcm_state_render_enable_unsafe,
               "render_enable (cond)", 4, 2, 3);
    RING_CHECK(ps3tc_gcm_state_frequency_divider_operation,
               ps3tc_gcm_state_frequency_divider_operation_unsafe,
               "freq_divider", 2, 0x1234);
    const f32 vp_rs[4] = {1.5f,2.5f,3.5f,4.5f}, vp_os[4] = {-1.0f,-2.0f,-3.0f,-4.0f};
    RING_CHECK(ps3tc_gcm_state_viewport,
               ps3tc_gcm_state_viewport_unsafe,
               "viewport", 24, 8,12,256,144, 0.5f,8.25f, vp_rs, vp_os);
    RING_CHECK(ps3tc_gcm_state_scissor,
               ps3tc_gcm_state_scissor_unsafe,
               "scissor", 3, 32,48,320,240);
    RING_CHECK(ps3tc_gcm_state_clear_color,
               ps3tc_gcm_state_clear_color_unsafe,
               "clear_color", 2, 0xFF00FF00);
    RING_CHECK(ps3tc_gcm_state_clear_depth_stencil,
               ps3tc_gcm_state_clear_depth_stencil_unsafe,
               "clear_depth_stencil", 2, 0x3F800000);
    RING_CHECK(ps3tc_gcm_state_zcull_control,
               ps3tc_gcm_state_zcull_control_unsafe,
               "zcull_control", 2, 3, 2);
    RING_CHECK(ps3tc_gcm_state_zcull_enable,
               ps3tc_gcm_state_zcull_enable_unsafe,
               "zcull_enable", 2, 1, 0);
    RING_CHECK(ps3tc_gcm_state_zcull_limit,
               ps3tc_gcm_state_zcull_limit_unsafe,
               "zcull_limit", 2, 0x1111, 0x2222);
    RING_CHECK(ps3tc_gcm_state_zcull_stats_enable,
               ps3tc_gcm_state_zcull_stats_enable_unsafe,
               "zcull_stats_enable", 2, 1);
    RING_CHECK(ps3tc_gcm_state_scull_control,
               ps3tc_gcm_state_scull_control_unsafe,
               "scull_control", 2, 0x21, 0x80, 0x03);
    RING_CHECK(ps3tc_gcm_state_invalidate_zcull,
               ps3tc_gcm_state_invalidate_zcull_unsafe,
               "invalidate_zcull", 2);
    RING_CHECK(ps3tc_gcm_state_texture_border_color,
               ps3tc_gcm_state_texture_border_color_unsafe,
               "texture_border_color", 2, 0x05, 0xA5D1B2C3);
    RING_CHECK(ps3tc_gcm_state_vertex_attrib_output_mask,
               ps3tc_gcm_state_vertex_attrib_output_mask_unsafe,
               "vertex_attrib_output_mask", 2, 0x0C3A7751);
    RING_CHECK(ps3tc_gcm_state_zpass_pixel_count_enable,
               ps3tc_gcm_state_zpass_pixel_count_enable_unsafe,
               "zpass_pixel_count_enable", 2, 0x64B2E910);
    RING_CHECK(ps3tc_gcm_state_clear_zcull_surface,
               ps3tc_gcm_state_clear_zcull_surface_unsafe,
               "clear_zcull_surface", 2, 0x5A, 0x27);
    RING_CHECK(ps3tc_gcm_state_reference_command,
               ps3tc_gcm_state_reference_command_unsafe,
               "reference_command", 2, 0x1B8F42A9);
    RING_CHECK(ps3tc_gcm_state_invalidate_vertex_cache,
               ps3tc_gcm_state_invalidate_vertex_cache_unsafe,
               "invalidate_vertex_cache", 8);
    RING_CHECK(ps3tc_gcm_state_invalidate_texture_cache,
               ps3tc_gcm_state_invalidate_texture_cache_unsafe,
               "invalidate_texture_cache", 2, 0x43CD8E02);
    {
        const f32 v[4] = { 0.25f, -1.5f, 3.0f, 1000.0f };
        RING_CHECK(ps3tc_gcm_state_vertex_data_4f,
                   ps3tc_gcm_state_vertex_data_4f_unsafe,
                   "vertex_data_4f", 5, 0x2A, v);
    }
    RING_CHECK(ps3tc_gcm_state_clear_report,
               ps3tc_gcm_state_clear_report_unsafe,
               "clear_report", 2, 0x88C42A3D);
    {
        const CellGcmSurface surf = {
            .type = 0x01,
            .antiAlias = 0x02,
            .colorFormat = 0x03,
            .colorTarget = 0x04,
            .colorLocation = { 0x00, 0x01, 0x02, 0x03 },
            .colorOffset = { 0x1000, 0x2000, 0x3000, 0x4000 },
            .colorPitch = { 0x0100, 0x0200, 0x0300, 0x0400 },
            .depthFormat = 0x05,
            .depthLocation = 0x01,
            ._pad = { 0, 0 },
            .depthOffset = 0x5000,
            .depthPitch = 0x0500,
            .width = 1024,
            .height = 512,
            .x = 16,
            .y = 32
        };
        RING_CHECK(ps3tc_gcm_state_surface,
                   ps3tc_gcm_state_surface_unsafe,
                   "surface", 32, &surf);
    }
    RING_CHECK(ps3tc_gcm_state_vertex_data_array,
               ps3tc_gcm_state_vertex_data_array_unsafe,
               "vertex_data_array", 4, 0x07, 0x1234, 0x20, 0x04, 0x02, 0x01, 0x00567890);
    {
        const f32 c4[4] = { 1.5f, -2.5f, 3.25f, -0.125f };
        RING_CHECK(ps3tc_gcm_state_vertex_program_constants,
                   ps3tc_gcm_state_vertex_program_constants_unsafe,
                   "vertex_program_constants", 6, 0x20, 4, c4);
    }
    RING_CHECK(ps3tc_gcm_state_update_fragment_program_parameter,
               ps3tc_gcm_state_update_fragment_program_parameter_unsafe,
               "update_fragment_program_parameter", 2, 0x01ABCDEF);
    RING_CHECK(ps3tc_gcm_state_nop_command,
               ps3tc_gcm_state_nop_command_unsafe,
               "nop_command", 16, 16);
    RING_CHECK(ps3tc_gcm_state_report_location,
               ps3tc_gcm_state_report_location_unsafe,
               "report_location", 2, GCM_LOCATION_CELL);
    RING_CHECK(ps3tc_gcm_state_report,
               ps3tc_gcm_state_report_unsafe,
               "report", 2, 0x5A, 0x00001234u);
    RING_CHECK(ps3tc_gcm_state_transfer_data_mode,
               ps3tc_gcm_state_transfer_data_mode_unsafe,
               "transfer_data_mode", 3, 3);
    RING_CHECK(ps3tc_gcm_state_transfer_data_offset,
               ps3tc_gcm_state_transfer_data_offset_unsafe,
               "transfer_data_offset", 8, 0x12345678, 0x87654321);
    RING_CHECK(ps3tc_gcm_state_transfer_data_format,
               ps3tc_gcm_state_transfer_data_format_unsafe,
               "transfer_data_format", 6, 1000, 2000, 100, 50, 2, 4);
    RING_CHECK(ps3tc_gcm_state_transfer_scale_mode,
               ps3tc_gcm_state_transfer_scale_mode_unsafe,
               "transfer_scale_mode", 6, 3, 1);
    {
        const gcmTransferScale scale = {
            .conversion = 1, .format = 2, .operation = 3,
            .clipX = 10, .clipY = 20, .clipW = 100, .clipH = 200,
            .outX = 30, .outY = 40, .outW = 300, .outH = 400,
            .ratioX = 0x12345, .ratioY = 0x54321,
            .inW = 50, .inH = 60, .pitch = 512,
            .origin = 1, .interp = 2,
            .offset = 0x12345000,
            .inX = 5, .inY = 15
        };
        const gcmTransferSurface surf = {
            .format = 4, .pitch = 1024, ._pad0 = {0, 0}, .offset = 0x56780000
        };
        const gcmTransferSwizzle swiz = {
            .format = 5, .width = 6, .height = 7, .offset = 0x9ABC0000
        };
        RING_CHECK(ps3tc_gcm_state_transfer_scale_surface,
                   ps3tc_gcm_state_transfer_scale_surface_unsafe,
                   "transfer_scale_surface", 20, &scale, &surf);
        RING_CHECK(ps3tc_gcm_state_transfer_scale_swizzle,
                   ps3tc_gcm_state_transfer_scale_swizzle_unsafe,
                   "transfer_scale_swizzle", 18, &scale, &swiz);
    }


    printf("\n");
    if (failures == 0)
        printf("  PASS (all natives safe+unsafe == pre-patch rsx*, byte-for-byte; "
               "ring-full: safe=1 call with correct count, unsafe=0 calls)\n");
    else
        printf("  FAIL (%d mismatches)\n", failures);
    return failures ? 1 : 0;
}
