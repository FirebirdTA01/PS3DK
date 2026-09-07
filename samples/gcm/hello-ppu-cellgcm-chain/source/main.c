/*
 * hello-ppu-cellgcm-chain — rsx-cg-compiler FP arithmetic-chain validation
 * with per-draw runtime uniform patching.
 *
 * Same render plumbing as hello-ppu-cellgcm-triangle (flip_immediate,
 * 4 colour buffers, MVP-uniform VP) — exercises the FP arithmetic-
 * chain ADDR emit by adding three runtime-patched uniforms (a, b, c)
 * to the per-vertex colour.  See shaders/fpshader.fcg for the chain
 * shape the reference compiler + our compiler both emit.
 *
 * The byte-diff harness at tools/rsx-cg-compiler/tests/run_diff.sh
 * proves our compiler's output matches the reference compiler.  This sample proves
 * those bytes execute correctly on RSX with runtime FP uniforms
 * patched in via cellGcmSetFragmentProgramParameter between successive draws
 * in the same frame:
 *
 *   - Draw 1 (left):  a=0, b=0, c=0 via cellGcmSetFragmentProgramParameter.
 *     Renders baseline canonical RGB-gradient triangle.
 *   - Draw 2 (right): a=(0.15,0,0,0), b=(0,0.15,0,0), c=(0,0,0.15,0) via
 *     cellGcmSetFragmentProgramParameter.  Renders visibly brighter triangle
 *     with all channels lifted by +0.15.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
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
#define HOST_SIZE            (16 * 1024 * 1024)
#define COLOR_BUFFER_NUM     4
#define MAX_QUEUE_FRAMES     1

/* Readback buffer in host memory for verifying rendered pixels. */
static u32 *g_readback = NULL;
static u32  g_readback_offset = 0;

/* Labels — same offsets the canonical flip_immediate sample uses. */
#define LABEL_PREPARED_BUFFER_OFFSET   0x41
#define LABEL_BUFFER_STATUS_OFFSET     0x42
#define BUFFER_IDLE                    0
#define BUFFER_BUSY                    1
#define GCM_LABEL_INDEX                255

typedef struct {
	float pos[3];
	float color[4];
} vertex_t;

static u32  display_width;
static u32  display_height;
static u32  color_pitch;
static u32  depth_pitch;
static u32  color_offset[COLOR_BUFFER_NUM];
static u32  depth_offset;

/* Local-memory bump allocator (matches the reference localMemoryAlloc). */
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

static void wait_rsx_idle(CellGcmContextData *ctx)
{
	vu32 *slot = (vu32 *)cellGcmGetLabelAddress(GCM_LABEL_INDEX);
	uint32_t target = *slot + 1u;
	cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, target);
	cellGcmFlush(ctx);
	while (*slot != target)
		usleep(30);
}

/* Shader objects.  CgBinaryProgram blobs from rsx-cg-compiler's
 * --emit-container output — paired with the cellGcmCg* / cellGcmSet*
 * runtime API (matches what reference SDK basic.cpp uses). */
static CGprogram   vpo;
static CGprogram   fpo;
static void       *vp_ucode;
static void       *fp_ucode;
static u32         fp_offset;
static CGparameter modelViewProjConst;
static int         position_index;
static int         color_index;

/* Fragment-program uniforms (a, b, c).  Each parameter lists the byte
 * offsets of its embedded-constant slots inside the fp ucode; patched
 * into RSX local memory via cellGcmSetFragmentProgramParameter. */
static CGparameter aParam;
static CGparameter bParam;
static CGparameter cParam;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;

/* Flip state — read by VBlank/Flip handlers and main flip(). */
static volatile u32 g_buffer_on_display = 0;
static volatile u32 g_buffer_flipped    = 0;
static volatile u32 g_on_flip           = 0;
static u32          g_frame_index       = 0;

/* Sysutil event signals. DRAWING_PAUSED is set while the XMB owns the
 * FIFO (between DRAWING_BEGIN and DRAWING_END / SYSTEM_MENU_CLOSE).
 * The render loop yields the FIFO while paused to avoid stuttering
 * PS-button menu navigation. */
