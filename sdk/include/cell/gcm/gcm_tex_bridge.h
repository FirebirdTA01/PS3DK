/*
 * PS3 Custom Toolchain — <cell/gcm/gcm_tex_bridge.h>
 *
 * Native RSX command emitter for the cellGcmSetTexture /
 * cellGcmSetTextureControl / cellGcmSetTextureFilter /
 * cellGcmSetTextureAddress surface.  Replaces the rsxLoadTexture /
 * rsxTextureControl / rsxTextureFilter / rsxTextureWrapMode forwarders
 * that previously lived in gcm_command_c.h — the bit-packing logic is
 * identical (copied from PSL1GHT commands_impl.h which is already on
 * the wire), we just drop the C function-call layer and its OPD/PRX
 * quirks so the emitters inline into the caller.
 *
 * FIFO reservation + callback invocation is shared with gcm_cg_bridge.h
 * via #include below — ps3tc_gcm_reserve / ps3tc_gcm_invoke_callback /
 * PS3TC_GCM_METHOD all come from there, single source of truth.
 *
 * Trace gated by the same PS3TC_GCM_CG_BRIDGE_TRACE flag as the cg
 * bridge — one Makefile -D enables both wire-level traces when you
 * need to diff FIFO output against the old forwarder version.
 */

#ifndef PS3TC_CELL_GCM_TEX_BRIDGE_H
#define PS3TC_CELL_GCM_TEX_BRIDGE_H

#include <stdint.h>
#include <rsx/gcm_sys.h>   /* gcmTexture / gcmContextData + GCM_LOCATION_* */
#include <rsx/nv40.h>      /* NV40TCL_TEX_* method IDs + field shifts */

/* Pulls in ps3tc_gcm_reserve, ps3tc_gcm_invoke_callback, PS3TC_GCM_METHOD
 * — see the comment block at the top of gcm_cg_bridge.h for why the
 * callback must go through the asm-wrapped noinline helper. */
#include <cell/gcm/gcm_cg_bridge.h>

#ifdef __cplusplus
extern "C" {
#endif

#if PS3TC_GCM_CG_BRIDGE_TRACE
#  define PS3TC_TEX_TRACE(...) do { printf("[tex_bridge] " __VA_ARGS__); fflush(stdout); } while (0)
#else
#  define PS3TC_TEX_TRACE(...) ((void)0)
#endif

/* -------------------------------------------------------------------- *
 * Load texture — emits NV40TCL_TEX_OFFSET / TEX_FORMAT / TEX_SWIZZLE /
 * TEX_SIZE0 / TEX_SIZE1 for the given texture unit.  9 FIFO words
 * total (4 method headers + 5 data).  Bit packing matches PSL1GHT
 * rsxLoadTexture byte-for-byte; the "+ 0x8000" literal in `format`
 * sets the DMA-format-select / no-border bit the hardware requires
 * on all sampler paths.
 * -------------------------------------------------------------------- */
static inline void ps3tc_gcm_load_texture(CellGcmContextData *ctx,
                                          uint8_t index,
                                          const gcmTexture *texture)
{
    if (!ctx || !texture) return;

    const uint32_t offset  = texture->offset;
    const uint32_t format  = ((uint32_t)(texture->location + 1)
                            | ((uint32_t)texture->cubemap   << 2)
                            | ((uint32_t)texture->dimension << NV40TCL_TEX_FORMAT_DIMS_SHIFT)
                            | ((uint32_t)texture->format    << NV40TCL_TEX_FORMAT_FORMAT_SHIFT)
                            | ((uint32_t)texture->mipmap    << NV40TCL_TEX_FORMAT_MIPMAP_COUNT_SHIFT)
                            | NV40TCL_TEX_FORMAT_NO_BORDER
                            | 0x8000u);
    const uint32_t swizzle = texture->remap;
    const uint32_t size0   = ((uint32_t)texture->width << NV40TCL_TEX_SIZE0_W_SHIFT)
                           | (uint32_t)texture->height;
    const uint32_t size1   = ((uint32_t)texture->depth << NV40TCL_TEX_SIZE1_DEPTH_SHIFT)
                           | (uint32_t)texture->pitch;

    PS3TC_TEX_TRACE("LoadTex idx=%u off=0x%08x fmt=0x%08x swz=0x%08x s0=0x%08x s1=0x%08x\n",
                    (unsigned)index, (unsigned)offset, (unsigned)format,
                    (unsigned)swizzle, (unsigned)size0, (unsigned)size1);

    uint32_t *w = ps3tc_gcm_reserve(ctx, 9);
    if (!w) return;

    /* TEX_OFFSET + auto-incremented TEX_FORMAT (count=2). */
    *w++ = PS3TC_GCM_METHOD(NV40TCL_TEX_OFFSET(index), 2);
    *w++ = offset;
    *w++ = format;

    /* TEX_SWIZZLE (single-word). */
    *w++ = PS3TC_GCM_METHOD(NV40TCL_TEX_SWIZZLE(index), 1);
    *w++ = swizzle;

    /* TEX_SIZE0 (width:height). */
    *w++ = PS3TC_GCM_METHOD(NV40TCL_TEX_SIZE0(index), 1);
    *w++ = size0;

    /* TEX_SIZE1 (depth:pitch). */
    *w++ = PS3TC_GCM_METHOD(NV40TCL_TEX_SIZE1(index), 1);
    *w++ = size1;
}

/* -------------------------------------------------------------------- *
 * Sampler control — emits NV40TCL_TEX_ENABLE for the given unit.
 * Packs enable/minlod/maxlod/maxaniso into one 32-bit register.
 * -------------------------------------------------------------------- */
static inline void ps3tc_gcm_tex_control(CellGcmContextData *ctx,
                                         uint8_t  index,
                                         uint32_t enable,
                                         uint16_t minlod,
                                         uint16_t maxlod,
                                         uint8_t  maxaniso)
{
    if (!ctx) return;

    const uint32_t val = ((uint32_t)enable   << NV40TCL_TEX_ENABLE_SHIFT)
                       | ((uint32_t)minlod   << NV40TCL_TEX_MINLOD_SHIFT)
                       | ((uint32_t)maxlod   << NV40TCL_TEX_MAXLOD_SHIFT)
                       | ((uint32_t)maxaniso << NV40TCL_TEX_MAXANISO_SHIFT);

    PS3TC_TEX_TRACE("TexCtl idx=%u val=0x%08x (en=%u min=%u max=%u aniso=%u)\n",
                    (unsigned)index, (unsigned)val,
                    (unsigned)enable, (unsigned)minlod,
                    (unsigned)maxlod, (unsigned)maxaniso);

    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_TEX_ENABLE(index), 1);
    *w++ = val;
}

