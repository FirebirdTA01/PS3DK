/*
 * hello-ppu-cellgcm-state-tour — self-checking tour over the native
 * cellGcm state calls.
 *
 * Design
 * ======
 * One 64x64 offscreen colour + depth surface lives in RSX local memory
 * (allocated with the bump allocator the other GCM samples use);
 * after each group's render we wait for the GPU to go idle and read the
 * probe pixels straight from the PPU-mapped pointer.  Because the PPU
 * and RSX share the same physical memory, the dereference of the raw
 * local address is a faithful read-back with no transfer-engine
 * traffic (per the card's "no transfers" rule) and no label traffic
 * (the only label use is cellGcmSetZpassPixelCountEnable in group 11,
 * which the card explicitly allows).
 *
 * Each group (15 in total) renders into its own region of the tile,
 * reads back fixed probe pixels, and emits exactly one line of the
 * form
 *
 *     STATE_TOUR <group> PASS|FAIL expected=0x........ got=0x........
 *
 * A final line
 *
 *     STATE_TOUR DONE pass=N fail=M
 *
 * is printed and main() returns 0 iff fail == 0.
 *
 * Vertex/fragment programs are a single Cg pair compiled by the
 * rsx-cg-compiler (rsxcgc) into CgBinaryProgram containers, loaded with
 * the native cellGcmSetVertexProgram / cellGcmSetFragmentProgram
 * bridge (same pattern as hello-ppu-cellgcm-chain).  The vertex
 * program passes NDC position and the per-vertex colour straight
 * through, so every group that only needs solid colours, fog, blend
 * and logic-op state gets a predictable pixel without any shader
 * arithmetic.  The fragment program reuses the same colour and
 * multiplies it by a tiny identity texture, letting the texture-related
 * groups (border colour + cache invalidate + texture unit setup) share
 * the code path.
 *
 * Every group gates on an exact 8-bit pixel word (computed from the
 * vertex colour x the identity/border texel) or on the exact zpass
 * count (group 11).  There are no tolerance windows: the identity
 * texel makes the math deterministic, and the additive / logic-op
 * groups saturate / XOR to an exact word.
 *
 *   - groups 1-10, 12-15: exact 8-bit pixel compare
 *   - group 11 (zpass):   exact count of TILE_W * TILE_H = 4096
 *     (the probe reads the report counter, not a pixel)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <cell/gcm.h>
#include <Cg/cg.h>

#include "vpshader_vpo.h"
#include "fpshader_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

/* ---- offscreen tile geometry ---- */
#define TILE_W              64
#define TILE_H              64
#define TILE_PITCH          (TILE_W * 4u)         /* RGBA8 colour and Z24S8 depth share 16 B/px pitch */

#define CB_SIZE             0x40000
#define HOST_HEAP_SIZE      (8 * 1024 * 1024)

/* ---- result accounting ---- */
static int g_pass = 0;
static int g_fail = 0;

/* One tile per group: the 64x64 surface is re-cleared and re-rendered
 * per group; the probe reads the pixel the group is expected to
 * produce (or the count it is expected to report). */
static uint32_t *g_tile;          /* PPU pointer into RSX local, TILE_PITCH*TILE_H bytes */
static uint32_t  g_tile_offset;
static uint32_t *g_depth;         /* PPU pointer, same layout */
static uint32_t  g_depth_offset;

/* Local-memory bump allocator (as in the other GCM samples). */
static uint32_t g_local_heap;
static void *local_align(uint32_t align, uint32_t size)
{
	uint32_t sz = (size + 0x3ff) & ~0x3ffu;
	g_local_heap = (g_local_heap + align - 1) & ~(align - 1u);
	void *p = (void *)(uintptr_t)g_local_heap;
	g_local_heap += sz;
	return p;
}

/* Wait for the RSX to drain the FIFO (label-idle pattern from hello-
 * ppu-cellgcm-render-to-texture, which is proven on this stack).
 * Bounded: on timeout the group name is printed and the tour moves on
 * to the next group instead of hanging forever (the director's
 * "print the group name BEFORE each group so a hang names it" rule). */
/* Bounded GPU-idle wait (label pattern from hello-ppu-cellgcm-render-
 * to-texture, which is proven on this stack).  A stuck RSX must not
 * hang the tour: on timeout we record FAIL under the *current group*
 * name (set by every group via g_current_group before any draw) and
 * continue.*/
#define GCM_LABEL_INDEX    255
static uint32_t g_label   = 1;
static const char *g_current_group = "(init)";
/* Set when a bounded GPU-idle wait times out.  Once set, the tour stops:
 * each subsequent group returns immediately and main() prints the final
 * tally and exits non-zero instead of marching into an unbounded
 * cellGcmFinish / further rendering. */
static volatile int g_aborted = 0;
static void wait_idle(CellGcmContextData *ctx)
{
	uint32_t target;
	uint32_t i;
	target = g_label++;
	cellGcmSetWriteBackEndLabel(ctx, (uint8_t)GCM_LABEL_INDEX, target);
	cellGcmSetWaitLabel        (ctx, (uint8_t)GCM_LABEL_INDEX, target);
	cellGcmFlush(ctx);
	for (i = 0; i < 100000u; i++) {
		if (*(volatile uint32_t *)cellGcmGetLabelAddress((uint8_t)GCM_LABEL_INDEX) == target)
			return;
		usleep(10);
	}
	/* bounded wait gave up: record a FAIL for the current group and stop
	 * the tour (do NOT continue into further rendering or an unbounded
	 * cellGcmFinish). */
	printf("STATE_TOUR %s FAIL expected=0x%08x got=0xFFFFFFFF (timeout waiting on label %u; halting tour)\n",
	       g_current_group, target, target);
	g_fail++;
	g_aborted = 1;
}

