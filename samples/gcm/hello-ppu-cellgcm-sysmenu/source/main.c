/*
 * hello-ppu-cellgcm-sysmenu — pause / resume on system-menu events.
 *
 * While the XMB overlay is closed the sample renders a spinning
 * triangle (Z-axis rotation, per-vertex colour).  When cellSysutil
 * delivers CELL_SYSUTIL_SYSTEM_MENU_OPEN the sample freezes the
 * rotation angle and draws a slowly-drifting "PAUSED" strip via
 * cellDbgFont.  CLOSE resumes normal rendering; EXITGAME exits the
 * main loop cleanly.
 *
 * Uses the simple cellGcmSetFlip + SetWaitFlip flip path so the
 * per-frame uniform upload path stays reliable under steady draw
 * load.
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

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2

#define GCM_LABEL_INDEX 255

typedef struct {
	float pos[3];
	float color[4];
} vertex_t;

typedef struct {
	uint32_t *ptr;
	uint32_t  offset;
	uint16_t  width;
	uint16_t  height;
	int       id;
} display_buffer;

static u32  depth_pitch;
static u32  depth_offset;
static u32 *depth_buffer;

/* CgBinaryProgram blobs from rsx-cg-compiler — consumed via the
 * cellGcmCg* / cellGcmSet* API so the runtime parses our CgBinary
 * header instead of mis-reading it through the legacy cgcomp
 * `rsxVertexProgram` struct. */
static CGprogram   vpo;
static CGprogram   fpo;
static void       *vp_ucode, *fp_ucode;
static u32         fp_offset;
static CGparameter transformConst;
static int         position_index;
static int         color_index;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;

static volatile int g_exit_request = 0;
static volatile int g_menu_open    = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata) {
	(void)param; (void)userdata;
	switch (status) {
	case CELL_SYSUTIL_REQUEST_EXITGAME:      g_exit_request = 1; break;
	case CELL_SYSUTIL_SYSTEM_MENU_OPEN:      g_menu_open    = 1; break;
	case CELL_SYSUTIL_SYSTEM_MENU_CLOSE:     g_menu_open    = 0; break;
	default: break;
	}
}

static void rotz_matrix(float angle, float out[16]) {
	float c = cosf(angle);
	float s = sinf(angle);
	out[ 0] =  c; out[ 1] = -s; out[ 2] = 0; out[ 3] = 0;
	out[ 4] =  s; out[ 5] =  c; out[ 6] = 0; out[ 7] = 0;
	out[ 8] =  0; out[ 9] =  0; out[10] = 1; out[11] = 0;
	out[12] =  0; out[13] =  0; out[14] = 0; out[15] = 1;
}

static void wait_rsx_idle(CellGcmContextData *ctx) {
	uint32_t label = 1;
	cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, label);
	cellGcmSetWaitLabel(ctx, GCM_LABEL_INDEX, label);
	label++;
	cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, label);
	cellGcmFlush(ctx);
	while (*(vu32 *)cellGcmGetLabelAddress(GCM_LABEL_INDEX) != label) usleep(30);
}
static void wait_flip(void) {
	while (cellGcmGetFlipStatus() != 0) usleep(200);
	cellGcmResetFlipStatus();
}
static int do_flip(CellGcmContextData *ctx, uint8_t buffer_id) {
	if (cellGcmSetFlip(ctx, buffer_id) == 0) {
		cellGcmFlush(ctx);
		cellGcmSetWaitFlip(ctx);
		return 1;
	}
	return 0;
}
static int make_buffer(display_buffer *b, uint16_t width, uint16_t height, int id) {
	int pitch = (int)(width * sizeof(uint32_t));
	int size  = pitch * height;
	b->ptr = (uint32_t *)rsxMemalign(64, size);
	if (!b->ptr) return 0;
	if (cellGcmAddressToOffset(b->ptr, &b->offset) != 0) return 0;
	if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, pitch, width, height) != 0) return 0;
	b->width = width; b->height = height; b->id = id;
	return 1;
}

