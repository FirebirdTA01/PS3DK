/*
 * uniform-readback — tier-c judged fragment uniform regression row.
 *
 * Asserts that a runtime fragment uniform (cellGcmSetFragmentProgramParameter)
 * actually updates RSX local memory and alters fragment shader output.
 *
 * Sequence:
 *   1. Clear RT to GPU_CLEAR_MARK1, set uniform u_color = val1, render quad,
 *      read back pixels to draw1_pixels.
 *   2. Clear RT to GPU_CLEAR_MARK2, set uniform u_color = val2, render quad,
 *      read back pixels to draw2_pixels.
 *   3. Compare draw1_pixels against draw2_pixels.
 *
 * THE FAILURE CONDITION: SAMENESS.
 * If cellGcmSetFragmentProgramParameter silently fails to patch RSX memory,
 * both draws execute with the compile-time baked default constants (all zeros)
 * and produce identical output (diff_pixels == 0). The row fails loudly.
 *
 * NOTE ON VALUES: Both draws reading 0x00000000 is consistent with the uniform
 * never being applied AND with it being applied-but-writing-zeros. This row
 * asserts DIFFERENCE, not any particular value. The specific probe colors
 * (val1 != val2 != 0) ensure that a partial, unvarying, or zero write also
 * fails, but the core invariant under test is that changing the uniform value
 * at runtime changes the rasterized output.
 *
 * SENTINELS & POSITIVE GATE:
 * Success requires the positive sentinel 'UNIFORM_READBACK_OK' on TTY; failure
 * emits 'UNIFORM_READBACK_FAIL'. The regression harness enforces both the
 * presence of the OK marker and the absence of the FAIL marker.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sysutil/video.h>
#include <cell/gcm.h>
#include <rsx/rsx.h>

#include "ru_pos_uv_vpo.h"
#include "ru_uniform_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2

#define GCM_LABEL_INDEX 255

#define RT_W 64
#define RT_H 64
#define RT_PIXELS (RT_W * RT_H)
#define CLEAR_SENTINEL   0xFF000000u  /* opaque black: host buffer sentinel */
#define GPU_CLEAR_MARK1  0xFF112233u  /* Draw 1 clear mark */
#define GPU_CLEAR_MARK2  0xFF445566u  /* Draw 2 clear mark */
#define TOLERANCE 3                   /* per-channel 8-bit tolerance */

typedef struct {
	float pos[2];
	float uv[2];
} vertex_t;

/* RSX local-memory bump allocator */
static u32 g_local_mem_heap;
static void *local_align(u32 alignment, u32 size)
{
	g_local_mem_heap = (g_local_mem_heap + alignment - 1u) & ~(alignment - 1u);
	void *p = (void *)(uintptr_t)g_local_mem_heap;
	g_local_mem_heap += (size + 1023u) & ~1023u;
	return p;
}

static void wait_rsx_idle(CellGcmContextData *ctx)
{
	vu32 *slot = (vu32 *)cellGcmGetLabelAddress(GCM_LABEL_INDEX);
	uint32_t target = *slot + 1u;
	cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, target);
	cellGcmFlush(ctx);
	while (*slot != target)
		usleep(30);
}

typedef struct {
	uint32_t *ptr;
	uint32_t  offset;
} display_buffer;

static u32 display_pitch;
static u32 display_w, display_h;
static u32 disp_depth_offset;

static int init_screen(void *host_addr, uint32_t size)
{
	videoState         state;
	videoConfiguration vcfg;
	videoResolution    res;

	if (cellGcmInit(CB_SIZE, size, host_addr) != 0)
		return 0;
	if (videoGetState(0, 0, &state) != 0 || state.state != 0)
		return 0;
	if (videoGetResolution(state.displayMode.resolution, &res) != 0)
		return 0;

	memset(&vcfg, 0, sizeof(vcfg));
	vcfg.resolution = state.displayMode.resolution;
	vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vcfg.pitch      = res.width * sizeof(uint32_t);
	vcfg.aspect     = state.displayMode.aspect;

	wait_rsx_idle(CELL_GCM_CURRENT);
	if (videoConfigure(0, &vcfg, NULL, 0) != 0)
		return 0;
	if (videoGetState(0, 0, &state) != 0)
		return 0;

	cellGcmSetFlipMode(GCM_FLIP_VSYNC);

	CellGcmConfig cfg;
	cellGcmGetConfiguration(&cfg);
	g_local_mem_heap = (u32)(uintptr_t)cfg.localAddress;

	display_w     = res.width;
	display_h     = res.height;
	display_pitch = res.width * sizeof(uint32_t);

	void *dd = local_align(64, display_h * display_pitch);
	cellGcmAddressToOffset(dd, &disp_depth_offset);
	cellGcmResetFlipStatus();
	return 1;
}