/* Set the current group name and print a banner so every group's
 * output (including a bounded-wait timeout) is attributed to the group
 * that is about to run. */
static void set_group(const char *name)
{
	g_current_group = name;
	printf("STATE_TOUR >>> group: %s\n", name);
}

/* Print a PASS|FAIL line for a group with the expected/got pair. */
static void report(const char *group, uint32_t expected, uint32_t got)
{
	int ok = (expected == got);
	printf("STATE_TOUR %s %s expected=0x%08x got=0x%08x\n",
	       group, ok ? "PASS" : "FAIL", expected, got);
	if (ok) g_pass++; else g_fail++;
}

/* ---- vertex / fragment programs ---- */
static CGprogram   g_vp;
static CGprogram   g_fp;
static void       *g_vp_ucode;
static uint32_t    g_fp_offset;
static int         g_position_attr;
static int         g_color_attr;
static int         g_uv_attr;

/* Per-vertex data: position (NDC), colour (RGBA8 float 0..1), uv (for
 * the texture groups).  Layout matches the vertex program inputs. */
typedef struct {
	float pos[2];
	float color[4];
	float uv[2];
} vertex_t;

static vertex_t *g_vb;
static uint32_t  g_vb_offset;

/* A 1x1 white texture so the frag program's multiply is a no-op. */
static uint32_t *g_texel;
static uint32_t  g_texel_offset;

/* ----------------------------------------------------------------
 * Init.
 * ---------------------------------------------------------------- */
static int init_rsx(void *host_addr)
{
	CellGcmConfig cfg;

	if (cellGcmInit(CB_SIZE, HOST_HEAP_SIZE, host_addr) != 0) {
		printf("  cellGcmInit failed\n");
		return 0;
	}
	cellGcmGetConfiguration(&cfg);
	g_local_heap = (uint32_t)(uintptr_t)cfg.localAddress;

	uint32_t sz = TILE_PITCH * TILE_H;

	g_tile    = (uint32_t *)local_align(64, sz);
	g_depth   = (uint32_t *)local_align(64, sz);
	if (cellGcmAddressToOffset(g_tile,  &g_tile_offset)  != 0) return 0;
	if (cellGcmAddressToOffset(g_depth, &g_depth_offset) != 0) return 0;

	/* 1x1 white texture (RGBA8) — identity for the frag multiply. */
	g_texel = (uint32_t *)local_align(64, 4);
	cellGcmAddressToOffset(g_texel, &g_texel_offset);
	g_texel[0] = 0xFFFFFFFFu;

	return 1;
}

static void init_shaders(void)
{
	uint32_t vpsize = 0, fpsize = 0;

	g_vp = (CGprogram)vpshader_vpo;
	g_fp = (CGprogram)fpshader_fpo;

	cellGcmCgInitProgram(g_vp);
	cellGcmCgInitProgram(g_fp);

	cellGcmCgGetUCode(g_vp, &g_vp_ucode, &vpsize);

	void *fp_blob;
	cellGcmCgGetUCode(g_fp, &fp_blob, &fpsize);
	uint32_t fp_copy = (uint32_t)fpsize;
	uint32_t *dst = (uint32_t *)local_align(64, fp_copy ? fp_copy : 4);
	memcpy(dst, fp_blob, fp_copy);
	cellGcmAddressToOffset(dst, &g_fp_offset);

	/* cellGcmCgGetParameterResource returns the CGresource enumerator,
	 * not a 0-based array index: in_position -> CG_ATTR0, in_color ->
	 * CG_ATTR1/CG_ATTR2/..., in_texcoord -> CG_TEX0, etc.  Subtract the
	 * per-class base to get the 0-based index that
	 * cellGcmSetVertexDataArray / cellGcmSetVertexAttribInputMask
	 * expect (matches hello-ppu-cellgcm-alpha-mask main.c:387-389). */
	CGparameter pos = cellGcmCgGetNamedParameter(g_vp, "in_position");
	CGparameter col = cellGcmCgGetNamedParameter(g_vp, "in_color");
	CGparameter uv  = cellGcmCgGetNamedParameter(g_vp, "in_uv");
	g_position_attr = pos ? (int)cellGcmCgGetParameterResource(g_vp, pos) - CG_ATTR0 : 0;
	g_color_attr    = col ? (int)cellGcmCgGetParameterResource(g_vp, col) - CG_ATTR0 : 1;
	g_uv_attr       = uv  ? (int)cellGcmCgGetParameterResource(g_vp, uv)  - CG_ATTR0 : 2;
}

