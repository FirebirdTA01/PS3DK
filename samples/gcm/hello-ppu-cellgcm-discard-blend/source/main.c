/*
 * hello-ppu-cellgcm-discard-blend — rsx-cg-compiler validation sample
 * for compound-discard + simple-discard + VecConstructTexMul output
 * mixing, end-to-end through cellGcmCg*.
 *
 * Fragment-program features under test:
 *
 *   1) Simple discard (FX12 SLT + KIL):
 *        if (texB.a < 0.1) discard;
 *
 *   2) Compound discard (LogicalAnd, MULXC + KIL):
 *        if (u_thresh > 0.0 && texA.a < u_thresh) discard;
 *      Lowers to MOVR R1.x (pre-pass), SGTR H2.x, SLTR H2.w,
 *      MULXC RC.x, KIL (NE.x).  Embedded uniform const blocks
 *      carry descending ucode offsets.
 *
 *   3) VecConstructTexMul post-discard output:
 *        float4(texA.xyz, pass_color.a * texA.w);
 *      Lowers to MOVH H2.x + MOVR R0.xyz + MULR R0.w.
 *
 *   4) $kill_0000 synthetic container parameter.
 *
 * Visual setup:
 *   - Full-viewport quad with 4-corner colour gradient.
 *   - Texture A (unit 0): soft radial luminance with alpha ramp
 *     (centre bright/opaque → edge dim/transparent).
 *   - Texture B (unit 1): circular hard mask — inside circle is
 *     transparent (alpha=0), outside is opaque (alpha=255).
 *   - Uniform u_thresh = 0.35 — pixels with texA.a below this are
 *     discarded by the compound condition.
 *   - Result: two concentric discard zones (centre circle from texB,
 *     mid-ring from texA threshold), outer ring shows textured
 *     colour with per-vertex gradient tint.
 *
 * Render plumbing mirrors hello-ppu-cellgcm-alpha-mask.
 * Press START to exit early.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <unistd.h>

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sysutil/video.h>
#include <io/pad.h>
#include <cell/gcm.h>
#include <cell/sysutil.h>
#include <Cg/cg.h>

#include "vpshader_vpo.h"
#include "fpshader_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE              0x10000
#define HOST_SIZE            (1 * 1024 * 1024)
#define COLOR_BUFFER_NUM     4
#define MAX_QUEUE_FRAMES     1

#define LABEL_PREPARED_BUFFER_OFFSET   0x41
#define LABEL_BUFFER_STATUS_OFFSET     0x42
#define BUFFER_IDLE                    0
#define BUFFER_BUSY                    1

#define TEX_W  64u
#define TEX_H  64u

/* Uniform value set at runtime: pixels with texA.a below this are
 * discarded by the compound condition.  0.35 is half-way between the
 * centre opaque region (a≈0.7) and the edge transparent region
 * (a≈0.05), yielding a ring-shaped discard zone. */
static const float g_uniformThresh = 0.35f;

typedef struct {
	float pos[3];
	float uv0[2];
	float uv1[2];
	float color[4];
} vertex_t;

static u32 display_width;
static u32 display_height;
static u32 color_pitch;
static u32 depth_pitch;
static u32 color_offset[COLOR_BUFFER_NUM];
static u32 depth_offset;

static u32 g_local_mem_heap;
static void *local_alloc(u32 size)
{
	u32 sz = (size + 1023) & ~1023u;
	u32 base = g_local_mem_heap;
	g_local_mem_heap += sz;
	return (void *)(uintptr_t)base;
}
static void *local_align(u32 alignment, u32 size)
{
	g_local_mem_heap = (g_local_mem_heap + alignment - 1) & ~(alignment - 1);
	return local_alloc(size);
}

static CGprogram   vpo;
static CGprogram   fpo;
static void       *vp_ucode;
static void       *fp_ucode;
static u32         fp_offset;
static CGparameter modelViewProjConst;
static CGparameter threshUniform;
static int         position_index;
static int         texcoord0_index;
static int         texcoord1_index;
static int         color_index;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;
static u16      *index_buffer;
static u32       index_buffer_offset;
static u32      *texture_a_pixels;
static u32       texture_a_offset;
static u32      *texture_b_pixels;
static u32       texture_b_offset;

static volatile u32 g_buffer_on_display = 0;
static volatile u32 g_buffer_flipped    = 0;
static volatile u32 g_on_flip           = 0;
static u32          g_frame_index       = 0;

