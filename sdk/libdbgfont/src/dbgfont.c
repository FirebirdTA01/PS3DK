/*
 * PS3 Custom Toolchain — libdbgfont / dbgfont.c
 *
 * Tier-2 renderer for the cellDbgFont* API family (see
 * sdk/include/cell/dbgfont.h).  Submits NV40 draw commands that
 * overlay text on top of the current frame using an 8-bit font
 * atlas + a vertex/fragment shader pair compiled by cgcomp.
 *
 * Pipeline:
 *   cellDbgFontInitGcm(cfg)
 *     - slice the provided local buffer into { fragment-ucode,
 *       texture-atlas, vertex-pool } regions;
 *     - upload the font atlas to the texture region;
 *     - memcpy the fragment ucode into vidmem (the vertex ucode
 *       lives in sysmem as delivered by cgcomp).
 *
 *   cellDbgFontPuts / cellDbgFontPrintf (x, y, scale, color, ...)
 *     - convert normalized [0..1] position + ARGB color into
 *       per-glyph quads, appended to the vertex pool;
 *     - no FIFO traffic — buffering only.
 *
 *   cellDbgFontDrawGcm()
 *     - emit all the state (blend, depth-off, VP/FP bind, texture,
 *       attribs) + the draw(quads), and zero the vertex count.
 *
 *   cellDbgFontExitGcm()
 *     - no-op; caller owns the localBufAddr allocation.
 *
 * The shaders and atlas are embedded as bin2s symbols — see
 * build/(vp|fp)shader_dbgfont.(vpo|fpo).o and
 * src/dbgfont_atlas_data.h.
 *
 * Layout math: the sample provides x,y as normalized [0..1] screen
 * coordinates.  We map to NDC as (2x−1, 1−2y) so y=0 is the top of
 * the frame.  Glyph size in NDC is a fixed pair of constants scaled
 * by the user's `scale` argument — we assume ~1280×720 as the
 * reference resolution, which gives the same visual size as the
 * Sony samples on a 720p RPCS3 window.  There's no runtime query of
 * the actual surface resolution (the API doesn't pass it); change
 * DBGFONT_GLYPH_NDC_W/H if your app uses a non-standard resolution.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <ppu-asm.h>
#include <ppu-types.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>

#include <cell/dbgfont.h>
#include <cell/gcm.h>

/* 128x128 8-bit-alpha font atlas — bytes stored as a brace-list
 * fragment in dbgfont_atlas_data.h (matches PSL1GHT's include-in-
 * initializer idiom).  ASCII mapped starting at space on row 2. */
static const uint8_t dbgfont_atlas_data[128 * 128] = {
#include "dbgfont_atlas_data.h"
};

/* ----- atlas + glyph constants (match PSL1GHT's dbgfont.cpp) ------ */
#define DBGFONT_ATLAS_W            128
#define DBGFONT_ATLAS_H            128
#define DBGFONT_ATLAS_BYTES        (DBGFONT_ATLAS_W * DBGFONT_ATLAS_H)
#define DBGFONT_GLYPH_W_PX         8
#define DBGFONT_GLYPH_H_PX         9
#define DBGFONT_TAB_SIZE           4
#define DBGFONT_MAX_VERTICES       (CELL_DBGFONT_VERTEX_SIZE * 32)   /* 1024 quads = 4096 verts worst-case; conservative default. */

/* Glyph size in NDC units at scale=1.0 on a 1280x720 reference.
 * NDC ranges from -1..+1 across the screen → 2 units total.  An 8px
 * glyph on a 1280px-wide screen is 2 * 8 / 1280 = 0.0125 wide; a 9px
 * tall glyph on 720 tall is 2 * 9 / 720 = 0.025.  Kerning adds 1 px
 * horizontally (glyph+1) and vertically (glyph+1). */
#define DBGFONT_GLYPH_NDC_W        (2.0f * (float)DBGFONT_GLYPH_W_PX / 1280.0f)
#define DBGFONT_GLYPH_NDC_H        (2.0f * (float)DBGFONT_GLYPH_H_PX / 720.0f)
#define DBGFONT_KERN_NDC_W         (2.0f * (float)(DBGFONT_GLYPH_W_PX + 1) / 1280.0f)
#define DBGFONT_LINE_NDC_H         (2.0f * (float)(DBGFONT_GLYPH_H_PX + 1) / 720.0f)