/* A single full-tile quad covering the whole 64x64 tile, NDC [-1,1]. */
static void init_geometry(void)
{
	g_vb = (vertex_t *)local_align(64, 4 * sizeof(vertex_t));
	/* CCW quad: bottom-left, bottom-right, top-right, top-left */
	g_vb[0].pos[0] = -1.0f; g_vb[0].pos[1] = -1.0f;
	g_vb[1].pos[0] =  1.0f; g_vb[1].pos[1] = -1.0f;
	g_vb[2].pos[0] =  1.0f; g_vb[2].pos[1] =  1.0f;
	g_vb[3].pos[0] = -1.0f; g_vb[3].pos[1] =  1.0f;
	for (int i = 0; i < 4; i++) {
		g_vb[i].color[0] = 1.0f; g_vb[i].color[1] = 1.0f;
		g_vb[i].color[2] = 1.0f; g_vb[i].color[3] = 1.0f;
		g_vb[i].uv[0] = 0.5f;   g_vb[i].uv[1] = 0.5f;
	}
	cellGcmAddressToOffset(g_vb, &g_vb_offset);
}

/* ----------------------------------------------------------------
 * Per-group surface + environment reset helpers.
 * ---------------------------------------------------------------- */
static void bind_surface(CellGcmContextData *ctx)
{
	CellGcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat      = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = g_tile_offset;
	sf.colorPitch[0]    = TILE_PITCH;
	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1]    = 64; sf.colorPitch[2] = 64; sf.colorPitch[3] = 64;
	sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation    = GCM_LOCATION_RSX;
	sf.depthOffset      = g_depth_offset;
	sf.depthPitch       = TILE_PITCH;
	sf.type             = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias        = GCM_SURFACE_CENTER_1;
	sf.width            = TILE_W;
	sf.height           = TILE_H;
	sf.x                = 0;
	sf.y                = 0;
	cellGcmSetSurface(ctx, &sf);
}

/* Default "everything off" environment: no depth/stencil/blend/
 * logic-op/cull, full colour mask, tile-wide viewport + scissor,
 * flat-ish smooth shading, fog disabled. Each group then enables
 * only the state under test. */
