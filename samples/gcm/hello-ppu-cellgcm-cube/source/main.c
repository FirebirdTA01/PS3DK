/*
 * hello-ppu-cellgcm-cube — rotating, depth-tested 3D cube.
 *
 * Geometry: 8 corner vertices (each tinted a different corner-colour)
 * plus a 36-entry index buffer so each of the six faces draws as two
 * indexed triangles.  Depth test is on, so back-facing geometry is
 * naturally occluded without culling.
 *
 * Transform: perspective * view * rotation-about-XY.  All matrix math
 * is done on the PPU with plain float arithmetic; the GPU just lerps
 * colours and runs the fragment program.
 *
 * Uses the simple cellGcmSetFlip + SetWaitFlip flip path (same as
 * cellgcm-clear); the label-based flip_immediate pattern is left to
 * the triangle / sysmenu samples.
 *
 * Exits after ~12 seconds, or on XMB quit, or on START.
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

static rsxVertexProgram   *vpo;
static rsxFragmentProgram *fpo;
static void               *vp_ucode, *fp_ucode;
static u32                 fp_offset;
static rsxProgramConst    *mvpConst;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;
static u16      *index_buffer;
static u32       index_buffer_offset;

static volatile int g_exit_request = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata) {
	(void)param; (void)userdata;
	if (status == CELL_SYSUTIL_REQUEST_EXITGAME) g_exit_request = 1;
}

/* ---- matrix math ---------------------------------------------------- */
static void mat_identity(float m[16]) {
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}
static void mat_mul(const float a[16], const float b[16], float out[16]) {
	float r[16];
	for (int row = 0; row < 4; row++)
		for (int col = 0; col < 4; col++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++) s += a[row * 4 + k] * b[k * 4 + col];
			r[row * 4 + col] = s;
		}
	memcpy(out, r, sizeof(r));
}
static void mat_perspective(float fov_rad, float aspect, float zn, float zf, float m[16]) {
	float f = 1.0f / tanf(fov_rad * 0.5f);
	memset(m, 0, 16 * sizeof(float));
	m[ 0] = f / aspect;
	m[ 5] = f;
	m[10] = (zf + zn) / (zn - zf);
	m[11] = (2.0f * zf * zn) / (zn - zf);
	m[14] = -1.0f;
}
static void mat_translate(float tx, float ty, float tz, float m[16]) {
	mat_identity(m);
	m[ 3] = tx;
	m[ 7] = ty;
	m[11] = tz;
}
static void mat_rot_x(float a, float m[16]) {
	mat_identity(m);
	float c = cosf(a), s = sinf(a);
	m[ 5] = c; m[ 6] = -s;
	m[ 9] = s; m[10] =  c;
}
static void mat_rot_y(float a, float m[16]) {
	mat_identity(m);
	float c = cosf(a), s = sinf(a);
	m[ 0] =  c; m[ 2] = s;
	m[ 8] = -s; m[10] = c;
}

/* ---- simple-flip helpers (same pattern as cellgcm-clear) ------------ */
static void wait_rsx_idle(CellGcmContextData *ctx)
{
	uint32_t label = 1;
	cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, label);
	cellGcmSetWaitLabel(ctx, GCM_LABEL_INDEX, label);
	label++;
	cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, label);
	cellGcmFlush(ctx);
	while (*(vu32 *)cellGcmGetLabelAddress(GCM_LABEL_INDEX) != label)
		usleep(30);
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

