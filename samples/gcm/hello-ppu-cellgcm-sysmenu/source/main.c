/*
 * hello-ppu-cellgcm-sysmenu — pause / resume on system-menu events.
 *
 * While the XMB overlay is closed the sample renders a spinning
 * triangle (Z-axis rotation, per-vertex colour).  When cellSysutil
 * delivers CELL_SYSUTIL_SYSTEM_MENU_OPEN the sample freezes the
 * rotation angle, stops drawing the triangle, and draws an animated
 * "PAUSED" text strip that drifts across the screen via cellDbgFont.
 * CLOSE resumes normal rendering; EXITGAME (XMB → quit) exits the
 * main loop cleanly.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <math.h>

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sysutil/video.h>
#include <io/pad.h>
#include <cell/gcm.h>
#include <cell/dbgfont.h>
#include <cell/sysutil.h>
#include <rsx/rsx.h>

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

static rsxVertexProgram   *vpo;
static rsxFragmentProgram *fpo;
static void               *vp_ucode;
static void               *fp_ucode;
static u32                 fp_offset;
static rsxProgramConst    *transformConst;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;

static volatile u32 g_buffer_on_display = 0;
static volatile u32 g_buffer_flipped    = 0;
static volatile u32 g_on_flip           = 0;
static u32          g_frame_index       = 0;

/* Sysutil state. */
static volatile int g_exit_request = 0;
static volatile int g_menu_open    = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata)
{
	(void)param;
	(void)userdata;
	switch (status) {
	case CELL_SYSUTIL_REQUEST_EXITGAME:
		g_exit_request = 1;
		break;
	case CELL_SYSUTIL_SYSTEM_MENU_OPEN:
		g_menu_open = 1;
		break;
	case CELL_SYSUTIL_SYSTEM_MENU_CLOSE:
		g_menu_open = 0;
		break;
	default:
		break;
	}
}

/* VBlank + flip handlers. */
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
		if (cellGcmSetFlipImmediate((u8)index_to_flip) != 0) {
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

/* Render-target + draw-env + render-state. */
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

/* Rotation-about-Z matrix, column-major-as-rows (matches what the
 * vertex program's mul(transform, v) expects). */
static void rotz_matrix(float angle, float out[16])
{
	float c = cosf(angle);
	float s = sinf(angle);
	out[ 0] =  c; out[ 1] = -s; out[ 2] = 0; out[ 3] = 0;
	out[ 4] =  s; out[ 5] =  c; out[ 6] = 0; out[ 7] = 0;
	out[ 8] =  0; out[ 9] =  0; out[10] = 1; out[11] = 0;
	out[12] =  0; out[13] =  0; out[14] = 0; out[15] = 1;
}

static void set_render_state(CellGcmContextData *ctx, float angle)
{
	float mtx[16];
	rotz_matrix(angle, mtx);

	rsxLoadVertexProgram(ctx, vpo, vp_ucode);
	rsxSetVertexProgramParameter(ctx, vpo, transformConst, mtx);

	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0,
	                         vertex_buffer_offset + offsetof(vertex_t, pos),
	                         sizeof(vertex_t), 3,
	                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0,
	                         vertex_buffer_offset + offsetof(vertex_t, color),
	                         sizeof(vertex_t), 4,
	                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	rsxLoadFragmentProgramLocation(ctx, fpo, fp_offset, GCM_LOCATION_RSX);
}

static void flip(CellGcmContextData *ctx, float angle)
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
	set_render_state(ctx, angle);

	g_frame_index = (g_frame_index + 1) % COLOR_BUFFER_NUM;
	cellGcmSetWaitLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_IDLE);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	set_render_target(ctx, g_frame_index);
}

/* Init helpers. */
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
	vpo = (rsxVertexProgram   *)vpshader_vpo;
	fpo = (rsxFragmentProgram *)fpshader_fpo;

	rsxVertexProgramGetUCode(vpo, &vp_ucode, &vpsize);
	transformConst = rsxVertexProgramGetConst(vpo, "transform");

	void *fp_ucode_blob;
	rsxFragmentProgramGetUCode(fpo, &fp_ucode_blob, &fpsize);
	fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_ucode_blob, fpsize);
	cellGcmAddressToOffset(fp_ucode, &fp_offset);
}