static int make_buffer(display_buffer *b, int id)
{
	int pitch = (int)display_pitch;
	int size  = pitch * (int)display_h;
	b->ptr = (uint32_t *)local_align(64, (u32)size);
	if (!b->ptr) return 0;
	if (cellGcmAddressToOffset(b->ptr, &b->offset) != 0) return 0;
	if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, (u32)pitch,
	                            display_w, display_h) != 0) return 0;
	return 1;
}

static void set_rt_surface(CellGcmContextData *ctx,
                           u32 color_off, u32 depth_off, u32 pitch,
                           u16 w, u16 h)
{
	CellGcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat      = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = color_off;
	sf.colorPitch[0]    = pitch;
	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1]    = 64;
	sf.colorPitch[2]    = 64;
	sf.colorPitch[3]    = 64;
	sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation    = GCM_LOCATION_RSX;
	sf.depthOffset      = depth_off;
	sf.depthPitch       = pitch;
	sf.type             = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias        = GCM_SURFACE_CENTER_1;
	sf.width            = w;
	sf.height           = h;
	sf.x                = 0;
	sf.y                = 0;
	cellGcmSetSurface(ctx, &sf);
}

static void set_draw_env(CellGcmContextData *ctx, u16 w, u16 h)
{
	cellGcmSetColorMask(ctx,
		GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	cellGcmSetColorMaskMrt(ctx, 0);

	float min = 0.0f, max = 1.0f;
	float scale[4]  = { w * 0.5f, h * -0.5f, (max - min) * 0.5f, 0.0f };
	float offset[4] = { w * 0.5f, h *  0.5f, (max + min) * 0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, w, h, min, max, scale, offset);
	rsxSetScissor(ctx, 0, 0, w, h);

	cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetBlendEnable(ctx, GCM_FALSE);
}

static u32 *g_readback;
static u32  g_readback_off;

static void transfer_rt_to_main(CellGcmContextData *ctx, u32 rt_off, u32 rt_pitch)
{
	cellGcmSetTransferImage(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN,
	                        g_readback_off, rt_pitch, 0, 0,
	                        rt_off, rt_pitch, 0, 0,
	                        RT_W, RT_H, 4);
	wait_rsx_idle(ctx);
}

static inline float saturatef(float x)
{
	return x != x ? 0.0f : (x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x));
}

static inline u32 pack_rgba(float r, float g, float b, float a)
{
	u32 ur = (u32)(saturatef(r) * 255.0f + 0.5f);
	u32 ug = (u32)(saturatef(g) * 255.0f + 0.5f);
	u32 ub = (u32)(saturatef(b) * 255.0f + 0.5f);
	u32 ua = (u32)(saturatef(a) * 255.0f + 0.5f);
	return (ua << 24) | (ur << 16) | (ug << 8) | ub;
}