static volatile int g_exit_request   = 0;
static volatile int g_drawing_paused = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
	(void)param; (void)userdata;
	switch (status) {
	case CELL_SYSUTIL_REQUEST_EXITGAME:   g_exit_request = 1;   break;
	case CELL_SYSUTIL_DRAWING_BEGIN:
	case CELL_SYSUTIL_SYSTEM_MENU_OPEN:   g_drawing_paused = 1; break;
	case CELL_SYSUTIL_DRAWING_END:
	case CELL_SYSUTIL_SYSTEM_MENU_CLOSE:  g_drawing_paused = 0; break;
	default: break;
	}
}

static void on_flip(const u32 head)
{
	(void)head;
	u32 v = g_buffer_flipped;
	for (u32 i = g_buffer_on_display; i != v; i = (i + 1) % COLOR_BUFFER_NUM)
		*(volatile u32 *)cellGcmGetLabelAddress(LABEL_BUFFER_STATUS_OFFSET + i) = BUFFER_IDLE;
	g_buffer_on_display = v;
	g_on_flip = 0;
}

static void on_vblank(const u32 head)
{
	(void)head;
	u32 data = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET);
	u32 buffer_to_flip = data >> 8;
	u32 index_to_flip  = data & 0x7;

	if (g_on_flip == 0 && buffer_to_flip != g_buffer_on_display) {
		g_on_flip = 1;
		s32 ret = cellGcmSetFlipImmediate((u8)index_to_flip);
		if (ret != 0) {
			printf("FlipImmediate failed: %d\n", ret);
			g_on_flip = 0;
			return;
		}
		g_buffer_flipped = buffer_to_flip;
	}
}

static int ppu_too_fast(void)
{
	u32 gpu = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET);
	gpu >>= 8;
	return (((g_frame_index + COLOR_BUFFER_NUM - gpu) % COLOR_BUFFER_NUM) > MAX_QUEUE_FRAMES);
}

static void set_render_target(CellGcmContextData *ctx, u32 idx)
{
	CellGcmSurface sf = {0};
	sf.colorFormat      = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = color_offset[idx];
	sf.colorPitch[0]    = color_pitch;
	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1]    = 64;
	sf.colorPitch[2]    = 64;
	sf.colorPitch[3]    = 64;
	sf.depthFormat      = GCM_SURFACE_ZETA_Z16;
	sf.depthLocation    = GCM_LOCATION_RSX;
	sf.depthOffset      = depth_offset;
	sf.depthPitch       = depth_pitch;
	sf.type             = GCM_TEXTURE_LINEAR;
	sf.antiAlias        = GCM_SURFACE_CENTER_1;
	sf.width            = display_width;
	sf.height           = display_height;
	sf.x                = 0;
	sf.y                = 0;
	cellGcmSetSurface(ctx, &sf);
}

static void set_draw_env(CellGcmContextData *ctx)
{
	cellGcmSetColorMask(ctx,
		GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	cellGcmSetColorMaskMrt(ctx, 0);

	float min = 0.0f, max = 1.0f;
	float scale[4]  = { display_width * 0.5f, display_height * -0.5f, (max - min) * 0.5f, 0.0f };
	float offset[4] = { display_width * 0.5f, display_height *  0.5f, (max + min) * 0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, display_width, display_height, min, max, scale, offset);
	rsxSetScissor(ctx, 0, 0, display_width, display_height);

	cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetBlendEnable(ctx, GCM_FALSE);
}

static void bind_textures(CellGcmContextData *ctx)
{
	CellGcmTexture t = {0};
	t.format    = CELL_GCM_TEXTURE_A8R8G8B8
	            | CELL_GCM_TEXTURE_LN
	            | CELL_GCM_TEXTURE_NR;
	t.mipmap    = 1;
	t.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
	t.cubemap   = CELL_GCM_FALSE;
	t.remap     = (CELL_GCM_TEXTURE_REMAP_REMAP <<  8)
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 10)
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 12)
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 14)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_A <<  0)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_R <<  2)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_G <<  4)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_B <<  6);
	t.width     = (u16)TEX_W;
	t.height    = (u16)TEX_H;
	t.depth     = 1;
	t.location  = CELL_GCM_LOCATION_LOCAL;
	t.pitch     = (u32)(TEX_W * sizeof(u32));

	/* Texture unit 0: radial luminance with alpha ramp */
	t.offset    = texture_a_offset;
	cellGcmSetTexture(ctx, 0, &t);
	cellGcmSetTextureControl(ctx, 0, CELL_GCM_TRUE,
	                         0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
	cellGcmSetTextureFilter(ctx, 0, 0,
	                        CELL_GCM_TEXTURE_LINEAR,
	                        CELL_GCM_TEXTURE_LINEAR,
	                        CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	cellGcmSetTextureAddress(ctx, 0,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
	                         CELL_GCM_TEXTURE_ZFUNC_LESS,
	                         0);

	/* Texture unit 1: hard circular mask */
	t.offset    = texture_b_offset;
	cellGcmSetTexture(ctx, 1, &t);
	cellGcmSetTextureControl(ctx, 1, CELL_GCM_TRUE,
	                         0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
	cellGcmSetTextureFilter(ctx, 1, 0,
	                        CELL_GCM_TEXTURE_LINEAR,
	                        CELL_GCM_TEXTURE_LINEAR,
	                        CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	cellGcmSetTextureAddress(ctx, 1,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
	                         CELL_GCM_TEXTURE_ZFUNC_LESS,
	                         0);
}

static void set_render_state(CellGcmContextData *ctx)
{
	static const float mvp[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1,
	};

	cellGcmSetVertexProgram(ctx, vpo, vp_ucode);
	cellGcmSetVertexProgramParameter(ctx, modelViewProjConst, mvp);

	/* Bind per-vertex data arrays */
	cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 3,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, texcoord0_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, uv0));
	cellGcmSetVertexDataArray(ctx, texcoord1_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, uv1));
	cellGcmSetVertexDataArray(ctx, color_index, 0, sizeof(vertex_t), 4,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, color));

	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);

	/* Runtime uniform: alpha-discard threshold */
	cellGcmSetFragmentProgramParameter(ctx, fpo, threshUniform,
	                                   &g_uniformThresh, fp_offset);

	bind_textures(ctx);
}