static int init_screen(void *host_addr, uint32_t size, uint16_t *out_w, uint16_t *out_h) {
	videoState         state;
	videoConfiguration vcfg;
	videoResolution    res;

	if (cellGcmInit(CB_SIZE, size, host_addr) != 0) return 0;
	if (videoGetState(0, 0, &state) != 0 || state.state != 0) return 0;
	if (videoGetResolution(state.displayMode.resolution, &res) != 0) return 0;

	memset(&vcfg, 0, sizeof(vcfg));
	vcfg.resolution = state.displayMode.resolution;
	vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vcfg.pitch      = res.width * sizeof(uint32_t);
	vcfg.aspect     = state.displayMode.aspect;

	wait_rsx_idle(CELL_GCM_CURRENT);
	if (videoConfigure(0, &vcfg, NULL, 0) != 0) return 0;
	if (videoGetState(0, 0, &state) != 0) return 0;

	cellGcmSetFlipMode(GCM_FLIP_VSYNC);

	depth_pitch  = res.width * sizeof(uint32_t);
	depth_buffer = (u32 *)rsxMemalign(64, res.height * depth_pitch * 2);
	cellGcmAddressToOffset(depth_buffer, &depth_offset);

	cellGcmResetFlipStatus();
	*out_w = res.width;
	*out_h = res.height;
	return 1;
}

static void set_render_target(CellGcmContextData *ctx, display_buffer *b) {
	CellGcmSurface sf = {0};
	sf.colorFormat      = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = b->offset;
	sf.colorPitch[0]    = depth_pitch;
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
	sf.width            = b->width;
	sf.height           = b->height;
	cellGcmSetSurface(ctx, &sf);
}

static void set_draw_env(CellGcmContextData *ctx, uint32_t width, uint32_t height) {
	cellGcmSetColorMask(ctx,
		GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	cellGcmSetColorMaskMrt(ctx, 0);

	float min = 0.0f, max = 1.0f;
	float scale[4]  = { width * 0.5f, height * -0.5f, (max - min) * 0.5f, 0.0f };
	float offset[4] = { width * 0.5f, height *  0.5f, (max + min) * 0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, width, height, min, max, scale, offset);
	rsxSetScissor(ctx, 0, 0, width, height);

	cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetBlendEnable(ctx, GCM_FALSE);
}

static void init_shaders(void) {
	u32 vpsize = 0, fpsize = 0;
	vpo = (CGprogram)vpshader_vpo;
	fpo = (CGprogram)fpshader_fpo;

	cellGcmCgInitProgram(vpo);
	cellGcmCgInitProgram(fpo);

	cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);
	transformConst = cellGcmCgGetNamedParameter(vpo, "transform");

	void *fp_ucode_blob;
	cellGcmCgGetUCode(fpo, &fp_ucode_blob, &fpsize);
	fp_ucode = rsxMemalign(64, fpsize);
	memcpy(fp_ucode, fp_ucode_blob, fpsize);
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	CGparameter pos = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter col = cellGcmCgGetNamedParameter(vpo, "in_color");
	position_index  = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	color_index     = col ? (int)cellGcmCgGetParameterResource(vpo, col) - CG_ATTR0 : 3;
}

static void init_geometry(void) {
	vertex_buffer = (vertex_t *)rsxMemalign(128, 3 * sizeof(vertex_t));
	vertex_buffer[0] = (vertex_t){{ 0.0f,  0.55f, 0.0f}, {1.0f, 0.2f, 0.2f, 1.0f}};
	vertex_buffer[1] = (vertex_t){{-0.55f, -0.45f, 0.0f}, {0.2f, 1.0f, 0.2f, 1.0f}};
	vertex_buffer[2] = (vertex_t){{ 0.55f, -0.45f, 0.0f}, {0.2f, 0.4f, 1.0f, 1.0f}};
	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);
}