/* ----- bin2s-emitted shader binaries ------------------------------ */
extern const uint8_t vpshader_dbgfont_vpo[];
extern const uint8_t fpshader_dbgfont_fpo[];

/* ----- renderer state --------------------------------------------- */

typedef struct { float x, y, z; }      DbgPos;
typedef struct { float s, t; }         DbgUV;
typedef struct { float r, g, b, a; }   DbgColor;

static struct {
    int      initialized;

    /* Region layout inside the caller-provided local buffer. */
    uint8_t *atlas_mem;        /* vidmem addr of font atlas upload   */
    uint32_t atlas_off;        /* RSX IO offset for the atlas        */
    uint8_t *fp_ucode_mem;     /* vidmem addr of fragment ucode copy */
    uint32_t fp_ucode_off;     /* RSX IO offset for fragment ucode   */

    /* Vertex pool — interleaved in caller's buffer: positions, then
     * texcoords, then colors.  We keep CPU-visible pointers + IO
     * offsets for each stream. */
    DbgPos   *pos_cpu;
    DbgUV    *uv_cpu;
    DbgColor *col_cpu;
    uint32_t  pos_off;
    uint32_t  uv_off;
    uint32_t  col_off;
    int       num_verts;
    int       max_verts;

    /* Shader handles — the vp_ucode pointer is sys-mem delivered by
     * cgcomp-compiled vpo; the fp got memcpy'd into fp_ucode_mem. */
    rsxVertexProgram   *vp;
    rsxFragmentProgram *fp;
    void               *vp_ucode;

    /* Shader attribute slot indices (resolved via rsx*ProgramGetAttrib
     * against the shader names "position", "color", "texcoord",
     * "texture"). */
    int pos_attrib;
    int col_attrib;
    int uv_attrib;
    int tex_unit;
} g_dbgfont;

/* ------------------------------------------------------------------ */
/*   Helpers                                                          */
/* ------------------------------------------------------------------ */

static inline float argb_to_f(uint32_t channel)
{
    return (float)channel * (1.0f / 255.0f);
}

static void unpack_argb(uint32_t color, DbgColor *out)
{
    out->a = argb_to_f((color >> 24) & 0xff);
    out->r = argb_to_f((color >> 16) & 0xff);
    out->g = argb_to_f((color >> 8)  & 0xff);
    out->b = argb_to_f( color        & 0xff);
}

/* UV calculation for glyph `c` in the 128x128 atlas — 16 glyphs per
 * row.  The atlas data starts at row 2 (ASCII 0x20 == space begins
 * on the third row), matching PSL1GHT's calcT0/T1. */
static inline float glyph_s0(uint8_t c) { return (float)((c % 16) * DBGFONT_GLYPH_W_PX) / (float)DBGFONT_ATLAS_W; }
static inline float glyph_s1(uint8_t c) { return (float)(((c % 16) * DBGFONT_GLYPH_W_PX) + DBGFONT_GLYPH_W_PX) / (float)DBGFONT_ATLAS_W; }
static inline float glyph_t0(uint8_t c) { return (float)((((c / 16) - 2) * DBGFONT_GLYPH_H_PX) + 1) / (float)DBGFONT_ATLAS_H; }
static inline float glyph_t1(uint8_t c) { return (float)((((c / 16) - 2) * DBGFONT_GLYPH_H_PX) + DBGFONT_GLYPH_H_PX + 1) / (float)DBGFONT_ATLAS_H; }

static inline int is_printable(char c) { return ((uint8_t)c & 0x7fu) > 31; }

/* ------------------------------------------------------------------ */
/*   Init / Exit                                                      */
/* ------------------------------------------------------------------ */