int main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	printf("uniform-readback: fragment uniform runtime update witness (t_224f9771)\n");

	void *host_addr = memalign(1024 * 1024, HOST_SIZE);
	if (!init_screen(host_addr, HOST_SIZE)) {
		printf("uniform-readback: init failed\nUNIFORM_READBACK_FAIL\n");
		free(host_addr);
		return 1;
	}
	CellGcmContextData *ctx = (CellGcmContextData *)gCellGcmCurrentContext;

	display_buffer buffers[MAX_BUFFERS];
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!make_buffer(&buffers[i], i)) {
			printf("uniform-readback: buffer init failed\nUNIFORM_READBACK_FAIL\n");
			cellGcmFinish(ctx, 0);
			free(host_addr);
			return 1;
		}
	}
	if (cellGcmSetFlip(ctx, (uint8_t)(MAX_BUFFERS - 1)) == 0) {
		cellGcmFlush(ctx);
		cellGcmSetWaitFlip(ctx);
	}

	set_rt_surface(ctx, buffers[0].offset, disp_depth_offset,
	               display_pitch, (u16)display_w, (u16)display_h);
	wait_rsx_idle(ctx);

	/* ---- vertex program (fullscreen quad) ---- */
	CGprogram vpo = (CGprogram)ru_pos_uv_vpo;
	cellGcmCgInitProgram(vpo);
	void *vp_ucode = NULL; u32 vpsize = 0;
	cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);

	CGparameter pos = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter tc  = cellGcmCgGetNamedParameter(vpo, "in_texcoord");
	int position_index = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	int texcoord_index = tc  ? (int)cellGcmCgGetParameterResource(vpo, tc)  - CG_ATTR0 : 8;

	vertex_t *vertex_buffer = (vertex_t *)local_align(128, 4 * sizeof(vertex_t));
	vertex_buffer[0] = (vertex_t){{-1.0f,  1.0f}, {0.0f, 0.0f}};
	vertex_buffer[1] = (vertex_t){{ 1.0f,  1.0f}, {1.0f, 0.0f}};
	vertex_buffer[2] = (vertex_t){{-1.0f, -1.0f}, {0.0f, 1.0f}};
	vertex_buffer[3] = (vertex_t){{ 1.0f, -1.0f}, {1.0f, 1.0f}};
	u32 vertex_buffer_offset = 0;
	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);

	cellGcmCgUploadInternalConsts(ctx, vpo);

	/* ---- fragment program ---- */
	CGprogram fpo = (CGprogram)ru_uniform_fpo;
	cellGcmCgInitProgram(fpo);
	void *fp_blob_ucode = NULL; u32 fpsize = 0;
	cellGcmCgGetUCode(fpo, &fp_blob_ucode, &fpsize);
	void *fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_blob_ucode, fpsize);
	u32 fp_offset = 0;
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	CGparameter u_color = cellGcmCgGetNamedParameter(fpo, "u_color");
	if (!u_color) {
		printf("uniform-readback: uniform 'u_color' not found in container\nUNIFORM_READBACK_FAIL\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 1;
	}

	/* ---- host readback buffer ---- */
	g_readback = (u32 *)((char *)host_addr + HOST_SIZE - (64 * 1024));
	if (cellGcmAddressToOffset(g_readback, &g_readback_off) != 0) {
		printf("uniform-readback: readback offset failed\nUNIFORM_READBACK_FAIL\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 1;
	}

	/* ---- off-screen RT ---- */
	u32 rt_pitch = RT_W * 4;
	u32 rt_sz    = rt_pitch * RT_H;
	u32 *rt_ptr  = (u32 *)local_align(64, rt_sz);
	void *rt_depth_ptr = local_align(64, rt_sz);
	u32 rt_off = 0, rt_depth_off = 0;
	if (cellGcmAddressToOffset(rt_ptr, &rt_off) != 0 ||
	    cellGcmAddressToOffset(rt_depth_ptr, &rt_depth_off) != 0) {
		printf("uniform-readback: RT alloc failed\nUNIFORM_READBACK_FAIL\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 1;
	}

	set_rt_surface(ctx, rt_off, rt_depth_off, rt_pitch, RT_W, RT_H);
	set_draw_env(ctx, RT_W, RT_H);

	cellGcmSetVertexProgram(ctx, vpo, vp_ucode);
	cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, texcoord_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, uv));

	/* Allocate buffers to capture pixel readback from both draws */
	static u32 draw1_pixels[RT_PIXELS];
	static u32 draw2_pixels[RT_PIXELS];

	const float val1[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
	const float val2[4] = { 0.80f, 0.20f, 0.40f, 1.00f };
	const u32 exp1 = pack_rgba(val1[0], val1[1], val1[2], val1[3]);
	const u32 exp2 = pack_rgba(val2[0], val2[1], val2[2], val2[3]);

	/* ================================================================
	 * DRAW 1: Apply val1, render quad, capture pixels
	 * ================================================================ */
	for (u32 i = 0; i < RT_PIXELS; i++) g_readback[i] = CLEAR_SENTINEL;
	cellGcmSetClearColor(ctx, GPU_CLEAR_MARK1);
	cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	wait_rsx_idle(ctx);
	transfer_rt_to_main(ctx, rt_off, rt_pitch);

	if (g_readback[0] != GPU_CLEAR_MARK1) {
		printf("uniform-readback: draw1 gpu clear failed (got 0x%08x want 0x%08x)\nUNIFORM_READBACK_FAIL\n",
		       g_readback[0], GPU_CLEAR_MARK1);
		cellGcmFinish(ctx, 0); free(host_addr); return 1;
	}

	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);
	cellGcmSetFragmentProgramParameter(ctx, fpo, u_color, val1, fp_offset);

	for (int tries = 1; tries <= 10; tries++) {
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
		wait_rsx_idle(ctx);
		transfer_rt_to_main(ctx, rt_off, rt_pitch);
		if (g_readback[0] != GPU_CLEAR_MARK1) break;
		usleep(200000);
	}
	memcpy(draw1_pixels, g_readback, sizeof(draw1_pixels));
	printf("  draw 1 complete: probe pixel=0x%08x (expected if uniform applied: 0x%08x)\n",
	       draw1_pixels[0], exp1);

	/* ================================================================
	 * DRAW 2: Apply val2, render quad, capture pixels
	 * ================================================================ */
	for (u32 i = 0; i < RT_PIXELS; i++) g_readback[i] = CLEAR_SENTINEL;
	cellGcmSetClearColor(ctx, GPU_CLEAR_MARK2);
	cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	wait_rsx_idle(ctx);
	transfer_rt_to_main(ctx, rt_off, rt_pitch);

	if (g_readback[0] != GPU_CLEAR_MARK2) {
		printf("uniform-readback: draw2 gpu clear failed (got 0x%08x want 0x%08x)\nUNIFORM_READBACK_FAIL\n",
		       g_readback[0], GPU_CLEAR_MARK2);
		cellGcmFinish(ctx, 0); free(host_addr); return 1;
	}

	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);
	cellGcmSetFragmentProgramParameter(ctx, fpo, u_color, val2, fp_offset);

	for (int tries = 1; tries <= 10; tries++) {
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
		wait_rsx_idle(ctx);
		transfer_rt_to_main(ctx, rt_off, rt_pitch);
		if (g_readback[0] != GPU_CLEAR_MARK2) break;
		usleep(200000);
	}
	memcpy(draw2_pixels, g_readback, sizeof(draw2_pixels));
	printf("  draw 2 complete: probe pixel=0x%08x (expected if uniform applied: 0x%08x)\n",
	       draw2_pixels[0], exp2);

	/* ================================================================
	 * EVALUATION: Check for sameness vs difference
	 * ================================================================ */
	int diff_pixels = 0;
	int max_delta = 0;
	for (int i = 0; i < RT_PIXELS; i++) {
		u32 p1 = draw1_pixels[i];
		u32 p2 = draw2_pixels[i];
		if (p1 != p2) diff_pixels++;
		for (int c = 0; c < 4; c++) {
			int ch1 = (int)((p1 >> (c * 8)) & 0xff);
			int ch2 = (int)((p2 >> (c * 8)) & 0xff);
			int d = ch1 - ch2;
			if (d < 0) d = -d;
			if (d > max_delta) max_delta = d;
		}
	}

	printf("  comparison across %d pixels: diff_pixels=%d max_delta=%d\n",
	       RT_PIXELS, diff_pixels, max_delta);

	/* Sameness check: if no pixels changed, uniform was not applied */
	if (diff_pixels == 0) {
		printf("uniform-readback: FAIL — sameness detected (diff_pixels=0/%d, max_delta=0)\n", RT_PIXELS);
		printf("  Both draws produced identical pixels (0x%08x); cellGcmSetFragmentProgramParameter did not update RSX memory\n",
		       draw1_pixels[0]);
		printf("UNIFORM_READBACK_FAIL\n");
		cellGcmSetWaitFlip(ctx);
		cellGcmFinish(ctx, 1);
		free(host_addr);
		return 1;
	}

	/* Difference detected: verify that both draws match their expected uniform colors */
	int d1_bad = 0, d2_bad = 0;
	for (int i = 0; i < RT_PIXELS; i++) {
		u32 p1 = draw1_pixels[i];
		u32 p2 = draw2_pixels[i];
		for (int c = 0; c < 4; c++) {
			int a1 = (int)((p1 >> (c * 8)) & 0xff);
			int e1 = (int)((exp1 >> (c * 8)) & 0xff);
			int a2 = (int)((p2 >> (c * 8)) & 0xff);
			int e2 = (int)((exp2 >> (c * 8)) & 0xff);
			if (abs(a1 - e1) > TOLERANCE) d1_bad++;
			if (abs(a2 - e2) > TOLERANCE) d2_bad++;
		}
	}

	if (d1_bad == 0 && d2_bad == 0 && diff_pixels == RT_PIXELS) {
		printf("uniform-readback: PASS (diff_pixels=%d/%d max_delta=%d, draw1=0x%08x draw2=0x%08x)\n",
		       diff_pixels, RT_PIXELS, max_delta, draw1_pixels[0], draw2_pixels[0]);
		printf("UNIFORM_READBACK_OK\n");
		cellGcmSetWaitFlip(ctx);
		cellGcmFinish(ctx, 1);
		free(host_addr);
		return 0;
	}

	printf("uniform-readback: FAIL — pixels changed but did not match expected uniform values (d1_bad=%d d2_bad=%d)\n",
	       d1_bad, d2_bad);
	printf("UNIFORM_READBACK_FAIL\n");
	cellGcmSetWaitFlip(ctx);
	cellGcmFinish(ctx, 1);
	free(host_addr);
	return 1;
}