int main(int argc, const char **argv) {
	(void)argc; (void)argv;
	printf("hello-ppu-cellgcm-sysmenu: spinning-triangle + XMB pause/resume\n");

	void               *host_addr = memalign(1024 * 1024, HOST_SIZE);
	display_buffer      buffers[MAX_BUFFERS];
	int                 cur = 0;
	uint16_t            width = 0, height = 0;
	CellGcmContextData *ctx;

	if (!init_screen(host_addr, HOST_SIZE, &width, &height)) {
		printf("  init_screen failed\n");
		free(host_addr);
		return 1;
	}
	ctx = CELL_GCM_CURRENT;
	printf("  RSX up at %ux%u\n", width, height);

	ioPadInit(7);
	cellSysutilRegisterCallback(0, on_sysutil_event, NULL);

	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!make_buffer(&buffers[i], width, height, i)) {
			printf("  make_buffer[%d] failed\n", i);
			cellGcmFinish(ctx, 0);
			free(host_addr);
			return 1;
		}
	}
	do_flip(ctx, MAX_BUFFERS - 1);

	init_shaders();
	init_geometry();

	/* libdbgfont scratch buffer. */
	size_t dbg_buf_size = CELL_DBGFONT_FRAGMENT_SIZE
	                    + CELL_DBGFONT_TEXTURE_SIZE
	                    + 1024 * CELL_DBGFONT_VERTEX_SIZE;
	void  *dbg_local    = rsxMemalign(128, (u32)dbg_buf_size);
	CellDbgFontConfigGcm dbg_cfg = {0};
	dbg_cfg.localBufAddr = (sys_addr_t)(uintptr_t)dbg_local;
	dbg_cfg.localBufSize = (uint32_t)dbg_buf_size;
	dbg_cfg.option       = CELL_DBGFONT_VERTEX_LOCAL | CELL_DBGFONT_TEXTURE_LOCAL;
	cellDbgFontInitGcm(&dbg_cfg);

	float angle = 0.0f;   /* advances while menu is closed */
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
		if (paused && !was_paused_last) printf("  -> PAUSED\n");
		if (!paused && was_paused_last) printf("  -> RESUMED\n");
		was_paused_last = paused;

		set_render_target(ctx, &buffers[cur]);
		set_draw_env(ctx, width, height);

		cellGcmSetClearColor(ctx, paused ? 0xff202830 : 0xff181818);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		if (!paused) {
			/* Spinning triangle while the menu is closed. */
			float mtx[16];
			rotz_matrix(angle, mtx);

			cellGcmSetVertexProgram(ctx, vpo, vp_ucode);
			cellGcmSetVertexProgramParameter(ctx, transformConst, mtx);
			cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 3,
			                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
			                          vertex_buffer_offset + offsetof(vertex_t, pos));
			cellGcmSetVertexDataArray(ctx, color_index, 0, sizeof(vertex_t), 4,
			                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
			                          vertex_buffer_offset + offsetof(vertex_t, color));
			cellGcmSetFragmentProgram(ctx, fpo, fp_offset);

			rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, 0, 3);
			angle += 0.04f;
		} else {
			/* Paused overlay — slow drift on a small sinusoid plus a
			 * gentle colour pulse so it reads clearly. */
			float px    = 0.35f + 0.05f * sinf(t * 0.6f);
			float py    = 0.45f + 0.03f * sinf(t * 0.8f);
			float pulse = 0.5f + 0.5f * sinf(t * 1.2f);
			uint8_t g   = (uint8_t)(140 + 115 * pulse);
			uint32_t col =
				0xff000000u | (0xffu << 16) | ((uint32_t)g << 8) | 0x40u;

			cellDbgFontPrintf(px,         py,         2.0f, col,
			                  "== PAUSED ==");
			cellDbgFontPrintf(px + 0.04f, py + 0.08f, 1.0f, 0xffe0e0e0,
			                  "close the system menu to resume");
			cellDbgFontPrintf(0.05f,      0.90f,      0.8f, 0xffa0a0ff,
			                  "frame %d", frame);
		}

		cellDbgFontDrawGcm();

		wait_flip();
		do_flip(ctx, (uint8_t)buffers[cur].id);
		cur = (cur + 1) % MAX_BUFFERS;

		t += 1.0f / 60.0f;
		frame++;
	}

	printf("  exit requested; ran %d frames\n", frame);
	cellDbgFontExitGcm();

	cellGcmSetWaitFlip(ctx);
	for (int i = 0; i < MAX_BUFFERS; i++) rsxFree(buffers[i].ptr);
	cellGcmFinish(ctx, 1);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-sysmenu: done\n");
	return 0;
}
