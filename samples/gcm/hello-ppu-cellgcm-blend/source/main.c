/*
 * hello-ppu-cellgcm-blend — alpha-blend visual demo + emitter validation.
 *
 * Renders three overlapping translucent quads (red / green / blue, each
 * with alpha 0.5) over a dark-grey background to demonstrate
 * source-over alpha blending.
 *
 *   ┌─────────┐
 *   │  red    │── translucent red, alpha=0.5
 *   │   ┌─────┼─────┐
 *   │   │ R+B │  blue
 *   │   │  +G │
 *   │   │  +R ├─────┐
 *   │   └─────┤   green
 *   └────────┐│
 *            │└──────┘
 *            └─────── translucent green, alpha=0.5
 *
 * Visible regions show:
 *   - background only (dark grey)
 *   - dark grey + red blended  (= 50% grey + 50% red, dim red)
 *   - dark grey + green
 *   - dark grey + blue
 *   - red + blue overlap (purple-ish)
 *   - green + blue overlap (teal-ish)
 *   - red + green + blue triple-stack (mid grey-ish, all channels)
 *
 * Exercises the new cell-named blend emitters added to our SDK:
 *   cellGcmSetBlendFunc         — 4-arg + 5-arg overloads
 *   cellGcmSetBlendEquation     — 2-arg + 3-arg overloads
 *   cellGcmSetBlendColor        — set the constant blend factor
 *   cellGcmSetLogicOp           — opcode setter (off here, but linked)
 *   cellGcmSetLogicOpEnable     — on/off toggle
 *
 * Plus the CELL_GCM_SRC_ALPHA / FUNC_ADD / etc. constants exposed
 * via gcm_enum.h.  Render plumbing and flip protocol mirror
 * hello-ppu-cellgcm-triangle.
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
/* Migration note (mirrored across all gcm samples): shaders now go
 * through rsx-cg-compiler (CgBinaryProgram container output) and load
 * via the cellGcmCg* / cellGcmSet* runtime API.  The legacy rsx*Program
 * loaders are not deprecated — only the cgcomp compiler + cg.dll are
 * scheduled for removal once rsx-cg-compiler covers all sample shader
 * features.  After cgcomp retirement, rsx*Program* will dispatch to
 * the same CgBinaryProgram parsing as cellGcmCg* below.  Old call
 * sites kept commented for visual API parity. */

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
static int         position_index;
static int         color_index;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;

static volatile u32 g_buffer_on_display = 0;
static volatile u32 g_buffer_flipped    = 0;
static volatile u32 g_on_flip           = 0;
static u32          g_frame_index       = 0;

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

/* Set up the render env every frame.  Blend state is what makes this
 * sample interesting — we issue every new emitter our SDK shipped:
 * SetBlendFunc, SetBlendEquation, SetBlendColor, SetLogicOp,
 * SetLogicOpEnable.  Depth-test stays off so paint order alone
 * decides what's on top, which is the standard alpha-blend setup. */
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
	cellGcmSetDepthMask(ctx, GCM_FALSE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);

	/* === blend state === */
	cellGcmSetBlendEnable(ctx, GCM_TRUE);
	cellGcmSetBlendFunc(ctx,
	                    CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA,
	                    CELL_GCM_SRC_ALPHA, CELL_GCM_ONE_MINUS_SRC_ALPHA);
	cellGcmSetBlendEquation(ctx, CELL_GCM_FUNC_ADD, CELL_GCM_FUNC_ADD);
	cellGcmSetBlendColor(ctx, 0x80808080u, 0x80808080u);

	/* logic op stays disabled (it would override blending if on) */
	cellGcmSetLogicOp(ctx, CELL_GCM_COPY);
	cellGcmSetLogicOpEnable(ctx, GCM_FALSE);
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
	/*   rsxLoadVertexProgram(ctx, vpo, vp_ucode);
	 *   rsxSetVertexProgramParameter(ctx, vpo, modelViewProjConst, mvp);
	 */

	cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 3,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, color_index, 0, sizeof(vertex_t), 4,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, color));
	/*   rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0,
	 *                            vertex_buffer_offset + offsetof(vertex_t, pos),
	 *                            sizeof(vertex_t), 3,
	 *                            GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	 *   rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0,
	 *                            vertex_buffer_offset + offsetof(vertex_t, color),
	 *                            sizeof(vertex_t), 4,
	 *                            GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	 */

	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);
	/*   rsxLoadFragmentProgramLocation(ctx, fpo, fp_offset, GCM_LOCATION_RSX); */
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
	/*   vpo = (rsxVertexProgram   *)vpshader_vpo;
	 *   fpo = (rsxFragmentProgram *)fpshader_fpo;
	 */

	cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);
	modelViewProjConst = cellGcmCgGetNamedParameter(vpo, "modelViewProj");
	/*   rsxVertexProgramGetUCode(vpo, &vp_ucode, &vpsize);
	 *   modelViewProjConst = rsxVertexProgramGetConst(vpo, "modelViewProj");
	 */

	void *fp_ucode_blob;
	cellGcmCgGetUCode(fpo, &fp_ucode_blob, &fpsize);
	/*   rsxFragmentProgramGetUCode(fpo, &fp_ucode_blob, &fpsize); */
	fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_ucode_blob, fpsize);
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	CGparameter pos = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter col = cellGcmCgGetNamedParameter(vpo, "in_color");
	position_index  = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	color_index     = col ? (int)cellGcmCgGetParameterResource(vpo, col) - CG_ATTR0 : 3;

	printf("  vp ucode: %u bytes; mvp uniform: %p\n", vpsize, (void *)modelViewProjConst);
	printf("  fp ucode: %u bytes (rsx-local at offset 0x%08x)\n", fpsize, fp_offset);
	printf("  vp attribs: position@%d color@%d\n", position_index, color_index);
}