static void flip(CellGcmContextData *ctx)
{
	s32 qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index);
	while (qid < 0) {
		usleep(1000);
		qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index);
	}

	cellGcmSetWriteBackEndLabel(ctx, LABEL_PREPARED_BUFFER_OFFSET,
	                            (g_frame_index << 8) | (u32)qid);
	cellGcmFlush(ctx);

	while (ppu_too_fast())
		usleep(3000);

	set_draw_env(ctx);
	set_render_state(ctx);

	g_frame_index = (g_frame_index + 1) % COLOR_BUFFER_NUM;

	cellGcmSetWaitLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_IDLE);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	set_render_target(ctx, g_frame_index);
}

static int init_display(void)
{
	videoState         state;
	videoConfiguration vcfg;
	videoResolution    res;

	if (videoGetState(0, 0, &state) != 0 || state.state != 0) return 0;
	if (videoGetResolution(state.displayMode.resolution, &res) != 0) return 0;

	display_width  = res.width;
	display_height = res.height;
	color_pitch    = display_width * sizeof(uint32_t);
	depth_pitch    = display_width * sizeof(uint16_t);

	memset(&vcfg, 0, sizeof(vcfg));
	vcfg.resolution = state.displayMode.resolution;
	vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vcfg.pitch      = color_pitch;
	vcfg.aspect     = state.displayMode.aspect;
	if (videoConfigure(0, &vcfg, NULL, 0) != 0) return 0;

	cellGcmSetFlipMode(GCM_FLIP_VSYNC);

	CellGcmConfig cfg;
	cellGcmGetConfiguration(&cfg);
	g_local_mem_heap = (u32)(uintptr_t)cfg.localAddress;

	u32 color_size = color_pitch * display_height;
	u32 depth_size = depth_pitch * display_height;

	void *color_base = local_align(64, COLOR_BUFFER_NUM * color_size);
	for (int i = 0; i < COLOR_BUFFER_NUM; i++) {
		void *addr = (void *)((uintptr_t)color_base + (u32)i * color_size);
		cellGcmAddressToOffset(addr, &color_offset[i]);
		cellGcmSetDisplayBuffer((u8)i, color_offset[i],
		                        color_pitch, display_width, display_height);
	}

	void *depth_addr = local_align(64, depth_size);
	cellGcmAddressToOffset(depth_addr, &depth_offset);

	for (int i = 0; i < COLOR_BUFFER_NUM; i++)
		*(volatile u32 *)cellGcmGetLabelAddress(LABEL_BUFFER_STATUS_OFFSET + i) = BUFFER_IDLE;
	*(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) = (g_buffer_on_display << 8);
	*(volatile u32 *)cellGcmGetLabelAddress(LABEL_BUFFER_STATUS_OFFSET + g_buffer_on_display) = BUFFER_BUSY;
	g_frame_index = (g_buffer_on_display + 1) % COLOR_BUFFER_NUM;

	cellGcmSetVBlankHandler(on_vblank);
	cellGcmSetFlipHandler(on_flip);
	return 1;
}