static void init_geometry(void)
{
	vertex_buffer = (vertex_t *)local_align(128, 3 * sizeof(vertex_t));
	vertex_buffer[0] = (vertex_t){{ 0.0f,  0.55f, 0.0f}, {1.0f, 0.2f, 0.2f, 1.0f}};
	vertex_buffer[1] = (vertex_t){{-0.55f, -0.45f, 0.0f}, {0.2f, 1.0f, 0.2f, 1.0f}};
	vertex_buffer[2] = (vertex_t){{ 0.55f, -0.45f, 0.0f}, {0.2f, 0.4f, 1.0f, 1.0f}};
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
	(void)argc; (void)argv;

	printf("hello-ppu-cellgcm-sysmenu: spinning-triangle + XMB pause/resume\n");

	void               *host_addr = memalign(1024 * 1024, HOST_SIZE);
	CellGcmContextData *ctx;

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

	/* libdbgfont scratch buffer. */
	size_t dbg_buf_size = CELL_DBGFONT_FRAGMENT_SIZE
	                    + CELL_DBGFONT_TEXTURE_SIZE
	                    + 1024 * CELL_DBGFONT_VERTEX_SIZE;
	void  *dbg_local    = local_align(128, (u32)dbg_buf_size);

	CellDbgFontConfigGcm dbg_cfg = {0};
	dbg_cfg.localBufAddr = (sys_addr_t)(uintptr_t)dbg_local;
	dbg_cfg.localBufSize = (uint32_t)dbg_buf_size;
	dbg_cfg.option       = CELL_DBGFONT_VERTEX_LOCAL | CELL_DBGFONT_TEXTURE_LOCAL;
	cellDbgFontInitGcm(&dbg_cfg);

	set_draw_env(ctx);
	set_render_state(ctx, 0.0f);
	set_render_target(ctx, g_frame_index);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	float angle = 0.0f;   /* advances while the menu is closed */
	float t     = 0.0f;   /* always advances — animates PAUSED text */
	int   frame = 0;
	int   was_paused_last = 0;

	while (!g_exit_request) {
		cellSysutilCheckCallback();

		padInfo padinfo;
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				padData paddata;
				ioPadGetData(i, &paddata);
				if (paddata.BTN_START) { g_exit_request = 1; break; }
			}
		}

		int paused = g_menu_open;
		if (paused && !was_paused_last) printf("  -> PAUSED (system menu open)\n");
		if (!paused && was_paused_last) printf("  -> RESUMED\n");
		was_paused_last = paused;

		cellGcmSetClearColor(ctx, paused ? 0xff202830 : 0xff181818);
		cellGcmSetClearDepthStencil(ctx, 0xffffff00);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
			GCM_CLEAR_Z | GCM_CLEAR_S);

		if (!paused) {
			angle += 0.04f;   /* ~0.6 rad/s at 60 fps */
			rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, 0, 3);
		} else {
			/* Animated overlay while the XMB is up.  Position drifts
			 * on a small Lissajous figure; colour pulses between two
			 * tints so the text is visibly "alive" even though the
			 * underlying scene is frozen. */
			float px = 0.35f + 0.18f * sinf(t * 2.1f);
			float py = 0.45f + 0.08f * sinf(t * 2.8f + 1.2f);
			float pulse = 0.5f + 0.5f * sinf(t * 4.0f);
			uint8_t g = (uint8_t)(120 + 135 * pulse);
			uint32_t col = 0xff000000u | (0xffu << 16) | ((uint32_t)g << 8) | 0x40u;

			cellDbgFontPrintf(px,          py,          2.0f, col,
			                  "== PAUSED ==");
			cellDbgFontPrintf(px + 0.02f,  py + 0.06f,  1.0f, 0xffe0e0e0,
			                  "close the system menu to resume");
			cellDbgFontPrintf(0.02f,       0.92f,       0.8f, 0xffa0a0ff,
			                  "frame %d   t=%.2f", frame, t);
		}

		cellDbgFontDrawGcm();

		flip(ctx, paused ? angle : angle);  /* same matrix — frozen or fresh */

		t += 1.0f / 60.0f;
		frame++;
	}

	printf("  exit requested; ran %d frames\n", frame);
	cellDbgFontExitGcm();
	program_exit(ctx);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-sysmenu: done\n");
	return 0;
}
