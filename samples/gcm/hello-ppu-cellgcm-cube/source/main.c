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

/* CB_SIZE / HOST_SIZE chosen to match cellgcm-clear: 64 KB command
 * buffers and a 1 MB I/O map hit the FIFO-wrap drain-wait quickly on
 * draw-heavy samples (see docs/known-issues.md).  1 MB CB + 32 MB
 * I/O keep the wrap rare enough that the known stutter isn't
 * observable in practice. */
#define CB_SIZE              0x100000
#define HOST_SIZE            (32 * 1024 * 1024)
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

static u32 display_width, display_height;
static u32 color_pitch, depth_pitch;
static u32 color_offset[COLOR_BUFFER_NUM];
static u32 depth_offset;

static u32 g_local_mem_heap;
static void *local_alloc(u32 size) {
	u32 sz = (size + 1023) & ~1023u;
	u32 base = g_local_mem_heap;
	g_local_mem_heap += sz;
	return (void *)(uintptr_t)base;
}
static void *local_align(u32 alignment, u32 size) {
	g_local_mem_heap = (g_local_mem_heap + alignment - 1) & ~(alignment - 1);
	return local_alloc(size);
}

static rsxVertexProgram   *vpo;
static rsxFragmentProgram *fpo;
static void               *vp_ucode, *fp_ucode;
static u32                 fp_offset;
static rsxProgramConst    *mvpConst;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;
static u16      *index_buffer;
static u32       index_buffer_offset;

static volatile u32 g_buffer_on_display = 0;
static volatile u32 g_buffer_flipped    = 0;
static volatile u32 g_on_flip           = 0;
static u32          g_frame_index       = 0;

static volatile int g_exit_request = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata) {
	(void)param; (void)userdata;
	if (status == CELL_SYSUTIL_REQUEST_EXITGAME) g_exit_request = 1;
}

static void on_flip(const u32 head) {
	(void)head;
	u32 v = g_buffer_flipped;
	for (u32 i = g_buffer_on_display; i != v; i = (i + 1) % COLOR_BUFFER_NUM)
		*(volatile u32 *)cellGcmGetLabelAddress(LABEL_BUFFER_STATUS_OFFSET + i) = BUFFER_IDLE;
	g_buffer_on_display = v;
	g_on_flip = 0;
}

static void on_vblank(const u32 head) {
	(void)head;
	u32 data = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET);
	u32 buffer_to_flip = data >> 8;
	u32 index_to_flip  = data & 0x7;
	if (g_on_flip == 0 && buffer_to_flip != g_buffer_on_display) {
		g_on_flip = 1;
		if (cellGcmSetFlipImmediate((u8)index_to_flip) != 0) { g_on_flip = 0; return; }
		g_buffer_flipped = buffer_to_flip;
	}
}

static int ppu_too_fast(void) {
	u32 gpu = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) >> 8;
	return (((g_frame_index + COLOR_BUFFER_NUM - gpu) % COLOR_BUFFER_NUM) > MAX_QUEUE_FRAMES);
}

/* ---- matrix math ---------------------------------------------------- */

static void mat_identity(float m[16]) {
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_mul(const float a[16], const float b[16], float out[16]) {
	float r[16];
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += a[row * 4 + k] * b[k * 4 + col];
			r[row * 4 + col] = s;
		}
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

/* ---- render state --------------------------------------------------- */
static void set_render_target(CellGcmContextData *ctx, u32 idx) {
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

static void set_draw_env(CellGcmContextData *ctx) {
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

static void set_render_state(CellGcmContextData *ctx, const float mvp[16]) {
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
}

static void flip_and_setup_next(CellGcmContextData *ctx, const float mvp[16]) {
	s32 qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index);
	while (qid < 0) { usleep(1000); qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index); }

	cellGcmSetWriteBackEndLabel(ctx, LABEL_PREPARED_BUFFER_OFFSET,
	                            (g_frame_index << 8) | (u32)qid);
	cellGcmFlush(ctx);

	while (ppu_too_fast()) usleep(3000);

	set_draw_env(ctx);
	set_render_state(ctx, mvp);

	g_frame_index = (g_frame_index + 1) % COLOR_BUFFER_NUM;
	cellGcmSetWaitLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_IDLE);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);
	set_render_target(ctx, g_frame_index);
}