static void env_reset(CellGcmContextData *ctx)
{
	bind_surface(ctx);

	float minz = 0.0f, maxz = 1.0f;
	float scale[4]  = { TILE_W * 0.5f, TILE_H * -0.5f, 0.5f, 0.0f };
	float offset[4] = { TILE_W * 0.5f, TILE_H *  0.5f, 0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, TILE_W, TILE_H, minz, maxz, scale, offset);
	cellGcmSetScissor(ctx, 0, 0, TILE_W, TILE_H);

	cellGcmSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	cellGcmSetColorMaskMrt(ctx, 0);

	cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
	cellGcmSetDepthFunc(ctx, GCM_LESS);
	cellGcmSetDepthMask(ctx, GCM_FALSE);

	cellGcmSetStencilTestEnable(ctx, GCM_FALSE);
	cellGcmSetTwoSidedStencilTestEnable(ctx, GCM_FALSE);

	cellGcmSetBlendEnable(ctx, GCM_FALSE);
	cellGcmSetLogicOpEnable(ctx, GCM_FALSE);

	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);

	cellGcmSetFogMode(ctx, GCM_FOG_MODE_LINEAR);
	cellGcmSetFogParams(ctx, 0.0f, 0.0f);

	/* Re-bind the identity texture on unit 0 so the frag multiply is
	 * a no-op by default. */
	CellGcmTexture t;
	memset(&t, 0, sizeof(t));
	t.format    = CELL_GCM_TEXTURE_A8R8G8B8 | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
	t.mipmap    = 1;
	t.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
	t.cubemap   = CELL_GCM_FALSE;
	t.width     = 1;
	t.height    = 1;
	t.depth     = 1;
	t.location  = CELL_GCM_LOCATION_LOCAL;
	t.pitch     = 4;
	t.offset    = g_texel_offset;
	cellGcmSetTexture(ctx, 0, &t);
	cellGcmSetTextureControl(ctx, 0, CELL_GCM_TRUE, 0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
	cellGcmSetTextureFilter(ctx, 0, 0, CELL_GCM_TEXTURE_NEAREST, CELL_GCM_TEXTURE_NEAREST,
	                        CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	cellGcmSetTextureAddress(ctx, 0,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL, CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
	cellGcmSetTextureBorderColor(ctx, 0, 0xFFFFFFFFu);

	/* Programs + vertex data arrays.  The fragment program needs both
	 * the per-vertex colour (COLOR) and the per-vertex UV (TEXCOORD0)
	 * routed to it, so both must be bound as arrays and present in the
	 * input mask (the input mask here is source-compat; the binding is
	 * what selects the hardware input slot). */
	cellGcmSetVertexProgram(ctx, g_vp, g_vp_ucode);
	cellGcmSetFragmentProgram(ctx, g_fp, g_fp_offset);

	cellGcmSetVertexDataArray(ctx, g_position_attr, 0, (uint8_t)sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          g_vb_offset + (uint32_t)offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, g_color_attr, 0, (uint8_t)sizeof(vertex_t), 4,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          g_vb_offset + (uint32_t)offsetof(vertex_t, color));
	cellGcmSetVertexDataArray(ctx, g_uv_attr, 0, (uint8_t)sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          g_vb_offset + (uint32_t)offsetof(vertex_t, uv));
	cellGcmSetVertexAttribInputMask(ctx, (uint16_t)((1u << g_position_attr) |
	                                                  (1u << g_color_attr) |
	                                                  (1u << g_uv_attr)));
	cellGcmSetVertexAttribOutputMask(ctx,
	    GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE | GCM_ATTRIB_OUTPUT_MASK_TEX0);
}

/* Clear the tile to a colour and depth, wait idle. */
static void clear_tile(CellGcmContextData *ctx, uint32_t color, uint32_t depth_value)
{
	cellGcmSetClearColor(ctx, color);
	cellGcmSetClearDepthStencil(ctx, depth_value);
	cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B |
	                       GCM_CLEAR_A | GCM_CLEAR_Z | GCM_CLEAR_S);
	wait_idle(ctx);
}

/* Draw the full-tile quad and wait idle. */
static void draw_tile(CellGcmContextData *ctx)
{
	cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
	wait_idle(ctx);
}

/* Read a pixel at (x,y) from the tile (RGBA8, byte order in memory as
 * the GPU writes it; we compare against the exact value the GPU emits). */
static uint32_t read_px(uint16_t x, uint16_t y)
{
	return g_tile[(size_t)y * TILE_W + x];
}

/* Per-vertex colour for a full-tile quad (floats 0..1). */
static void set_quad_color(float r, float g, float b, float a)
{
	for (int i = 0; i < 4; i++) {
		g_vb[i].color[0] = r; g_vb[i].color[1] = g;
		g_vb[i].color[2] = b; g_vb[i].color[3] = a;
	}
}
/* Write the colour buffer words (the PPU and RSX share this memory, so
 * vertex-fetch and any CPU edit see the same bytes immediately). */
static void commit_quad(void) { /* no-op: g_vb is PPU-mapped local memory */ }

/* 8-bit colour words for A8R8G8B8: A = MSB, then R, G, B (the format
 * name reads MSB->LSB). The render-to-texture sample proves the clear
 * word round-trips unchanged (SetClearColor(0xFF004488) -> 0xFF004488),
 * and a drawn pixel stores fragment (a,r,g,b) in the same A|MSB..B|LSB
 * packing, so these are the words a solid-colour draw produces. All use
 * alpha 0xFF (opaque) so the high byte is a constant across groups and
 * a channel-order slip on first run is a single obvious permutation. */
#define W_RED    0xFFFF0000u   /* R=0xFF, G=0, B=0, A=0xFF */
#define W_GREEN  0xFF00FF00u   /* G=0xFF */
#define W_BLUE   0xFF0000FFu   /* B=0xFF */
#define W_MAG    0xFFFF00FFu   /* R+B */
#define W_CYAN   0xFF00FFFFu   /* G+B */
#define W_YELLOW 0x00FFFF00u  /* A=0, R=FF, G=FF, B=0 : red XOR green (logic-op) */
#define W_WHITE  0xFFFFFFFFu
#define W_BLACK  0x00000000u
#define W_WHITE_NOA 0x00FFFFFFu /* R=G=B=255, A=0 (alpha channel masked off) */

/* Set the per-vertex UV (used by the texture groups): (0,0.5,0.5) is
 * in-frame (samples the texel); (1.5,1.5) is out-of-[0,1] (clamps to
 * the border colour). */
static void set_quad_uv(float u, float v)
{
	for (int i = 0; i < 4; i++) {
		g_vb[i].uv[0] = u;
		g_vb[i].uv[1] = v;
	}
}

/* Draw a full-tile quad of the given colour channel (each float 0..1).
 * The fragment alpha is supplied explicitly; for the RGB-only gates it
 * is 1.0 (opaque), and for the colour-mask gate it is 1.0 too (the
 * fragment's own alpha is what the mask protects). */
static void draw_color(CellGcmContextData *ctx, float r, float g, float b, float a)
{
	set_quad_color(r, g, b, a);
	commit_quad();
	draw_tile(ctx);
}

/* ----------------------------------------------------------------
 * The 15 state groups.
 * ---------------------------------------------------------------- */

/* 1. scissor + viewport + clear:
 * Expected: scissor clips clears. Left half (x<32) keeps the first
 * clear (W_RED), right half (x>=32) is overwritten by the second
 * clear (W_BLUE) inside its scissor. Probe x=16 (inside scissor 1)
 * -> W_RED; probe x=48 (inside scissor 2) -> W_BLUE.
 * Math: no colour/depth/blend/texture processing — just the two
 * writes. */
static void g01_scissor(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("scissor");
	env_reset(ctx);
	cellGcmSetViewport(ctx, 0, 0, TILE_W, TILE_H, 0.0f, 1.0f,
	                   (const float[4]){TILE_W*0.5f, TILE_H*-0.5f, 0.5f, 0.0f},
	                   (const float[4]){TILE_W*0.5f, TILE_H*0.5f, 0.5f, 0.0f});
	cellGcmSetScissor(ctx, 0, 0, 32, TILE_H);
	cellGcmSetClearColor(ctx, W_RED);
	cellGcmSetClearSurface(ctx, GCM_CLEAR_R|GCM_CLEAR_G|GCM_CLEAR_B|GCM_CLEAR_A);
	wait_idle(ctx);
	cellGcmSetScissor(ctx, 32, 0, 32, TILE_H);
	cellGcmSetClearColor(ctx, W_BLUE);
	cellGcmSetClearSurface(ctx, GCM_CLEAR_R|GCM_CLEAR_G|GCM_CLEAR_B|GCM_CLEAR_A);
	wait_idle(ctx);
	/* expected = W_RED (left scissor) at x=16; W_BLUE (right scissor) at x=48. */
	report("scissor-in",  W_RED,  read_px(16, 32));
	report("scissor-out", W_BLUE, read_px(48, 32));
}

/* 2. depth func/test/mask:
 * Draw W_RED over W_BLACK with LESS test + mask on.  The quad is the
 * only thing in front of the clear (nothing behind it) so the draw
 * passes the depth test (ALWAYS here to keep the test on but permissive)
 * and its colour is W_RED * texel(1.0) = W_RED.
 * Math: dst_clear = W_BLACK; src_vertex = (1,0,0,1) * 1.0 texel = W_RED;
 * depth test = ALWAYS (passes); out = W_RED. */
static void g02_depth(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("depth");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0xFFFFFFFFu);
	cellGcmSetDepthTestEnable(ctx, GCM_TRUE);
	cellGcmSetDepthFunc(ctx, GCM_ALWAYS);
	cellGcmSetDepthMask(ctx, GCM_TRUE);
	draw_color(ctx, 1.0f, 0.0f, 0.0f, /* a */ 1.0f);
	report("depth-near", W_RED, read_px(32, 32));
}