/* -------------------------------------------------------------------- *
 * Sampler filter — emits NV40TCL_TEX_FILTER for the given unit.
 * The LOD-bias lives in the low 13 bits; MIN/MAG/CONV are at higher
 * nibbles — see nv40.h NV40TCL_TEX_FILTER_* shifts.
 * -------------------------------------------------------------------- */
static inline void ps3tc_gcm_tex_filter(CellGcmContextData *ctx,
                                        uint8_t  index,
                                        uint16_t bias,
                                        uint8_t  min,
                                        uint8_t  mag,
                                        uint8_t  conv)
{
    if (!ctx) return;

    const uint32_t val = ((uint32_t)mag  << NV40TCL_TEX_FILTER_MAG_SHIFT)
                       | ((uint32_t)min  << NV40TCL_TEX_FILTER_MIN_SHIFT)
                       | ((uint32_t)conv << NV40TCL_TEX_FILTER_CONV_SHIFT)
                       | ((uint32_t)bias & 0x1fffu);

    PS3TC_TEX_TRACE("TexFlt idx=%u val=0x%08x (mag=%u min=%u conv=%u bias=%u)\n",
                    (unsigned)index, (unsigned)val,
                    (unsigned)mag, (unsigned)min,
                    (unsigned)conv, (unsigned)bias);

    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_TEX_FILTER(index), 1);
    *w++ = val;
}

/* -------------------------------------------------------------------- *
 * Sampler wrap/addressing — emits NV40TCL_TEX_WRAP for the given unit.
 * Packs S/T/R wrap modes, unsigned-remap bit, sRGB gamma bit, and
 * the shadow-compare zfunc into one register.
 * -------------------------------------------------------------------- */
static inline void ps3tc_gcm_tex_wrap_mode(CellGcmContextData *ctx,
                                           uint8_t index,
                                           uint8_t wraps,
                                           uint8_t wrapt,
                                           uint8_t wrapr,
                                           uint8_t unsignedRemap,
                                           uint8_t zfunc,
                                           uint8_t gamma)
{
    if (!ctx) return;

    const uint32_t val = ((uint32_t)wraps         << NV40TCL_TEX_WRAP_S_SHIFT)
                       | ((uint32_t)wrapt         << NV40TCL_TEX_WRAP_T_SHIFT)
                       | ((uint32_t)wrapr         << NV40TCL_TEX_WRAP_R_SHIFT)
                       | ((uint32_t)unsignedRemap << NV40TCL_TEX_UREMAP_SHIFT)
                       | ((uint32_t)gamma         << NV40TCL_TEX_GAMMA_SHIFT)
                       | ((uint32_t)zfunc         << NV40TCL_TEX_ZFUNC_SHIFT);

    PS3TC_TEX_TRACE("TexWrap idx=%u val=0x%08x (s=%u t=%u r=%u urem=%u z=%u gam=%u)\n",
                    (unsigned)index, (unsigned)val,
                    (unsigned)wraps, (unsigned)wrapt, (unsigned)wrapr,
                    (unsigned)unsignedRemap, (unsigned)zfunc, (unsigned)gamma);

    uint32_t *w = ps3tc_gcm_reserve(ctx, 2);
    if (!w) return;
    *w++ = PS3TC_GCM_METHOD(NV40TCL_TEX_WRAP(index), 1);
    *w++ = val;
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_GCM_TEX_BRIDGE_H */