static void init_shaders(void)
{
	u32 vpsize = 0, fpsize = 0;
	vpo = (CGprogram)vpshader_vpo;
	fpo = (CGprogram)fpshader_fpo;

	cellGcmCgInitProgram(vpo);
	cellGcmCgInitProgram(fpo);

	cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);
	modelViewProjConst = cellGcmCgGetNamedParameter(vpo, "modelViewProj");

	void *fp_ucode_blob;
	cellGcmCgGetUCode(fpo, &fp_ucode_blob, &fpsize);
	fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_ucode_blob, fpsize);
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	threshUniform = cellGcmCgGetNamedParameter(fpo, "u_thresh");

	CGparameter pos  = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter uv0  = cellGcmCgGetNamedParameter(vpo, "in_texcoord0");
	CGparameter uv1  = cellGcmCgGetNamedParameter(vpo, "in_texcoord1");
	CGparameter col  = cellGcmCgGetNamedParameter(vpo, "in_color");
	position_index   = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	texcoord0_index  = uv0 ? (int)cellGcmCgGetParameterResource(vpo, uv0) - CG_ATTR0 : 8;
	texcoord1_index  = uv1 ? (int)cellGcmCgGetParameterResource(vpo, uv1) - CG_ATTR0 : 9;
	color_index      = col ? (int)cellGcmCgGetParameterResource(vpo, col) - CG_ATTR0 : 3;

	printf("  vp ucode: %u bytes; mvp uniform: %p\n", vpsize, (void *)modelViewProjConst);
	printf("  fp ucode: %u bytes (rsx-local at offset 0x%08x)\n", fpsize, fp_offset);
	printf("  fp uniform u_thresh: %p\n", (void *)threshUniform);
	printf("  vp attribs: position@%d texcoord0@%d texcoord1@%d color@%d\n",
	       position_index, texcoord0_index, texcoord1_index, color_index);
}

/*
 * Texture A: soft radial luminance with alpha ramp.
 *   Centre (radius 0..TEX_W/6):  white, alpha ≈ 0.7
 *   Mid   (radius TEX_W/6..TEX_W/3):  gradient to grey, alpha ramp 0.7→0.35
 *   Edge  (outer third):  dark grey, alpha ramp 0.35→0.05
 * The compound discard kills pixels where alpha < 0.35, which
 * happens in the outer two-thirds.
 */
static void init_texture_a(void)
{
	texture_a_pixels = (u32 *)local_align(128, TEX_W * TEX_H * sizeof(u32));

	const float cx = (float)TEX_W * 0.5f;
	const float cy = (float)TEX_H * 0.5f;
	const float maxDist = (float)TEX_W * 0.5f;

	for (u32 y = 0; y < TEX_H; y++) {
		for (u32 x = 0; x < TEX_W; x++) {
			float dx = (float)x - cx;
			float dy = (float)y - cy;
			float dist = sqrtf(dx * dx + dy * dy);
			float t = (dist < 1.0f) ? 1.0f : 1.0f - (dist / maxDist);
			if (t < 0.0f) t = 0.0f;
			u8 lum  = (u8)(160.0f + 95.0f * t);  /* 160..255 */
			u8 alpha = (u8)(10.0f + 180.0f * t);  /* 10..190 → ≈ 0.04..0.75 */
			texture_a_pixels[y * TEX_W + x] =
				((u32)alpha << 24)
				| ((u32)lum << 16)
				| ((u32)lum << 8)
				| ((u32)lum);
		}
	}

	cellGcmAddressToOffset(texture_a_pixels, &texture_a_offset);
	printf("  texture A: %ux%u radial luminance + alpha ramp at rsx offset 0x%08x\n",
	       TEX_W, TEX_H, texture_a_offset);
}

/*
 * Texture B: hard circular mask (matches discard_tex_alpha sample).
 *   Inside radius (centre circle):  alpha = 0  → discarded
 *   Outside:                        alpha = 255 → pass
 * RGB is solid white — the FP only reads alpha for the discard check.
 */