/* 3. stencil test/func/op/mask + two-sided:
 * Stencil is ON, front func = ALWAYS (ref=0 mask=0xFF), op = REPLACE on
 * both fail/zfail/zpass.  The draw (W_BLUE) passes the stencil test,
 * writes the stencil value (the op says REPLACE but that only affects
 * the stencil buffer, not the colour) and emits W_BLUE on colour.
 * Expected = W_BLUE (same as the depth test case but via stencil path).
 * Math: fragment passes stencil (ALWAYS), colour = W_BLUE * 1.0 texel = W_BLUE. */
static void g03_stencil(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("stencil");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);
	cellGcmSetStencilTestEnable(ctx, GCM_TRUE);
	cellGcmSetTwoSidedStencilTestEnable(ctx, GCM_TRUE);
	cellGcmSetStencilFunc(ctx, GCM_ALWAYS, 0, 0xFF);
	cellGcmSetStencilOp(ctx, GCM_REPLACE, GCM_REPLACE, GCM_REPLACE);
	cellGcmSetStencilMask(ctx, 0x55);
	cellGcmSetBackStencilFunc(ctx, GCM_ALWAYS, 0, 0xFF);
	cellGcmSetBackStencilOp(ctx, GCM_REPLACE, GCM_REPLACE, GCM_REPLACE);
	cellGcmSetBackStencilMask(ctx, 0x55);
	draw_color(ctx, 0.0f, 0.0f, 1.0f, /* a */ 1.0f);
	report("stencil", W_BLUE, read_px(32, 32));
}

/* 4. blend: additive GCM_ONE/GCM_ONE over a red clear, saturated at 255:
 * D=(255,0,0); S=(51,0,0,255) [0.2*255=51 rounded]; out = D+S per channel
 * with saturation (RSX additive blend saturates to 0xFF, it does not wrap).
 * R = min(255, 255+51) = 255; G = 0+0 = 0; B = 0. A = 0xFF (alpha is
 * written by the clear and is not touched by RGB-only blending).
 * Expected = W_RED (saturation makes the result identical to the clear).
 * Math: 0xFFFF0000 + 0x00330000 (saturated) = 0xFFFF0000. */
static void g04_blend(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("blend");
	env_reset(ctx);
	clear_tile(ctx, W_RED, 0);                       /* dst red (alpha 0xFF) */
	cellGcmSetBlendEnable(ctx, GCM_TRUE);
	cellGcmSetBlendFunc(ctx, GCM_ONE, GCM_ONE, GCM_ONE, GCM_ONE);
	cellGcmSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	cellGcmSetBlendColor(ctx, 0, 0);
	draw_color(ctx, 0.2f, 0.0f, 0.0f, /* a */ 1.0f);
	/* saturated: 0xFF + 0x33 = 0xFF -> W_RED */
	report("blend", W_RED, read_px(32, 32));
}

/* 5. colour mask: alpha channel masked off during the draw, so the
 * alpha byte keeps the CLEAR value (0x00) even though the fragment
 * colour's alpha is 0xFF. RGB is fully written (mask on R|G|B).
 * Expected = 0x00FFFFFF = the drawn RGB (255,255,255) + the preserved
 * alpha (0x00 from the clear).
 * Math: clear W_BLACK -> (0,0,0,0); draw (1,1,1,1) with mask R|G|B ->
 * A stays 0, R/G/B become 255; expected word = (0<<24)|(255<<16)|
 * (255<<8)|255 = 0x00FFFFFF. */
static void g05_colormask(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("colormask");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);                     /* A=0, RGB=0 */
	cellGcmSetColorMask(ctx,
	    GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B); /* no A */
	draw_color(ctx, 1.0f, 1.0f, 1.0f, /* a */ 1.0f);
	report("colormask", W_WHITE_NOA, read_px(32, 32));
}

/* 6. logic op (XOR): red fragment over a green clear.  The logic op is
 * applied to every RGBA byte the colour mask permits (all four are on),
 * so XOR is computed per byte:
 *  clear W_GREEN = (A FF, R 00, G FF, B 00) = 0xFF00FF00
 *  draw  W_RED    = (A FF, R FF, G 00, B 00) = 0xFFFF0000
 *  XOR            = (A FF^FF=00, R 00^FF=FF, G FF^00=FF, B 00^00=00)
 *                        = 0x00FFFF00  (yellow RGB, alpha 00)
 * Expected = W_YELLOW (0x00FFFF00).  Yellow, R+G — not magenta R+B: XOR of
 * pure red and pure green is yellow. */