/* 12 vertices = 3 quads × 4 verts (TRIANGLE_STRIP layout TL/TR/BL/BR
 * gives the right winding when drawn as a strip).  Quads slightly
 * offset so each pair overlaps with the third overlapping both. */
static void init_geometry(void)
{
	const float a = 0.5f;  /* alpha */

	vertex_buffer = (vertex_t *)local_align(128, 12 * sizeof(vertex_t));

	/* Quad 0 — RED, upper-left */
	vertex_buffer[0]  = (vertex_t){{-0.7f,  0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, a}};
	vertex_buffer[1]  = (vertex_t){{ 0.1f,  0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, a}};
	vertex_buffer[2]  = (vertex_t){{-0.7f, -0.3f, 0.0f}, {1.0f, 0.0f, 0.0f, a}};
	vertex_buffer[3]  = (vertex_t){{ 0.1f, -0.3f, 0.0f}, {1.0f, 0.0f, 0.0f, a}};

	/* Quad 1 — GREEN, lower-right */
	vertex_buffer[4]  = (vertex_t){{-0.1f,  0.3f, 0.0f}, {0.0f, 1.0f, 0.0f, a}};
	vertex_buffer[5]  = (vertex_t){{ 0.7f,  0.3f, 0.0f}, {0.0f, 1.0f, 0.0f, a}};
	vertex_buffer[6]  = (vertex_t){{-0.1f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, a}};
	vertex_buffer[7]  = (vertex_t){{ 0.7f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f, a}};

	/* Quad 2 — BLUE, centre, sized to overlap both above */
	vertex_buffer[8]  = (vertex_t){{-0.4f,  0.4f, 0.0f}, {0.0f, 0.4f, 1.0f, a}};
	vertex_buffer[9]  = (vertex_t){{ 0.4f,  0.4f, 0.0f}, {0.0f, 0.4f, 1.0f, a}};
	vertex_buffer[10] = (vertex_t){{-0.4f, -0.4f, 0.0f}, {0.0f, 0.4f, 1.0f, a}};
	vertex_buffer[11] = (vertex_t){{ 0.4f, -0.4f, 0.0f}, {0.0f, 0.4f, 1.0f, a}};

	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);
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
	(void)argc;
	(void)argv;

	void               *host_addr;
	CellGcmContextData *ctx;

	printf("hello-ppu-cellgcm-blend: alpha-blended quads + new emitters\n");

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
	init_geometry();

	set_draw_env(ctx);
	set_render_state(ctx);
	set_render_target(ctx, g_frame_index);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	const int total = 600;  /* 10 s @ 60Hz */
	int       frame = 0;

	while (frame < total && !g_exit_request) {
		cellSysutilCheckCallback();

		padInfo padinfo;
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				padData paddata;
				ioPadGetData(i, &paddata);
				if (paddata.BTN_START) {
					g_exit_request = 1;
					break;
				}
			}
		}

		if (g_drawing_paused) {
			usleep(16000);
			continue;
		}

		/* Dark-grey background — colour visible "behind" the blended quads. */
		cellGcmSetClearColor(ctx, 0xff202028);
		cellGcmSetClearDepthStencil(ctx, 0xffffff00);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
			GCM_CLEAR_Z | GCM_CLEAR_S);

		/* Draw the 3 quads as separate TRIANGLE_STRIP draws so they
		 * blend correctly against each other. */
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_STRIP, 4, 4);
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_STRIP, 8, 4);

		flip(ctx);
		frame++;
	}

	printf("  drew %d frames\n", frame);
	if (g_exit_request) printf("  early exit on user request\n");

	program_exit(ctx);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-blend: done\n");
	return 0;
}