int32_t cellDbgFontInitGcm(const CellDbgFontConfigGcm *cfg)
{
    if (!cfg || !cfg->localBufAddr || cfg->localBufSize == 0) return -1;

    memset(&g_dbgfont, 0, sizeof(g_dbgfont));

    uint8_t *base = (uint8_t *)(uintptr_t)cfg->localBufAddr;

    /* Region carve-up matches the sample's allocator sketch:
     *   [ fragment ucode: CELL_DBGFONT_FRAGMENT_SIZE bytes ]
     *   [ texture atlas: CELL_DBGFONT_TEXTURE_SIZE bytes   ]
     *   [ vertex pool: remainder                            ]
     * The sample passes localBufSize = frag + tex + 1024*vtx_bytes
     * so the remainder is exactly the vertex pool. */
    const uint32_t frag_bytes  = CELL_DBGFONT_FRAGMENT_SIZE;
    const uint32_t atlas_bytes = CELL_DBGFONT_TEXTURE_SIZE;
    if (cfg->localBufSize < frag_bytes + atlas_bytes) return -1;

    g_dbgfont.fp_ucode_mem = base;
    g_dbgfont.atlas_mem    = base + frag_bytes;

    /* Vertex pool region + stream partitioning.  Interleaving is
     * single-attribute-per-stream (positions | texcoords | colors) so
     * each array can be handed to rsxBindVertexArrayAttrib with a
     * tight stride.  Divide the remaining space proportionally. */
    uint32_t vtx_bytes = cfg->localBufSize - frag_bytes - atlas_bytes;
    uint32_t per_vertex = sizeof(DbgPos) + sizeof(DbgUV) + sizeof(DbgColor);
    uint32_t max_verts  = vtx_bytes / per_vertex;
    if (max_verts < 4) return -1;  /* not even one quad */

    uint8_t *vpool = base + frag_bytes + atlas_bytes;
    g_dbgfont.pos_cpu = (DbgPos   *)(vpool);
    g_dbgfont.uv_cpu  = (DbgUV    *)(vpool + max_verts * sizeof(DbgPos));
    g_dbgfont.col_cpu = (DbgColor *)(vpool + max_verts * (sizeof(DbgPos) + sizeof(DbgUV)));
    g_dbgfont.num_verts = 0;
    g_dbgfont.max_verts = (int)max_verts;

    /* Resolve RSX IO offsets for each resident region. */
    if (gcmAddressToOffset(g_dbgfont.atlas_mem,    &g_dbgfont.atlas_off)    != 0) return -1;
    if (gcmAddressToOffset(g_dbgfont.fp_ucode_mem, &g_dbgfont.fp_ucode_off) != 0) return -1;
    if (gcmAddressToOffset(g_dbgfont.pos_cpu, &g_dbgfont.pos_off) != 0) return -1;
    if (gcmAddressToOffset(g_dbgfont.uv_cpu,  &g_dbgfont.uv_off)  != 0) return -1;
    if (gcmAddressToOffset(g_dbgfont.col_cpu, &g_dbgfont.col_off) != 0) return -1;

    /* Copy the atlas bitmap into vidmem.  The provided texture
     * region is 256x256; we only use the top-left 128x128. */
    memcpy(g_dbgfont.atlas_mem, dbgfont_atlas_data, DBGFONT_ATLAS_BYTES);

    /* The compiled .vpo/.fpo are cgBinaryProgram blobs.  Cast to the
     * opaque PSL1GHT/Sony shader types; rsxVertexProgramGetUCode
     * returns the sysmem-resident ucode pointer from inside the blob,
     * and rsxFragmentProgramGetUCode returns an in-blob pointer we
     * then copy into vidmem because the fragment program must be
     * bound at an RSX-visible offset. */
    g_dbgfont.vp = (rsxVertexProgram   *)vpshader_dbgfont_vpo;
    g_dbgfont.fp = (rsxFragmentProgram *)fpshader_dbgfont_fpo;

    void *fp_src = NULL;
    uint32_t fp_size = 0;
    rsxFragmentProgramGetUCode(g_dbgfont.fp, &fp_src, &fp_size);
    if (!fp_src || fp_size > frag_bytes) return -1;
    memcpy(g_dbgfont.fp_ucode_mem, fp_src, fp_size);

    uint32_t vp_size = 0;
    rsxVertexProgramGetUCode(g_dbgfont.vp, &g_dbgfont.vp_ucode, &vp_size);

    /* Resolve attribute slots.  The VP / FP sources declare these
     * by name — the shader compiler records the binding register,
     * and rsx*ProgramGetAttrib retrieves it at runtime. */
    rsxProgramAttrib *pa = rsxVertexProgramGetAttrib(g_dbgfont.vp, "position");
    rsxProgramAttrib *ca = rsxVertexProgramGetAttrib(g_dbgfont.vp, "color");
    rsxProgramAttrib *ta = rsxVertexProgramGetAttrib(g_dbgfont.vp, "texcoord");
    rsxProgramAttrib *tu = rsxFragmentProgramGetAttrib(g_dbgfont.fp, "texture");
    if (!pa || !ca || !ta || !tu) return -1;
    g_dbgfont.pos_attrib = pa->index;
    g_dbgfont.col_attrib = ca->index;
    g_dbgfont.uv_attrib  = ta->index;
    g_dbgfont.tex_unit   = tu->index;

    g_dbgfont.initialized = 1;
    return 0;
}