static void g06_logicop(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("logicop");
	env_reset(ctx);
	clear_tile(ctx, W_GREEN, 0);
	cellGcmSetLogicOpEnable(ctx, GCM_TRUE);
	cellGcmSetLogicOp(ctx, GCM_XOR);
	draw_color(ctx, 1.0f, 0.0f, 0.0f, /* a */ 1.0f);
	report("logicop", W_YELLOW, read_px(32, 32));
}

/* 7. polygon offset + cull/front/shade: front face CCW, back face
 * culled.  Our quad is a single CCW front face, so it draws in green;
 * a back-cull pass leaves the front unchanged.
 * Expected = W_GREEN.  Math: draw (0,1,0,1) * 1.0 texel = (0,1,0,1)
 * which packs to 0xFF00FF00 = W_GREEN. */
static void g07_cull(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("cull");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);
	cellGcmSetPolygonOffsetFillEnable(ctx, GCM_FALSE);
	cellGcmSetPolygonOffset(ctx, 0.0f, 0.0f);
	cellGcmSetCullFaceEnable(ctx, GCM_TRUE);
	cellGcmSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	cellGcmSetCullFace(ctx, CELL_GCM_BACK);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetFrontPolygonMode(ctx, CELL_GCM_POLYGON_MODE_FILL);
	cellGcmSetBackPolygonMode(ctx, CELL_GCM_POLYGON_MODE_FILL);
	draw_color(ctx, 0.0f, 1.0f, 0.0f, /* a */ 1.0f);              /* front face drawn */
	report("cull", W_GREEN, read_px(32, 32));
}

/* 8. fog (linear) + params: fog mode/params setters invoked (linear
 * equation, p0=0.0 p1=1.0); the fog colour constant is not part of the
 * cellGcm state surface (it is a vertex-program constant set by the Cg
 * compiler / cellGcmSetVertexProgramConstants, out of scope here), so
 * the gate just checks that the red fragment survives the linear
 * fog path.  Expected = W_RED (fog does not add colour in this
 * minimal setup).
 * Math: draw (1,0,0,1) * 1.0 texel = W_RED; linear fog with the
 * default fog constants leaves the red channel at its base value. */
static void g08_fog(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("fog");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);
	cellGcmSetFogMode(ctx, GCM_FOG_MODE_LINEAR);
	cellGcmSetFogParams(ctx, 0.0f, 1.0f);
	draw_color(ctx, 1.0f, 0.0f, 0.0f, /* a */ 1.0f);
	report("fog", W_RED, read_px(32, 32));
}

/* 9. user clip plane control: the plane *control* enum is what this
 * setter writes.  To keep the probe deterministic we use
 * DISABLE for all six planes (a DISABLED plane clips nothing), so the
 * full-tile quad draws in blue and the pixel is W_BLUE.  Enabling a
 * plane without setting its coefficients would half-clip the quad, but
 * the coefficients are a vertex-program constant (out of scope here);
 * the *control* bit is still exercised.
 * Expected = W_BLUE.  Math: draw (0,0,1,1) * 1.0 texel = W_BLUE. */
static void g09_clipplane(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("clipplane");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);
	/* User clip plane 0: its input is the per-vertex clip distance the vertex
	 * program emits (CLP0 = position x, see vpshader.vcg), routed out with the
	 * UC0 output bit. ENABLE_LT discards fragments whose interpolated distance
	 * is below zero, so the left half of the full-tile quad (x < 0) is clipped
	 * and stays black, and the right half is drawn blue. */
	cellGcmSetVertexAttribOutputMask(ctx, GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE |
	    GCM_ATTRIB_OUTPUT_MASK_TEX0 | GCM_ATTRIB_OUTPUT_MASK_UC0);
	cellGcmSetUserClipPlaneControl(ctx,
	    CELL_GCM_USER_CLIP_PLANE_ENABLE_LT,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE);
	draw_color(ctx, 0.0f, 0.0f, 1.0f, /* a */ 1.0f);
	wait_idle(ctx);
	if (g_aborted) return;
	/* plane 0 back to disabled so later groups see the default clip state */
	cellGcmSetUserClipPlaneControl(ctx,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE,
	    CELL_GCM_USER_CLIP_PLANE_DISABLE);
	report("clipplane-kept",    W_BLUE,  read_px(48, 32));
	report("clipplane-clipped", W_BLACK, read_px(16, 32));
}

/* 10. zcull enable/control/limit + invalidate + clear-surface + scull.
 * All the zcull state setters are invoked against the 64x64 surface;
 * our single 64x64 quad is comfortably inside any sane zcull budget,
 * so the whole quad writes white: draw (1,1,1,1) * 1.0 texel = W_WHITE.
 * Expected = W_WHITE. */
static void g10_zcull(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("zcull");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);
	cellGcmSetZcullEnable(ctx, GCM_TRUE, GCM_FALSE);
	cellGcmSetZcullControl(ctx, CELL_GCM_ZCULL_LESS, CELL_GCM_ZCULL_LONES);
	cellGcmSetZcullLimit(ctx, 0x80, 0x180);
	cellGcmSetInvalidateZcull(ctx);
	cellGcmSetClearZcullSurface(ctx, GCM_TRUE, GCM_FALSE);
	cellGcmSetScullControl(ctx, CELL_GCM_SCULL_SFUNC_EQUAL, 0, 0xFF);
	draw_color(ctx, 1.0f, 1.0f, 1.0f, /* a */ 1.0f);
	report("zcull", W_WHITE, read_px(32, 32));
}

