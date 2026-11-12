/*
 * samples/gcm/common/gcm_arena_common.h
 *
 * Shared pieces of the main-memory arena samples (hello-ppu-gcm-main-arena, hello-ppu-gcm-label-reuse):
 * display bring-up, a 1 MiB-aligned arena mapped with cellGcmMapMainMemory, the textured-quad program binding and
 * the readback helpers.  Static inline, no library, ILP32 and LP64.
 *
 * Arena rules these samples rely on (docs/gcm-labels.md):
 *   - cellGcmMapMainMemory needs a 1 MiB-aligned address and a size that is a nonzero multiple of 1 MiB; the arena
 *     comes from LV2 1 MiB pages (gcm_host_buffer_alloc), never from the sbrk heap.
 *   - A texture's offset must be 128-byte aligned; the samples keep vertex and index arrays 128-byte aligned too
 *     (16 bytes is what the RSX vertex fetch needs; 128 keeps one rule for every resource).
 *   - The RSX reads the arena through its IO mapping.  PPU stores into the arena become visible to it once a
 *     `sync` orders them before the PUT-pointer store that publishes the commands reading them.  cellGcmFlush does
 *     exactly that (librsx FlushBuffer: sync, then PUT).  Data written before the flush that publishes its draw
 *     needs nothing more; no dcbst/dcbf is used.
 *   - When the PPU rewrites arena content the RSX already read (the same offsets), the draws that read the new
 *     content must be preceded by cellGcmSetInvalidateVertexCache / cellGcmSetInvalidateTextureCache.
 */
#ifndef PS3TC_SAMPLES_GCM_ARENA_COMMON_H
#define PS3TC_SAMPLES_GCM_ARENA_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cell/gcm.h>
#include <cell/gcm/ps3tc_gcm_labels.h> /* also reached through cell/gcm.h from this SDK version on */
#include <sysutil/video.h>

#include "gcm_host_buffer.h"

typedef struct arena_vertex {
    float pos[2];
    float uv[2];
} arena_vertex;

typedef struct arena_display {
    uint32_t width, height, color_pitch, depth_pitch;
    uint32_t color_offset[2], depth_offset;
    uint8_t *local_next; /* bump allocator over the rest of RSX local memory */
    uint8_t *local_end;
} arena_display;

typedef struct arena_program {
    CGprogram vp, fp;
    void *vp_ucode;
    uint32_t fp_offset;
    int position_index, texcoord_index;
} arena_program;

static inline void *arena_local_alloc(arena_display *d, uint32_t align, uint32_t size)
{
    uintptr_t p = ((uintptr_t)d->local_next + align - 1) & ~(uintptr_t)(align - 1);
    if (p + size > (uintptr_t)d->local_end)
        return NULL;
    d->local_next = (uint8_t *)(p + size);
    return (void *)p;
}

/* 0 on success.  Two XRGB display buffers and one Z16 depth buffer in local memory. */
static inline int arena_display_init(arena_display *d)
{
    videoState state;
    videoResolution res;
    videoConfiguration video;
    CellGcmConfig config;
    memset(d, 0, sizeof *d);
    memset(&video, 0, sizeof video);
    if (videoGetState(0, 0, &state) || state.state != 0 || videoGetResolution(state.displayMode.resolution, &res))
        return -1;
    d->width = res.width;
    d->height = res.height;
    if (d->width < 640 || d->height < 480 || d->width > 4096 || d->height > 4096)
        return -1;
    d->color_pitch = (d->width * 4 + 63) & ~63u;
    d->depth_pitch = (d->width * 2 + 63) & ~63u;
    video.resolution = state.displayMode.resolution;
    video.format = VIDEO_BUFFER_FORMAT_XRGB;
    video.aspect = state.displayMode.aspect;
    video.pitch = d->color_pitch;
    if (videoConfigure(0, &video, NULL, 0))
        return -1;
    cellGcmSetFlipMode(GCM_FLIP_VSYNC);
    cellGcmGetConfiguration(&config);
    d->local_next = (uint8_t *)(uintptr_t)config.localAddress;
    d->local_end = d->local_next + config.localSize;
    for (unsigned n = 0; n < 2; ++n) {
        void *p = arena_local_alloc(d, 64, d->color_pitch * d->height);
        if (!p || cellGcmAddressToOffset(p, &d->color_offset[n]) ||
            cellGcmSetDisplayBuffer((uint8_t)n, d->color_offset[n], d->color_pitch, d->width, d->height))
            return -1;
    }
    void *z = arena_local_alloc(d, 64, d->depth_pitch * d->height);
    if (!z || cellGcmAddressToOffset(z, &d->depth_offset))
        return -1;
    return 0;
}

/* A linear X8R8G8B8 target at a local-memory offset (depth Z16 at depth_offset, pitch depth_pitch). */
static inline void arena_set_target(CellGcmContextData *ctx, uint32_t color_offset, uint32_t color_pitch,
                                    uint32_t depth_offset, uint32_t depth_pitch, uint32_t w, uint32_t h)
{
    CellGcmSurface s;
    memset(&s, 0, sizeof s);
    s.colorFormat = GCM_SURFACE_X8R8G8B8;
    s.colorTarget = GCM_SURFACE_TARGET_0;
    for (unsigned i = 0; i < 4; ++i) {
        s.colorLocation[i] = GCM_LOCATION_RSX;
        s.colorPitch[i] = i ? 64 : color_pitch;
    }
    s.colorOffset[0] = color_offset;
    s.depthFormat = GCM_SURFACE_ZETA_Z16;
    s.depthLocation = GCM_LOCATION_RSX;
    s.depthOffset = depth_offset;
    s.depthPitch = depth_pitch;
    s.type = GCM_SURFACE_TYPE_LINEAR;
    s.antiAlias = GCM_SURFACE_CENTER_1;
    s.width = (uint16_t)w;
    s.height = (uint16_t)h;
    cellGcmSetSurface(ctx, &s);
    cellGcmSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    cellGcmSetColorMaskMrt(ctx, 0);
    float scale[4] = {w * 0.5f, h * -0.5f, 0.5f, 0.0f};
    float offset[4] = {w * 0.5f, h * 0.5f, 0.5f, 0.0f};
    cellGcmSetViewport(ctx, 0, 0, (uint16_t)w, (uint16_t)h, 0.0f, 1.0f, scale, offset);
    cellGcmSetScissor(ctx, 0, 0, (uint16_t)w, (uint16_t)h);
    cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
    cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
    cellGcmSetBlendEnable(ctx, GCM_FALSE);
    cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
}