static int init_screen(void *host_addr, uint32_t size, uint16_t *out_w, uint16_t *out_h)
{
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

	cellGcmSetDepthTestEnable(ctx, GCM_TRUE);
	cellGcmSetDepthFunc(ctx, GCM_LESS);
	cellGcmSetDepthMask(ctx, GCM_TRUE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetBlendEnable(ctx, GCM_FALSE);
}

static void init_shaders(void) {
	u32 vpsize = 0, fpsize = 0;
	vpo = (rsxVertexProgram   *)vpshader_vpo;
	fpo = (rsxFragmentProgram *)fpshader_fpo;

	rsxVertexProgramGetUCode(vpo, &vp_ucode, &vpsize);
	mvpConst = rsxVertexProgramGetConst(vpo, "modelViewProj");

	void *fp_ucode_blob;
	rsxFragmentProgramGetUCode(fpo, &fp_ucode_blob, &fpsize);
	/* Put fp ucode in RSX local mem — same approach as cellgcm-triangle. */
	fp_ucode = rsxMemalign(64, fpsize);
	memcpy(fp_ucode, fp_ucode_blob, fpsize);
	cellGcmAddressToOffset(fp_ucode, &fp_offset);
}

static void init_geometry(void) {
	/* 8 corners of a unit-ish cube; each corner tinted so the six
	 * faces have distinct colour gradients. */
	static const vertex_t corners[8] = {
		{{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, 0.0f, 1.0f}},
		{{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f, 1.0f}},
		{{ 0.5f,  0.5f, -0.5f}, {1.0f, 1.0f, 0.0f, 1.0f}},
		{{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f, 1.0f}},
		{{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}},
		{{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 1.0f, 1.0f}},
		{{ 0.5f,  0.5f,  0.5f}, {1.0f, 1.0f, 1.0f, 1.0f}},
		{{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 1.0f, 1.0f}},
	};
	vertex_buffer = (vertex_t *)rsxMemalign(128, sizeof(corners));
	memcpy(vertex_buffer, corners, sizeof(corners));
	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);

	static const u16 idx[36] = {
		0, 2, 1,   0, 3, 2,   /* -Z */
		4, 5, 6,   4, 6, 7,   /* +Z */
		0, 4, 7,   0, 7, 3,   /* -X */
		1, 2, 6,   1, 6, 5,   /* +X */
		0, 1, 5,   0, 5, 4,   /* -Y */
		3, 7, 6,   3, 6, 2,   /* +Y */
	};
	index_buffer = (u16 *)rsxMemalign(128, sizeof(idx));
	memcpy(index_buffer, idx, sizeof(idx));
	cellGcmAddressToOffset(index_buffer, &index_buffer_offset);
}

int main(int argc, const char **argv) {
	(void)argc; (void)argv;
	printf("hello-ppu-cellgcm-cube: starting\n");

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

	float mvp[16], proj[16], view[16], rx[16], ry[16], rot[16], mv[16];
	mat_perspective(1.0472f, (float)width / (float)height, 0.1f, 100.0f, proj);
	mat_translate(0.0f, 0.0f, -2.5f, view);

	const int total = 60 * 12;
	int       frame = 0;
	float     a     = 0.0f;

	while (frame < total && !g_exit_request) {
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

		mat_rot_x(a * 0.7f, rx);
		mat_rot_y(a,        ry);
		mat_mul(ry, rx, rot);
		mat_mul(view, rot, mv);
		mat_mul(proj, mv, mvp);

		set_render_target(ctx, &buffers[cur]);
		set_draw_env(ctx, width, height);

		cellGcmSetClearColor(ctx, 0xff181824);
		cellGcmSetClearDepthStencil(ctx, 0xffffff00);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
			GCM_CLEAR_Z | GCM_CLEAR_S);

		rsxLoadVertexProgram(ctx, vpo, vp_ucode);
		rsxSetVertexProgramParameter(ctx, vpo, mvpConst, mvp);
		rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0,
		                         vertex_buffer_offset + offsetof(vertex_t, pos),
		                         sizeof(vertex_t), 3,
		                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
		rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0,
		                         vertex_buffer_offset + offsetof(vertex_t, color),
		                         sizeof(vertex_t), 4,
		                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
		rsxLoadFragmentProgramLocation(ctx, fpo, fp_offset, GCM_LOCATION_RSX);

		rsxDrawIndexArray(ctx, GCM_TYPE_TRIANGLES,
		                  index_buffer_offset, 36,
		                  GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);

		wait_flip();
		do_flip(ctx, (uint8_t)buffers[cur].id);
		cur = (cur + 1) % MAX_BUFFERS;

		a += 0.025f;
		frame++;
	}

	printf("  drew %d frames\n", frame);

	cellGcmSetWaitFlip(ctx);
	for (int i = 0; i < MAX_BUFFERS; i++) rsxFree(buffers[i].ptr);
	cellGcmFinish(ctx, 1);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-cube: done\n");
	return 0;
}