static volatile int g_exit_request   = 0;
static volatile int g_drawing_paused = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
	(void)param;
	(void)userdata;
	switch (status) {
	case CELL_SYSUTIL_REQUEST_EXITGAME:
		g_exit_request = 1;
		break;
	case CELL_SYSUTIL_DRAWING_BEGIN:
	case CELL_SYSUTIL_SYSTEM_MENU_OPEN:
		g_drawing_paused = 1;
		break;
	case CELL_SYSUTIL_DRAWING_END:
	case CELL_SYSUTIL_SYSTEM_MENU_CLOSE:
		g_drawing_paused = 0;
		break;
	default:
		break;
	}
}

/* ----------------------------------------------------------------
 * VBlank + Flip handlers — canonical flip_immediate pattern.
 * VBlank reads the "prepared buffer" label and issues an immediate
 * flip if there is a new buffer ready.  Flip handler marks all
 * scanned-out buffers IDLE so the main loop can reuse them.
 * ---------------------------------------------------------------- */
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

/* ----------------------------------------------------------------
 * Render-state setup.
 * ---------------------------------------------------------------- */
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

	cellGcmSetDepthTestEnable(ctx, GCM_TRUE);
	cellGcmSetDepthFunc(ctx, GCM_LESS);
	cellGcmSetDepthMask(ctx, GCM_TRUE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetBlendEnable(ctx, GCM_FALSE);
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

	cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 3,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, color_index, 0, sizeof(vertex_t), 4,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, color));

	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);
}

/* ----------------------------------------------------------------
 * flip() — canonical flip_immediate pattern.
 *  1. SetPrepareFlip enqueues a PREPARE_FLIP command in the CB.
 *  2. WriteBackEndLabel writes (frame_index<<8 | qid) to label
 *     LABEL_PREPARED_BUFFER_OFFSET so VBlank handler knows what to flip.
 *  3. Flush so the GPU can start consuming.
 *  4. Throttle PPU if it's >MAX_QUEUE_FRAMES ahead of GPU.
 *  5. Re-emit draw env / render state — they don't persist across
 *     buffer cycles.
 *  6. Bump frame_index, mark next buffer BUSY, set new render target.
 * ---------------------------------------------------------------- */
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

/* ----------------------------------------------------------------
 * Init helpers.
 * ---------------------------------------------------------------- */
static int init_display(void)
{
	videoState         state;
	videoConfiguration vcfg;
	videoResolution    res;

	if (videoGetState(0, 0, &state) != 0 || state.state != 0) return 0;
	if (videoGetResolution(state.displayMode.resolution, &res) != 0) return 0;

	display_width  = res.width;
	display_height = res.height;
	color_pitch    = display_width * sizeof(uint32_t); /* X8R8G8B8 = 4 B/px */
	depth_pitch    = display_width * sizeof(uint16_t); /* Z16        = 2 B/px */

	memset(&vcfg, 0, sizeof(vcfg));
	vcfg.resolution = state.displayMode.resolution;
	vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vcfg.pitch      = color_pitch;
	vcfg.aspect     = state.displayMode.aspect;
	if (videoConfigure(0, &vcfg, NULL, 0) != 0) return 0;

	cellGcmSetFlipMode(GCM_FLIP_HSYNC);

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

	CGparameter pos = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter col = cellGcmCgGetNamedParameter(vpo, "in_color");
	position_index  = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	color_index     = col ? (int)cellGcmCgGetParameterResource(vpo, col) - CG_ATTR0 : 3;

	aParam = cellGcmCgGetNamedParameter(fpo, "a");
	bParam = cellGcmCgGetNamedParameter(fpo, "b");
	cParam = cellGcmCgGetNamedParameter(fpo, "c");


	printf("  vp ucode: %u bytes; mvp uniform: %p\n", vpsize, (void *)modelViewProjConst);
	printf("  fp ucode: %u bytes (rsx-local at offset 0x%08x)\n", fpsize, fp_offset);
	printf("  vp attribs: position@%d color@%d\n", position_index, color_index);
	printf("  fp uniforms: a=%p b=%p c=%p\n",
	       (const void *)aParam, (const void *)bParam, (const void *)cParam);
}

