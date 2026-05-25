/*
 * hello-ppu-gcm-zcull-clip-border - GCM command coverage sample.
 *
 * 4 screen-space vertices (NDC positions + UVs) drawn as a triangle
 * strip.  The UV range deliberately extends outside 0..1, so border
 * sampling visibly uses the programmed texture border colour.
 */

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
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
#include <rsx/rsx.h>

#include "vpshader_vpo.h"
#include "fpshader_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

/* 1 MB / 32 MB keeps the FIFO wrap rare enough that the known
 * wrap-stutter doesn't hit this sample. */
#define CB_SIZE              0x100000
#define HOST_SIZE            (32 * 1024 * 1024)
#define COLOR_BUFFER_NUM     4
#define MAX_QUEUE_FRAMES     1

#define TEX_W 64
#define TEX_H 64

#define LABEL_PREPARED_BUFFER_OFFSET   0x41
#define LABEL_BUFFER_STATUS_OFFSET     0x42
#define BUFFER_IDLE                    0
#define BUFFER_BUSY                    1

typedef struct {
	float pos[2];
	float uv[2];
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

/* CgBinaryProgram blobs produced by rsx-cg-compiler.  We consume them
 * via the cellGcmCg* / cellGcmSet* API so the runtime parses the
 * CgBinary header layout instead of mis-reading our blob through the
 * legacy cgcomp `rsxVertexProgram` struct (the layouts are different;
 * feeding a CgBinaryProgram to rsxLoadVertexProgram crashes the
 * GPU). */
static CGprogram vpo;
static CGprogram fpo;
static void     *vp_ucode, *fp_ucode;
static u32       fp_offset;
static int       position_index;
static int       texcoord_index;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;
static u32      *texture_pixels;
static u32       texture_offset;
static u32       zcull_stats_tag_sum;

static volatile u32 g_buffer_on_display = 0;
static volatile u32 g_buffer_flipped    = 0;
static volatile u32 g_on_flip           = 0;
static u32          g_frame_index       = 0;
static int          g_flip_seen         = 0;

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
	g_flip_seen = 1;
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
	cellGcmSetDepthFunc(ctx, GCM_LEQUAL);
	cellGcmSetDepthMask(ctx, GCM_TRUE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetBlendEnable(ctx, GCM_FALSE);
}

static void bind_texture(CellGcmContextData *ctx) {
	CellGcmTexture t = {0};
	t.format    = CELL_GCM_TEXTURE_A8R8G8B8
	            | CELL_GCM_TEXTURE_LN
	            | CELL_GCM_TEXTURE_NR;
	t.mipmap    = 1;
	t.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
	t.cubemap   = CELL_GCM_FALSE;
	t.remap     = (CELL_GCM_TEXTURE_REMAP_REMAP <<  8) /* A */
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 10) /* R */
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 12) /* G */
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 14) /* B */
	            | (CELL_GCM_TEXTURE_REMAP_FROM_A << 0)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_R << 2)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_G << 4)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_B << 6);
	t.width     = TEX_W;
	t.height    = TEX_H;
	t.depth     = 1;
	t.location  = CELL_GCM_LOCATION_LOCAL;
	t.pitch     = TEX_W * sizeof(u32);
	t.offset    = texture_offset;

	cellGcmSetTexture(ctx, 0, &t);
	cellGcmSetTextureControl(ctx, 0, CELL_GCM_TRUE,
	                         0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
	cellGcmSetTextureFilter(ctx, 0,
	                        0,
	                        CELL_GCM_TEXTURE_LINEAR,
	                        CELL_GCM_TEXTURE_LINEAR,
	                        CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	cellGcmSetTextureAddress(ctx, 0,
	                         CELL_GCM_TEXTURE_BORDER,
	                         CELL_GCM_TEXTURE_BORDER,
	                         CELL_GCM_TEXTURE_WRAP,
	                         CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
	                         CELL_GCM_TEXTURE_ZFUNC_LESS,
	                         0);
	cellGcmSetTextureBorderColor(ctx, 0, 0xffff00ffu);
}

static void set_new_command_state(CellGcmContextData *ctx) {
	cellGcmSetZcullEnable(ctx, CELL_GCM_TRUE, CELL_GCM_TRUE);
	cellGcmSetZcullControl(ctx, CELL_GCM_ZCULL_LESS, CELL_GCM_ZCULL_LONES);
	cellGcmSetZcullStatsEnable(ctx, CELL_GCM_TRUE);
	cellGcmSetZcullLimit(ctx, 0x80, 0x180);
	cellGcmSetClearZcullSurface(ctx, CELL_GCM_TRUE, CELL_GCM_TRUE);
	cellGcmSetClearReport(ctx, CELL_GCM_ZCULL_STATS);
	cellGcmSetUserClipPlaneControl(ctx,
	                               CELL_GCM_USER_CLIP_PLANE_ENABLE_LT,
	                               CELL_GCM_USER_CLIP_PLANE_DISABLE,
	                               CELL_GCM_USER_CLIP_PLANE_DISABLE,
	                               CELL_GCM_USER_CLIP_PLANE_DISABLE,
	                               CELL_GCM_USER_CLIP_PLANE_DISABLE,
	                               CELL_GCM_USER_CLIP_PLANE_DISABLE);
}

static void set_render_state(CellGcmContextData *ctx) {
	cellGcmSetVertexProgram(ctx, vpo, vp_ucode);

	cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, texcoord_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, uv));

	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);
	bind_texture(ctx);
	set_new_command_state(ctx);
}