int32_t cellDbgFontExitGcm(void)
{
    g_dbgfont.initialized = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/*   Glyph quad builder                                               */
/* ------------------------------------------------------------------ */

/* Append one glyph quad (4 verts: TL, TR, BR, BL) at NDC top-left
 * (nx, ny) with per-vertex color `col`.  Returns 0 on success, -1 if
 * the vertex pool is full.  The winding matches CELL_GCM_PRIMITIVE_QUADS
 * which NV40 consumes as TL→TR→BR→BL. */
static int append_glyph(float nx_tl, float ny_tl, float scale,
                        uint8_t ch, const DbgColor *col)
{
    if (g_dbgfont.num_verts + 4 > g_dbgfont.max_verts) return -1;

    float gw = DBGFONT_GLYPH_NDC_W * scale;
    float gh = DBGFONT_GLYPH_NDC_H * scale;
    float nx_br = nx_tl + gw;
    float ny_br = ny_tl - gh;

    float s0 = glyph_s0(ch), s1 = glyph_s1(ch);
    float t0 = glyph_t0(ch), t1 = glyph_t1(ch);

    int i = g_dbgfont.num_verts;
    g_dbgfont.pos_cpu[i + 0] = (DbgPos){ nx_tl, ny_tl, 0.0f };
    g_dbgfont.uv_cpu [i + 0] = (DbgUV ){ s0, t0 };
    g_dbgfont.col_cpu[i + 0] = *col;

    g_dbgfont.pos_cpu[i + 1] = (DbgPos){ nx_br, ny_tl, 0.0f };
    g_dbgfont.uv_cpu [i + 1] = (DbgUV ){ s1, t0 };
    g_dbgfont.col_cpu[i + 1] = *col;

    g_dbgfont.pos_cpu[i + 2] = (DbgPos){ nx_br, ny_br, 0.0f };
    g_dbgfont.uv_cpu [i + 2] = (DbgUV ){ s1, t1 };
    g_dbgfont.col_cpu[i + 2] = *col;

    g_dbgfont.pos_cpu[i + 3] = (DbgPos){ nx_tl, ny_br, 0.0f };
    g_dbgfont.uv_cpu [i + 3] = (DbgUV ){ s0, t1 };
    g_dbgfont.col_cpu[i + 3] = *col;

    g_dbgfont.num_verts = i + 4;
    return 0;
}

/* ------------------------------------------------------------------ */
/*   String staging                                                   */
/* ------------------------------------------------------------------ */

int32_t cellDbgFontPuts(float x, float y, float scale, uint32_t color, const char *s)
{
    if (!g_dbgfont.initialized || !s) return 0;

    DbgColor col;
    unpack_argb(color, &col);

    /* Convert normalized [0..1] anchor to NDC [-1..+1] with Y-down
     * screen convention (y=0 is top).  Subsequent glyph positions
     * are in NDC and track via cursor offsets. */
    float cursor_x_ndc = 2.0f * x - 1.0f;
    float cursor_y_ndc = 1.0f - 2.0f * y;
    float origin_x_ndc = cursor_x_ndc;

    float kern_w = DBGFONT_KERN_NDC_W * scale;
    float line_h = DBGFONT_LINE_NDC_H * scale;
    float tab_w  = kern_w * DBGFONT_TAB_SIZE;

    int written = 0;
    for (const char *p = s; *p; p++) {
        char ch = *p;
        if (is_printable(ch)) {
            if (append_glyph(cursor_x_ndc, cursor_y_ndc, scale, (uint8_t)ch, &col) != 0)
                break;  /* vertex pool exhausted */
            cursor_x_ndc += kern_w;
            written++;
        } else if (ch == '\n') {
            cursor_x_ndc = origin_x_ndc;
            cursor_y_ndc -= line_h;
        } else if (ch == '\t') {
            cursor_x_ndc += tab_w;
        } else {
            /* Advance blanks (space and anything else non-printable) so
             * layout still makes sense — matches PSL1GHT's fallthrough. */
            cursor_x_ndc += kern_w;
        }
    }
    return written;
}

int32_t cellDbgFontPrintf(float x, float y, float scale, uint32_t color, const char *fmt, ...)
{
    if (!g_dbgfont.initialized || !fmt) return 0;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (m < 0) return 0;
    return cellDbgFontPuts(x, y, scale, color, buf);
}

/* ------------------------------------------------------------------ */
/*   FIFO submit                                                      */
/* ------------------------------------------------------------------ */

int32_t cellDbgFontDrawGcm(void)
{
    if (!g_dbgfont.initialized) return -1;
    if (g_dbgfont.num_verts == 0) return 0;

    gcmContextData *ctx = gCellGcmCurrentContext;
    if (!ctx) return -1;

    /* Overlay render state: alpha-blend on, depth-test off, no logic
     * op, no depth write.  Matches the PSL1GHT reference renderer. */
    rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(ctx, GCM_TRUE);
    rsxSetLogicOpEnable(ctx, GCM_FALSE);
    rsxSetDepthTestEnable(ctx, GCM_FALSE);

    /* Bind the vertex + fragment shaders. */
    rsxLoadVertexProgram(ctx, g_dbgfont.vp, g_dbgfont.vp_ucode);
    rsxLoadFragmentProgramLocation(ctx, g_dbgfont.fp,
                                   g_dbgfont.fp_ucode_off,
                                   GCM_LOCATION_RSX);

    /* Describe the font atlas as a 2D 8-bit alpha texture.  The
     * fragment shader samples it and uses .w as the alpha-kill
     * coverage, so we remap all four destination channels onto the
     * B source channel (which, for _FORMAT_B8, carries the single
     * byte of the texel). */
    gcmTexture tex = {
        .format    = GCM_TEXTURE_FORMAT_B8 | GCM_TEXTURE_FORMAT_LIN,
        .mipmap    = 1,
        .dimension = GCM_TEXTURE_DIMS_2D,
        .cubemap   = GCM_FALSE,
        .remap     = GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT |
                     GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT |
                     GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT |
                     GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT |
                     GCM_TEXTURE_REMAP_COLOR_B    << GCM_TEXTURE_REMAP_COLOR_B_SHIFT |
                     GCM_TEXTURE_REMAP_COLOR_B    << GCM_TEXTURE_REMAP_COLOR_G_SHIFT |
                     GCM_TEXTURE_REMAP_COLOR_B    << GCM_TEXTURE_REMAP_COLOR_R_SHIFT |
                     GCM_TEXTURE_REMAP_COLOR_B    << GCM_TEXTURE_REMAP_COLOR_A_SHIFT,
        .width     = DBGFONT_ATLAS_W,
        .height    = DBGFONT_ATLAS_H,
        .depth     = 1,
        .pitch     = DBGFONT_ATLAS_W,
        .location  = GCM_LOCATION_RSX,
        .offset    = g_dbgfont.atlas_off,
    };
    rsxLoadTexture(ctx, g_dbgfont.tex_unit, &tex);
    rsxTextureControl(ctx, g_dbgfont.tex_unit, GCM_TRUE, 0 << 8, 12 << 8,
                      GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(ctx, g_dbgfont.tex_unit, 0,
                     GCM_TEXTURE_NEAREST_MIPMAP_LINEAR,
                     GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(ctx, g_dbgfont.tex_unit,
                       GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT,
                       GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                       GCM_TEXTURE_ZFUNC_LESS, 0);

    /* Bind the vertex-attribute streams — each attribute reads from a
     * contiguous, tightly-packed array at its own RSX IO offset. */
    rsxBindVertexArrayAttrib(ctx, g_dbgfont.pos_attrib, 0, g_dbgfont.pos_off,
                             sizeof(DbgPos), 3, GCM_VERTEX_DATA_TYPE_F32,
                             GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, g_dbgfont.uv_attrib, 0, g_dbgfont.uv_off,
                             sizeof(DbgUV), 2, GCM_VERTEX_DATA_TYPE_F32,
                             GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(ctx, g_dbgfont.col_attrib, 0, g_dbgfont.col_off,
                             sizeof(DbgColor), 4, GCM_VERTEX_DATA_TYPE_F32,
                             GCM_LOCATION_RSX);

    rsxDrawVertexArray(ctx, GCM_TYPE_QUADS, 0, (uint32_t)g_dbgfont.num_verts);
    rsxInvalidateVertexCache(ctx);

    /* Reset for next frame.  We don't wait here — the caller's flip
     * path handles GPU/PPU synchronisation.  Next frame's Puts/Printf
     * calls overwrite the vertex pool in place. */
    g_dbgfont.num_verts = 0;
    return 0;
}