static void init_geometry(void)
{
	vertex_buffer = (vertex_t *)local_align(128, 3 * sizeof(vertex_t));
	vertex_buffer[0] = (vertex_t){{ 0.0f,  0.6f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}};
	vertex_buffer[1] = (vertex_t){{-0.6f, -0.6f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
	vertex_buffer[2] = (vertex_t){{ 0.6f, -0.6f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);
}

static void program_exit(CellGcmContextData *ctx)
{
	cellGcmFinish(ctx, 1);
	u32 last = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) >> 8;
	while (g_buffer_on_display != last)
		usleep(1000);
}

/* ---------------------------------------------------------------- */
int main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	void               *host_addr;
	CellGcmContextData *ctx;

	printf("hello-ppu-cellgcm-chain: rsx-cg-compiler arithmetic-chain validation\n");

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

	g_readback = (u32 *)((char *)host_addr + (8 * 1024 * 1024));
	cellGcmAddressToOffset(g_readback, &g_readback_offset);

	ioPadInit(7);
	cellSysutilRegisterCallback(0, on_sysutil_event, NULL);

	init_shaders();
	init_geometry();

	/* First-time draw env / render state install + render-target set. */
	set_draw_env(ctx);
	set_render_state(ctx);
	set_render_target(ctx, g_frame_index);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	const int total = 120;  /* 2 s @ 60Hz */
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

		/* Yield the FIFO to the XMB while it owns the overlay. */
		if (g_drawing_paused) {
			usleep(16000);
			continue;
		}

		cellGcmSetClearColor(ctx, 0xff404040);
		cellGcmSetClearDepthStencil(ctx, 0xffffff00);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
			GCM_CLEAR_Z | GCM_CLEAR_S);

		/* Draw 1: left triangle with zero uniform bias (canonical RGB gradient) */
		static const float mvp_left[16] = {
			0.7f, 0.0f, 0.0f, -0.45f,
			0.0f, 0.7f, 0.0f,  0.0f,
			0.0f, 0.0f, 1.0f,  0.0f,
			0.0f, 0.0f, 0.0f,  1.0f,
		};
		static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		cellGcmSetVertexProgramParameter(ctx, modelViewProjConst, mvp_left);
		cellGcmSetFragmentProgram(ctx, fpo, fp_offset);
		cellGcmSetFragmentProgramParameter(ctx, fpo, aParam, zero, fp_offset);
		cellGcmSetFragmentProgramParameter(ctx, fpo, bParam, zero, fp_offset);
		cellGcmSetFragmentProgramParameter(ctx, fpo, cParam, zero, fp_offset);
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, 0, 3);

		/* Draw 2: right triangle with runtime-patched uniform biases (all channels lifted by +0.15) */
		static const float mvp_right[16] = {
			0.7f, 0.0f, 0.0f,  0.45f,
			0.0f, 0.7f, 0.0f,  0.0f,
			0.0f, 0.0f, 1.0f,  0.0f,
			0.0f, 0.0f, 0.0f,  1.0f,
		};
		static const float a_val[4] = { 0.15f, 0.0f,  0.0f,  0.0f };
		static const float b_val[4] = { 0.0f,  0.15f, 0.0f,  0.0f };
		static const float c_val[4] = { 0.0f,  0.0f,  0.15f, 0.0f };
		cellGcmSetVertexProgramParameter(ctx, modelViewProjConst, mvp_right);
		cellGcmSetFragmentProgram(ctx, fpo, fp_offset);
		cellGcmSetFragmentProgramParameter(ctx, fpo, aParam, a_val, fp_offset);
		cellGcmSetFragmentProgramParameter(ctx, fpo, bParam, b_val, fp_offset);
		cellGcmSetFragmentProgramParameter(ctx, fpo, cParam, c_val, fp_offset);
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, 0, 3);

		/* On frame 10, transfer back the rendered frame to host memory to verify
		 * that the between-draw uniform patch took effect in pixels.
		 * Calibrated probe coordinates assume 720p (1280x720); skip on other modes. */
		if (frame == 10) {
			if (display_width == 1280 && display_height == 720) {
				/* Transfer left half [0, 640) and right half [640, 1280) to avoid 1024-pixel tile splits */
				cellGcmSetTransferImage(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN,
				                        g_readback_offset, color_pitch, 0, 0,
				                        color_offset[g_frame_index], color_pitch, 0, 0,
				                        640, display_height, 4);
				cellGcmSetTransferImage(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN,
				                        g_readback_offset, color_pitch, 640, 0,
				                        color_offset[g_frame_index], color_pitch, 640, 0,
				                        640, display_height, 4);
				wait_rsx_idle(ctx);

				/* Background: center of screen (640, 360)
				 * Gradient probes along active scanline y=495:
				 *   - Green-dominant: Left (202, 495), Right (778, 495)
				 *   - Center:         Left (352, 495), Right (928, 495)
				 *   - Blue-dominant:  Left (502, 495), Right (1078, 495) */
				u32 px_bg      = g_readback[360 * display_width + 640];
				u32 px_l_mid   = g_readback[495 * display_width + 352];
				u32 px_r_mid   = g_readback[495 * display_width + 928];
				u32 px_l_green = g_readback[495 * display_width + 202];
				u32 px_r_green = g_readback[495 * display_width + 778];
				u32 px_l_blue  = g_readback[495 * display_width + 502];
				u32 px_r_blue  = g_readback[495 * display_width + 1078];

				int lm_r = (px_l_mid >> 16) & 0xff, lm_g = (px_l_mid >> 8) & 0xff, lm_b = px_l_mid & 0xff;
				int rm_r = (px_r_mid >> 16) & 0xff, rm_g = (px_r_mid >> 8) & 0xff, rm_b = px_r_mid & 0xff;

				int lg_r = (px_l_green >> 16) & 0xff, lg_g = (px_l_green >> 8) & 0xff, lg_b = px_l_green & 0xff;
				int rg_r = (px_r_green >> 16) & 0xff, rg_g = (px_r_green >> 8) & 0xff, rg_b = px_r_green & 0xff;

				int lb_r = (px_l_blue >> 16) & 0xff, lb_g = (px_l_blue >> 8) & 0xff, lb_b = px_l_blue & 0xff;
				int rb_r = (px_r_blue >> 16) & 0xff, rb_g = (px_r_blue >> 8) & 0xff, rb_b = px_r_blue & 0xff;

				printf("READBACK|frame=10|bg=0x%08x\n", px_bg);
				printf("READBACK|mid_left(352,495)=0x%08x (R=%d, G=%d, B=%d)\n", px_l_mid, lm_r, lm_g, lm_b);
				printf("READBACK|mid_right(928,495)=0x%08x (R=%d, G=%d, B=%d)\n", px_r_mid, rm_r, rm_g, rm_b);
				printf("READBACK|mid_delta=(dR=%+d, dG=%+d, dB=%+d) (expected +38, +38, +38)\n",
				       rm_r - lm_r, rm_g - lm_g, rm_b - lm_b);
				printf("READBACK|green_delta=(dR=%+d, dG=%+d, dB=%+d)\n",
				       rg_r - lg_r, rg_g - lg_g, rg_b - lg_b);
				printf("READBACK|blue_delta=(dR=%+d, dG=%+d, dB=%+d)\n",
				       rb_r - lb_r, rb_g - lb_g, rb_b - lb_b);
			} else {
				printf("READBACK|calibration skipped for non-720p resolution (%ux%u)\n",
				       display_width, display_height);
			}
		}

		flip(ctx);
		frame++;
	}

	printf("  drew %d frames\n", frame);
	if (g_exit_request) printf("  early exit on user request\n");

	program_exit(ctx);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-chain: done\n");
	return 0;
}