static void init_texture_b(void)
{
	texture_b_pixels = (u32 *)local_align(128, TEX_W * TEX_H * sizeof(u32));

	const float cx = (float)TEX_W * 0.5f;
	const float cy = (float)TEX_H * 0.5f;
	const float r2 = (TEX_W * 0.22f) * (TEX_W * 0.22f);

	for (u32 y = 0; y < TEX_H; y++) {
		for (u32 x = 0; x < TEX_W; x++) {
			float dx = (float)x - cx;
			float dy = (float)y - cy;
			int   inside = (dx * dx + dy * dy) < r2;
			u8 alpha = inside ? 0x00 : 0xff;
			texture_b_pixels[y * TEX_W + x] =
				((u32)alpha << 24) | 0x00ffffffu;
		}
	}

	cellGcmAddressToOffset(texture_b_pixels, &texture_b_offset);
	printf("  texture B: %ux%u hard circular mask at rsx offset 0x%08x\n",
	       TEX_W, TEX_H, texture_b_offset);
}

static void init_geometry(void)
{
	/* Full-viewport quad (clip-space [-0.85, 0.85]) with a 4-corner
	 * colour gradient and two 0..1 UV sets.  2 triangles, 6 indices. */
	static const vertex_t verts[4] = {
		{ {-0.85f,-0.85f, 0.0f}, {0.0f,1.0f}, {0.0f,1.0f}, {1.0f,0.0f,0.0f,1.0f} },
		{ { 0.85f,-0.85f, 0.0f}, {1.0f,1.0f}, {1.0f,1.0f}, {0.0f,1.0f,0.0f,1.0f} },
		{ { 0.85f, 0.85f, 0.0f}, {1.0f,0.0f}, {1.0f,0.0f}, {0.0f,0.0f,1.0f,1.0f} },
		{ {-0.85f, 0.85f, 0.0f}, {0.0f,0.0f}, {0.0f,0.0f}, {1.0f,1.0f,1.0f,1.0f} },
	};
	static const u16 idx[6] = { 0, 1, 2,  0, 2, 3 };

	vertex_buffer = (vertex_t *)local_align(128, sizeof verts);
	memcpy(vertex_buffer, verts, sizeof verts);
	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);

	index_buffer = (u16 *)local_align(128, sizeof idx);
	memcpy(index_buffer, idx, sizeof idx);
	cellGcmAddressToOffset(index_buffer, &index_buffer_offset);
}

static void program_exit(CellGcmContextData *ctx)
{
	cellGcmFinish(ctx, 1);
	u32 last = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) >> 8;
	while (g_buffer_on_display != last)
		usleep(1000);
}

int main(int argc, const char **argv)
{
	(void)argc; (void)argv;

	void               *host_addr;
	CellGcmContextData *ctx;

	printf("hello-ppu-cellgcm-discard-blend: FP discard + VecConstructTexMul validation\n");

	host_addr = memalign(1024 * 1024, HOST_SIZE);
	if (cellGcmInit(CB_SIZE, HOST_SIZE, host_addr) != 0) {
		printf("  cellGcmInit failed\n");
		free(host_addr);
		return 1;
	}
	if (!init_display()) {
		printf("  init_display failed\n");
		free(host_addr);
		return 1;
	}
	ctx = CELL_GCM_CURRENT;
	printf("  RSX up at %ux%u\n", display_width, display_height);

	ioPadInit(7);
	cellSysutilRegisterCallback(0, on_sysutil_event, NULL);

	init_shaders();
	init_texture_a();
	init_texture_b();
	init_geometry();

	set_draw_env(ctx);
	set_render_state(ctx);
	set_render_target(ctx, g_frame_index);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	printf("  uniform u_thresh = %.2f\n", g_uniformThresh);
	printf("  running for 10 s — press START to exit\n");

	const int total = 600;  /* 10 s @ 60 Hz */
	int       frame = 0;

	while (frame < total && !g_exit_request) {
		cellSysutilCheckCallback();

		padInfo padinfo;
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				padData paddata = {0};
				ioPadGetData(i, &paddata);
				/* Detect rising edge — only exit on press, not hold */
				static uint16_t prev_start[MAX_PADS];
				uint16_t cur = paddata.BTN_START;
				if (cur && !prev_start[i]) {
					g_exit_request = 1; break;
				}
				prev_start[i] = cur;
			}
		}

		if (g_drawing_paused) {
			usleep(16000);
			continue;
		}

		cellGcmSetClearColor(ctx, 0xff202028);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
		rsxDrawIndexArray(ctx, GCM_TYPE_TRIANGLES,
		                  index_buffer_offset, 6,
		                  GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);

		flip(ctx);
		frame++;
	}

	printf("  drew %d frames\n", frame);
	if (g_exit_request) printf("  early exit on user request\n");

	program_exit(ctx);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-discard-blend: done\n");
	return 0;
}