/* 0 on success.  The fragment ucode is copied into local memory (it must be RSX-visible). */
static inline int arena_program_init(arena_display *d, arena_program *p, const void *vp_blob, const void *fp_blob)
{
    uint32_t vsize = 0, fsize = 0;
    void *fblob;
    p->vp = (CGprogram)(uintptr_t)vp_blob;
    p->fp = (CGprogram)(uintptr_t)fp_blob;
    cellGcmCgInitProgram(p->vp);
    cellGcmCgInitProgram(p->fp);
    cellGcmCgGetUCode(p->vp, &p->vp_ucode, &vsize);
    cellGcmCgGetUCode(p->fp, &fblob, &fsize);
    void *fucode = arena_local_alloc(d, 64, fsize);
    if (!fucode)
        return -1;
    memcpy(fucode, fblob, fsize);
    if (cellGcmAddressToOffset(fucode, &p->fp_offset))
        return -1;
    CGparameter pos = cellGcmCgGetNamedParameter(p->vp, "in_position");
    CGparameter tc = cellGcmCgGetNamedParameter(p->vp, "in_texcoord");
    if (!pos || !tc)
        return -1;
    p->position_index = (int)cellGcmCgGetParameterResource(p->vp, pos) - CG_ATTR0;
    p->texcoord_index = (int)cellGcmCgGetParameterResource(p->vp, tc) - CG_ATTR0;
    return 0;
}

/* Binds the program, the vertex arrays (main-memory offset `vbo`) and a linear A8R8G8B8 texture (main-memory
 * offset `tex`, w x h, nearest filtering). */
static inline void arena_bind(CellGcmContextData *ctx, const arena_program *p, uint32_t vbo, uint32_t tex,
                              uint32_t w, uint32_t h)
{
    CellGcmTexture t;
    cellGcmSetVertexProgram(ctx, p->vp, p->vp_ucode);
    cellGcmSetVertexDataArray(ctx, (uint8_t)p->position_index, 0, sizeof(arena_vertex), 2, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_MAIN, vbo + (uint32_t)offsetof(arena_vertex, pos));
    cellGcmSetVertexDataArray(ctx, (uint8_t)p->texcoord_index, 0, sizeof(arena_vertex), 2, CELL_GCM_VERTEX_F,
                              CELL_GCM_LOCATION_MAIN, vbo + (uint32_t)offsetof(arena_vertex, uv));
    cellGcmSetFragmentProgram(ctx, p->fp, p->fp_offset);
    memset(&t, 0, sizeof t);
    t.format = CELL_GCM_TEXTURE_A8R8G8B8 | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
    t.mipmap = 1;
    t.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
    t.cubemap = CELL_GCM_FALSE;
    t.remap = (CELL_GCM_TEXTURE_REMAP_REMAP << 8) | (CELL_GCM_TEXTURE_REMAP_REMAP << 10) |
              (CELL_GCM_TEXTURE_REMAP_REMAP << 12) | (CELL_GCM_TEXTURE_REMAP_REMAP << 14) |
              (CELL_GCM_TEXTURE_REMAP_FROM_A << 0) | (CELL_GCM_TEXTURE_REMAP_FROM_R << 2) |
              (CELL_GCM_TEXTURE_REMAP_FROM_G << 4) | (CELL_GCM_TEXTURE_REMAP_FROM_B << 6);
    t.width = (uint16_t)w;
    t.height = (uint16_t)h;
    t.depth = 1;
    t.location = CELL_GCM_LOCATION_MAIN;
    t.pitch = w * 4;
    t.offset = tex;
    cellGcmSetTexture(ctx, 0, &t);
    cellGcmSetTextureControl(ctx, 0, CELL_GCM_TRUE, 0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
    cellGcmSetTextureFilter(ctx, 0, 0, CELL_GCM_TEXTURE_NEAREST, CELL_GCM_TEXTURE_NEAREST,
                            CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    cellGcmSetTextureAddress(ctx, 0, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
                             CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                             CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
}

/* A quad covering NDC [x0,x1] x [y0,y1] with uv 0..1, as 4 vertices (index order 0 1 2, 2 1 3). */
static inline void arena_quad(arena_vertex *v, float x0, float y0, float x1, float y1)
{
    v[0] = (arena_vertex){{x0, y1}, {0.0f, 0.0f}};
    v[1] = (arena_vertex){{x1, y1}, {1.0f, 0.0f}};
    v[2] = (arena_vertex){{x0, y0}, {0.0f, 1.0f}};
    v[3] = (arena_vertex){{x1, y0}, {1.0f, 1.0f}};
}

static inline void arena_fill(uint32_t *texels, uint32_t n, uint32_t argb)
{
    for (uint32_t i = 0; i < n; ++i)
        texels[i] = argb;
}

#endif