/* ---- init ----------------------------------------------------------- */
static int init_display(void) {
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

static void init_shaders(void) {
	u32 vpsize = 0, fpsize = 0;
	vpo = (rsxVertexProgram   *)vpshader_vpo;
	fpo = (rsxFragmentProgram *)fpshader_fpo;

	rsxVertexProgramGetUCode(vpo, &vp_ucode, &vpsize);
	mvpConst = rsxVertexProgramGetConst(vpo, "modelViewProj");

	void *fp_ucode_blob;
	rsxFragmentProgramGetUCode(fpo, &fp_ucode_blob, &fpsize);
	fp_ucode = local_align(64, fpsize);
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
	vertex_buffer = (vertex_t *)local_align(128, sizeof(corners));
	memcpy(vertex_buffer, corners, sizeof(corners));
	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);

	/* 12 triangles, 36 indices.  Winding is CCW when looking at the
	 * outside of the cube from a right-handed coordinate system. */
	static const u16 idx[36] = {
		0, 2, 1,   0, 3, 2,   /* -Z */
		4, 5, 6,   4, 6, 7,   /* +Z */
		0, 4, 7,   0, 7, 3,   /* -X */
		1, 2, 6,   1, 6, 5,   /* +X */
		0, 1, 5,   0, 5, 4,   /* -Y */
		3, 7, 6,   3, 6, 2,   /* +Y */
	};
	index_buffer = (u16 *)local_align(128, sizeof(idx));
	memcpy(index_buffer, idx, sizeof(idx));
	cellGcmAddressToOffset(index_buffer, &index_buffer_offset);
}

static void program_exit(CellGcmContextData *ctx) {
	cellGcmFinish(ctx, 1);
	u32 last = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) >> 8;
	while (g_buffer_on_display != last) usleep(1000);
}

/* ---- main ----------------------------------------------------------- */
int main(int argc, const char **argv) {
	(void)argc; (void)argv;
	printf("hello-ppu-cellgcm-cube: starting\n");

	void               *host_addr = memalign(1024 * 1024, HOST_SIZE);
	CellGcmContextData *ctx;
	if (cellGcmInit(CB_SIZE, HOST_SIZE, host_addr) != 0) { free(host_addr); return 1; }
	if (!init_display())                                 { free(host_addr); return 1; }
	ctx = CELL_GCM_CURRENT;
	printf("  RSX up at %ux%u\n", display_width, display_height);

	ioPadInit(7);
	cellSysutilRegisterCallback(0, on_sysutil_event, NULL);

	init_shaders();
	init_geometry();

	float mvp[16], proj[16], view[16], rx[16], ry[16], rot[16], mv[16];
	mat_perspective(1.0472f, (float)display_width / (float)display_height, 0.1f, 100.0f, proj);
	mat_translate(0.0f, 0.0f, -2.5f, view);

	mat_identity(mvp);

	set_draw_env(ctx);
	set_render_state(ctx, mvp);
	set_render_target(ctx, g_frame_index);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	const int total = 60 * 12;   /* ~12 s */
	int       frame = 0;
	float     a = 0.0f;

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

		cellGcmSetClearColor(ctx, 0xff181824);
		cellGcmSetClearDepthStencil(ctx, 0xffffff00);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
			GCM_CLEAR_Z | GCM_CLEAR_S);

		rsxSetVertexProgramParameter(ctx, vpo, mvpConst, mvp);
		rsxDrawIndexArray(ctx, GCM_TYPE_TRIANGLES,
		                  index_buffer_offset, 36,
		                  GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);

		flip_and_setup_next(ctx, mvp);
		a += 0.025f;
		frame++;
	}

	printf("  drew %d frames\n", frame);
	program_exit(ctx);
	free(host_addr);
	ioPadEnd();
	printf("hello-ppu-cellgcm-cube: done\n");
	return 0;
}