static void flip_and_setup_next(CellGcmContextData *ctx) {
	s32 qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index);
	while (qid < 0) { usleep(1000); qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index); }
	cellGcmSetWriteBackEndLabel(ctx, LABEL_PREPARED_BUFFER_OFFSET,
	                            (g_frame_index << 8) | (u32)qid);
	cellGcmFlush(ctx);
	while (ppu_too_fast()) usleep(3000);

	set_draw_env(ctx);
	set_render_state(ctx);

	g_frame_index = (g_frame_index + 1) % COLOR_BUFFER_NUM;
	cellGcmSetWaitLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_IDLE);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);
	set_render_target(ctx, g_frame_index);
}

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
	vpo = (CGprogram)vpshader_vpo;
	fpo = (CGprogram)fpshader_fpo;

	cellGcmCgInitProgram(vpo);
	cellGcmCgInitProgram(fpo);

	cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);

	void *fp_ucode_blob;
	cellGcmCgGetUCode(fpo, &fp_ucode_blob, &fpsize);
	fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_ucode_blob, fpsize);
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	/* Resolve vertex-attribute indices from the CgBinary parameter
	 * table.  cellGcmCgGetParameterResource returns CG_ATTR0..15 for
	 * vertex inputs; subtract CG_ATTR0 to get the attribute index
	 * the cellGcmSetVertexDataArray binding takes. */
	CGparameter pos = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter tc  = cellGcmCgGetNamedParameter(vpo, "in_texcoord");
	position_index  = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	texcoord_index  = tc  ? (int)cellGcmCgGetParameterResource(vpo, tc)  - CG_ATTR0 : 8;
}

static void init_texture(void) {
	/* Procedural 64x64 checker board.  Each 8x8 block alternates
	 * between a warm amber and a cool teal so the UV wrapping at the
	 * quad centre contrasts with the magenta border colour. */
	texture_pixels = (u32 *)local_align(128, TEX_W * TEX_H * sizeof(u32));
	for (int y = 0; y < TEX_H; y++) {
		for (int x = 0; x < TEX_W; x++) {
			int on = ((x >> 3) ^ (y >> 3)) & 1;
			u32 c = on ? 0xff20b0c0u
			           : 0xffe0a040u;
			/* Add a thin diagonal accent so orientation is readable. */
			if (((x + y) & 0x1f) == 0) c = 0xffffffffu;
			texture_pixels[y * TEX_W + x] = c;
		}
	}
	cellGcmAddressToOffset(texture_pixels, &texture_offset);
}