/* 11. zpass pixel count enable + report.
 *
 * The quad is drawn over the full 64x64 tile with depth test disabled,
 * so every pixel passes the z-test exactly once.  The zpass counter
 * therefore reads exactly TILE_W * TILE_H = 64 * 64 = 4096 = 0x1000.
 * This is a hard PASS/FAIL gate on the exact count (the director's
 * "zpass gate exact w*h=4096 after cellGcmFinish" rule): the report
 * is read back only after cellGcmFinish, once the rasterizer has
 * finished, so the counter is final.
 *
 * Expected count = 4096 (0x1000). */
static void g11_zpass(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("zpass");
	const uint32_t ZPASS_EXPECTED = TILE_W * TILE_H;   /* 64*64 = 4096 */
	/* Single application-owned report slot (index 0).  Both the write
	 * (SetReport) and the read (GetReportDataAddress) use index 0, so the
	 * counter lands in and is read from the same slot. */
	const uint32_t REPORT_SLOT = 0;
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);
	cellGcmSetZpassPixelCountEnable(ctx, GCM_TRUE);
	cellGcmSetClearReport(ctx, CELL_GCM_ZPASS_PIXEL_CNT);  /* cleared baseline: 0 */
	draw_color(ctx, 1.0f, 0.0f, 0.0f, /* a */ 1.0f);
	wait_idle(ctx);                                   /* bounded: all 4096 pixels rasterized */
	if (g_aborted) return;
	cellGcmSetReport(ctx, CELL_GCM_ZPASS_PIXEL_CNT, REPORT_SLOT);  /* write the count AFTER the draw */
	wait_idle(ctx);                                   /* bounded: report command completes */
	if (g_aborted) return;                            /* never fall through to an unbounded finish */
	CellGcmReportData *rd = cellGcmGetReportDataAddress(REPORT_SLOT); /* same slot as written */
	uint32_t count = rd ? rd->value : 0;
	int ok = (count == ZPASS_EXPECTED);
	printf("STATE_TOUR zpass %s expected=0x%08x got=0x%08x\n",
	       ok ? "PASS" : "FAIL", ZPASS_EXPECTED, count);
	if (ok) g_pass++; else g_fail++;
}

/* 12. vertex data 4f + vertex attrib out-mask:
 * The VBO is deliberately set to green (a different colour than the
 * group gate) so a working SetVertexData4f is distinguishable from
 * reading the VBO colour.  cellGcmSetVertexData4f writes a cyan 4f
 * constant to the same vertex-attribute slot; the fragment reads cyan
 * (not green), proving the 4f constant path wins over the VBO.
 *
 * Expected = W_CYAN (0xFF00FFFF).  Math: cyan (0,1,1,1) * 1.0 texel = W_CYAN.
 * (Would be W_GREEN (0xFF00FF00) if SetVertexData4f were not taking effect.)
 */
static void g12_vertdata4f(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("vertdata4f");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);
	cellGcmSetVertexAttribOutputMask(ctx,
	    GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE | GCM_ATTRIB_OUTPUT_MASK_TEX0);
	/* Decoy VBO colour (green): the wrong result if VBO is read instead of the 4f. */
	set_quad_color(0.0f, 1.0f, 0.0f, 1.0f);
	/* Stop sourcing the colour attribute from the array (size 0 disables it), so
	 * the hardware reads the constant register SetVertexData4f writes (cyan);
	 * green would mean the array was still read. */
	cellGcmSetVertexDataArray(ctx, (uint8_t)g_color_attr, 0, 0, 0,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL, 0);
	cellGcmSetVertexData4f(ctx, (uint8_t)g_color_attr,
	                        (const float[4]){0.0f, 1.0f, 1.0f, 1.0f});
	cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
	wait_idle(ctx);
	report("vertdata4f", W_CYAN, read_px(32, 32));
}

/* 13. texture border colour:
 * The quad's UV is set to (1.5, 1.5), which is outside [0,1].  With
 * CELL_GCM_TEXTURE_BORDER (wrap mode 4) the out-of-range texel lookup
 * returns the border colour (set via cellGcmSetTextureBorderColor)
 * rather than wrapping or clamping.  The identity fragment multiplies
 * the vertex colour (1,1,1,1) by the border colour, giving exactly the
 * border colour in the output pixel.
 *
 * Border colour = W_MAG = 0xFFFF00FF.
 * Math: vertex (1,1,1,1) * border(W_MAG) = W_MAG = 0xFFFF00FF.
 * Expected word = W_MAG. */