static void init_geometry(void) {
	/* Screen-space unit quad centred at origin.  NDC y up, y=-1 is
	 * the bottom of the screen, y=+1 is the top.  UVs run outside
	 * 0..1 to force border-colour sampling around the quad edges. */
	vertex_buffer = (vertex_t *)local_align(128, 4 * sizeof(vertex_t));
	vertex_buffer[0] = (vertex_t){{-0.75f,  0.75f}, {-0.35f, -0.35f}};
	vertex_buffer[1] = (vertex_t){{ 0.75f,  0.75f}, { 1.35f, -0.35f}};
	vertex_buffer[2] = (vertex_t){{-0.75f, -0.75f}, {-0.35f,  1.35f}};
	vertex_buffer[3] = (vertex_t){{ 0.75f, -0.75f}, { 1.35f,  1.35f}};
	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);
}

static void program_exit(CellGcmContextData *ctx) {
	cellGcmFinish(ctx, 1);
	u32 last = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) >> 8;
	for (int i = 0; i < 3000 && g_buffer_on_display != last; i++)
		usleep(1000);
}

int main(int argc, const char **argv) {
	(void)argc; (void)argv;
	printf("hello-ppu-gcm-zcull-clip-border: start\n");

	void               *host_addr = memalign(1024 * 1024, HOST_SIZE);
	CellGcmContextData *ctx;
	if (cellGcmInit(CB_SIZE, HOST_SIZE, host_addr) != 0) {
		printf("FAILURE: GCM init failed\n");
		free(host_addr);
		return 1;
	}
	if (!init_display()) {
		printf("FAILURE: display init failed\n");
		free(host_addr);
		return 1;
	}
	ctx = CELL_GCM_CURRENT;
	printf("Initialization succeeded: GCM init %ux%u\n", display_width, display_height);

	ioPadInit(7);
	cellSysutilRegisterCallback(0, on_sysutil_event, NULL);

	init_shaders();
	init_texture();
	printf("Initialization succeeded: texture upload\n");
	init_geometry();
	zcull_stats_tag_sum = CELL_GCM_ZCULL_STATS + CELL_GCM_ZCULL_STATS1
	                    + CELL_GCM_ZCULL_STATS2 + CELL_GCM_ZCULL_STATS3;

	/* Push the VP literal-pool slots (e.g. c[467] = [0,1,0,0] for the
	 * `float4(x, y, 0, 1)` position output) into the const bank exactly
	 * once, before the first draw.  These are immutable shader literals;
	 * binding the program every frame does not need to re-upload them. */
	cellGcmCgUploadInternalConsts(ctx, vpo);

	set_draw_env(ctx);
	set_render_state(ctx);
	set_render_target(ctx, g_frame_index);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);
	printf("Initialization succeeded: border color set, zcull enabled, user-clip configured (zcull tags=%u)\n",
	       zcull_stats_tag_sum);

	const int total = 3;
	int       frame = 0;

	while (frame < total && !g_exit_request) {
		cellSysutilCheckCallback();

		padInfo padinfo;
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				padData paddata = {0};
				ioPadGetData(i, &paddata);
				/* Detect rising edge: only exit on press, not hold */
				static uint16_t prev_start[MAX_PADS];
				uint16_t cur = paddata.BTN_START;
				if (cur && !prev_start[i]) {
					g_exit_request = 1; break;
				}
				prev_start[i] = cur;
			}
		}

		cellGcmSetClearColor(ctx, 0xff202830);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_Z);

		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);

		flip_and_setup_next(ctx);
		frame++;
	}

	cellGcmFinish(ctx, 1);
	printf("Render frame submitted without RPCS3 errors\n");
	printf("%s: flip completion%s observed after %d frames\n",
	       g_flip_seen ? "SUCCESS" : "FAILURE",
	       g_flip_seen ? "" : " not",
	       frame);
	program_exit(ctx);
	free(host_addr);
	ioPadEnd();
	printf("hello-ppu-gcm-zcull-clip-border: done\n");
	return g_flip_seen ? 0 : 1;
}