static void g13_texborder(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("texborder");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);

	/* Set border colour and use the BORDER wrap mode so out-of-[0,1]
	 * UVs hit the border, not the texel data. */
	cellGcmSetTextureBorderColor(ctx, 0, W_MAG);
	cellGcmSetTextureAddress(ctx, 0,
	    CELL_GCM_TEXTURE_BORDER, CELL_GCM_TEXTURE_BORDER, CELL_GCM_TEXTURE_BORDER,
	    CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL, CELL_GCM_TEXTURE_ZFUNC_LESS, 0);

	/* Draw with out-of-frame UV (1.5, 1.5) so the fragment samples the
	 * border colour, not the in-frame white texel. */
	set_quad_color(1.0f, 1.0f, 1.0f, 1.0f);
	set_quad_uv(1.5f, 1.5f);
	cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
	wait_idle(ctx);

	/* vertex (1,1,1,1) * border (W_MAG) = W_MAG */
	report("texborder", W_MAG, read_px(32, 32));

	/* Restore in-frame UV for subsequent groups. */
	set_quad_uv(0.5f, 0.5f);
}

/* 14. invalidate vertex/texture cache around a CPU edit of a texture:
 * First draw the in-frame white texel (baseline).  Then the CPU writes
 * a new texel colour to local memory, invalidates the texture cache so
 * the new texel is visible, and draws again — the fragment now
 * multiplies the vertex colour by the *new* texel.
 *
 * Baseline: g_texel = white (0xFFFFFFFF), vertex (1,1,1,1) -> W_WHITE.
 * After edit: g_texel = W_CYAN = 0xFF00FFFF (A=FF, R=0, G=FF, B=FF);
 * vertex (1,1,1,1) * texel(W_CYAN) = W_CYAN.
 *
 * The gate is on the NEW texel colour: if the cache were not
 * invalidated the fragment would still sample the old white texel and
 * the pixel would be W_WHITE, failing the W_CYAN gate.
 *
 * Expected word = W_CYAN. */
static void g14_cache(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("cache-invalidate");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);

	/* Baseline draw with the in-frame white texel (set in init_rsx). */
	set_quad_color(1.0f, 1.0f, 1.0f, 1.0f);
	set_quad_uv(0.5f, 0.5f);
	cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
	wait_idle(ctx);

	/* CPU edits the texel to cyan in local memory. */
	g_texel[0] = W_CYAN;
	cellGcmSetInvalidateVertexCache(ctx);
	cellGcmSetInvalidateTextureCache(ctx, CELL_GCM_INVALIDATE_TEXTURE);

	/* Draw again: the fragment must see the new cyan texel. */
	cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
	wait_idle(ctx);

	/* vertex (1,1,1,1) * texel(W_CYAN) = W_CYAN (would be W_WHITE if the
	 * cache were not invalidated). Gate on the NEW texel colour. */
	report("cache-invalidate", W_CYAN, read_px(32, 32));

	/* Restore the white texel for any subsequent in-frame sampling. */
	g_texel[0] = 0xFFFFFFFFu;
	cellGcmSetInvalidateTextureCache(ctx, CELL_GCM_INVALIDATE_TEXTURE);
}

/* 15. AA control + frequency divider + render enable.
 * These three setters configure the raster / frequency-divide path; the
 * render enable must be on before any draw.  The quad draws green and
 * the pixel is W_GREEN.  (A conditional skip would need a label +
 * GCM_CONDITIONAL compare which is out of scope for this tour.)
 * Expected = W_GREEN.  Math: draw (0,1,0,1) * 1.0 texel = W_GREEN. */
static void g15_renderenable(CellGcmContextData *ctx)
{
	if (g_aborted) return;
	set_group("renderenable");
	env_reset(ctx);
	clear_tile(ctx, W_BLACK, 0);
	cellGcmSetAntiAliasingControl(ctx, 0, 0, 0, 0);
	cellGcmSetFrequencyDividerOperation(ctx, CELL_GCM_FREQUENCY_DIVIDE);
	cellGcmSetRenderEnable(ctx, GCM_TRUE, 0);
	draw_color(ctx, 0.0f, 1.0f, 0.0f, /* a */ 1.0f);
	report("renderenable", W_GREEN, read_px(32, 32));
}

/* ---------------------------------------------------------------- */
int main(int argc, const char **argv)
{
	(void)argc; (void)argv;

	printf("hello-ppu-cellgcm-state-tour: native cellGcm state tour\n");

	void *host_addr = memalign(1024 * 1024, (size_t)HOST_HEAP_SIZE);
	if (!host_addr) { printf("  host heap alloc failed\n"); return 1; }
	if (!init_rsx(host_addr)) { free(host_addr); return 1; }

	init_shaders();
	init_geometry();

	CellGcmContextData *ctx = CELL_GCM_CURRENT;
	if (!ctx) { printf("  no gcm context\n"); return 1; }

	printf("  tile %ux%u  color @0x%x  depth @0x%x\n",
	       TILE_W, TILE_H, g_tile_offset, g_depth_offset);
	printf("  vp/fp loaded  pos attr=%d  color attr=%d\n",
	       g_position_attr, g_color_attr);

	g01_scissor(ctx);
	g02_depth(ctx);
	g03_stencil(ctx);
	g04_blend(ctx);
	g05_colormask(ctx);
	g06_logicop(ctx);
	g07_cull(ctx);
	g08_fog(ctx);
	g09_clipplane(ctx);
	g10_zcull(ctx);
	g11_zpass(ctx);
	g12_vertdata4f(ctx);
	g13_texborder(ctx);
	g14_cache(ctx);
	g15_renderenable(ctx);

	printf("STATE_TOUR DONE pass=%d fail=%d\n", g_pass, g_fail);

	free(host_addr);
	printf("hello-ppu-cellgcm-state-tour: done\n");
	return g_fail == 0 ? 0 : 1;
}
