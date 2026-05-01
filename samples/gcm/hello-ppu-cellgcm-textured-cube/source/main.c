/*
 * hello-ppu-cellgcm-textured-cube — rotating cube, SDK ICON0 texture,
 * canonical flip_immediate pattern.
 *
 * Decodes sdk/assets/ICON0.PNG via cellPngDec at startup, byte-shuffles
 * the RGBA stream into RSX-local A8R8G8B8, and samples it from a 3D
 * cube's per-face TEXCOORD0.  Cube spins around X+Y; depth-tested
 * indexed draw of 24 vertices / 36 indices.
 *
 * Combines:
 *   - hello-ppu-cellgcm-triangle — flip_immediate (4 buffers, queue
 *     depth 1, VBlank + Flip handlers, label-based PPU/GPU handshake)
 *   - hello-ppu-cellgcm-quad — RSX texture-unit state install
 *   - hello-ppu-png — cellPngDec decode of an embedded blob
 *
 * Vertex layout: { float pos[3]; float uv[2]; }, stride 20 bytes,
 * POS @ +0, TEXCOORD0 @ +12.
 *
 * Press START to exit early.
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
#include <cell/sysmodule.h>
#include <cell/pngdec.h>
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

/* 1 MiB command buffer + 32 MiB host I/O memory.  The triangle / loops
 * samples get away with 64 KiB CB because their per-frame command
 * stream is tiny; a full texture-unit state install plus a 36-index
 * indexed draw fills 64 KiB in 30-50 frames and the wrap callback's
 * synchronous GPU drain shows up as a multi-second hitch on RPCS3.
 * 1 MiB pushes the wrap interval out to ~10 s — comfortably past the
 * human-visible window — and 32 MiB host pool gives the texture +
 * vertex / index / depth buffers room to breathe. */
#define CB_SIZE              0x100000          /* 1 MiB */
#define HOST_SIZE            (32 * 1024 * 1024)/* 32 MiB */
#define COLOR_BUFFER_NUM     4
#define MAX_QUEUE_FRAMES     1

/* Labels — matching offsets the canonical flip_immediate sample uses. */
#define LABEL_PREPARED_BUFFER_OFFSET   0x41
#define LABEL_BUFFER_STATUS_OFFSET     0x42
#define BUFFER_IDLE                    0
#define BUFFER_BUSY                    1

/* bin2s symbols for the embedded ICON0.PNG.  bin2s replaces dots with
 * underscores; the source file is named ICON0.PNG so the exported
 * symbols are ICON0_PNG[] / ICON0_PNG_size. */
extern const uint8_t  ICON0_PNG[]      __attribute__((aligned(4)));
extern const uint32_t ICON0_PNG_size;

typedef struct {
	float pos[3];
	float uv[2];
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

/* Shader objects. */
static CGprogram   vpo;
static CGprogram   fpo;
static void       *vp_ucode;
static void       *fp_ucode;
static u32         fp_offset;
static CGparameter mvpConst;
static int         position_index;
static int         texcoord_index;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;
static u16      *index_buffer;
static u32       index_buffer_offset;
static u32      *texture_pixels;
static u32       texture_offset;
static u32       texture_w;
static u32       texture_h;

/* Flip state — read by VBlank/Flip handlers and main flip(). */
static volatile u32 g_buffer_on_display = 0;
static volatile u32 g_buffer_flipped    = 0;
static volatile u32 g_on_flip           = 0;
static u32          g_frame_index       = 0;

/* Sysutil event signals. */
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
	memcpy(out, r, sizeof r);
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
	m[ 3] = tx; m[ 7] = ty; m[11] = tz;
}
static void mat_rot_x(float a, float m[16]) {
	mat_identity(m);
	float c = cosf(a), s = sinf(a);
	m[ 5] =  c; m[ 6] = -s;
	m[ 9] =  s; m[10] =  c;
}
static void mat_rot_y(float a, float m[16]) {
	mat_identity(m);
	float c = cosf(a), s = sinf(a);
	m[ 0] =  c; m[ 2] =  s;
	m[ 8] = -s; m[10] =  c;
}

/* ---- VBlank + Flip handlers — flip_immediate pattern --------------- */
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

/* ---- render-state setup -------------------------------------------- */
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

/* Per-frame texture-unit setup. */
static void bind_texture(CellGcmContextData *ctx)
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
	t.width     = (u16)texture_w;
	t.height    = (u16)texture_h;
	t.depth     = 1;
	t.location  = CELL_GCM_LOCATION_LOCAL;
	t.pitch     = (u32)(texture_w * sizeof(u32));
	t.offset    = texture_offset;

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
}

static void set_render_state(CellGcmContextData *ctx, const float mvp[16])
{
	cellGcmSetVertexProgram(ctx, vpo, vp_ucode);
	cellGcmSetVertexProgramParameter(ctx, mvpConst, mvp);
	/*   rsxLoadVertexProgram(ctx, vpo, vp_ucode);
	 *   rsxSetVertexProgramParameter(ctx, vpo, mvpConst, mvp);
	 */

	cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 3,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, texcoord_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, uv));
	/*   rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0,
	 *                            vertex_buffer_offset + offsetof(vertex_t, pos),
	 *                            sizeof(vertex_t), 3,
	 *                            GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	 *   rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0,
	 *                            vertex_buffer_offset + offsetof(vertex_t, uv),
	 *                            sizeof(vertex_t), 2,
	 *                            GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	 */

	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);
	/*   rsxLoadFragmentProgramLocation(ctx, fpo, fp_offset, GCM_LOCATION_RSX); */
	bind_texture(ctx);
}

/* Canonical flip_immediate flip().  See the matching block in
 * hello-ppu-cellgcm-triangle for the full annotation. */
static void flip(CellGcmContextData *ctx, const float mvp[16])
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
	set_render_state(ctx, mvp);

	g_frame_index = (g_frame_index + 1) % COLOR_BUFFER_NUM;

	cellGcmSetWaitLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_IDLE);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	set_render_target(ctx, g_frame_index);
}

/* ---- init helpers --------------------------------------------------- */
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

	/* VSYNC presents only at VBlank — eliminates the mid-scanline
	 * tear that GCM_FLIP_HSYNC allows.  Triangle/loops/chain pick
	 * HSYNC for input-loop responsiveness; for a static-scene visual
	 * the tear-free path is preferable. */
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
	/*   vpo = (rsxVertexProgram   *)vpshader_vpo;
	 *   fpo = (rsxFragmentProgram *)fpshader_fpo;
	 */

	cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);
	mvpConst = cellGcmCgGetNamedParameter(vpo, "modelViewProj");
	/*   rsxVertexProgramGetUCode(vpo, &vp_ucode, &vpsize);
	 *   mvpConst = rsxVertexProgramGetConst(vpo, "modelViewProj");
	 */

	void *fp_ucode_blob;
	cellGcmCgGetUCode(fpo, &fp_ucode_blob, &fpsize);
	/*   rsxFragmentProgramGetUCode(fpo, &fp_ucode_blob, &fpsize); */
	fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_ucode_blob, fpsize);
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	/* in_position lands on attrib slot 0 (POSITION); in_texcoord lands
	 * on slot 8 (TEXCOORD0).  The VP tags both with their CG_ATTR_*
	 * resource id at compile time — read it back so the binding stays
	 * in sync with whatever the compiler picked. */
	CGparameter pos = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter tex = cellGcmCgGetNamedParameter(vpo, "in_texcoord");
	position_index  = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	texcoord_index  = tex ? (int)cellGcmCgGetParameterResource(vpo, tex) - CG_ATTR0 : 8;

	printf("  vp ucode: %u bytes; mvp uniform: %p\n", vpsize, (void *)mvpConst);
	printf("  fp ucode: %u bytes (rsx-local at offset 0x%08x)\n", fpsize, fp_offset);
	printf("  vp attribs: position@%d texcoord@%d\n", position_index, texcoord_index);
}

/* cellPngDec mandatory alloc/free callbacks. */
static void *png_malloc(uint32_t size, void *arg) { (void)arg; return memalign(16, size); }
static int32_t png_free(void *ptr, void *arg)     { (void)arg; free(ptr); return 0; }

/* Decode the embedded ICON0.PNG via cellPngDec into a freshly-allocated
 * RGBA buffer.  Returns NULL on failure (caller falls back). */
static uint8_t *decode_icon0(uint32_t *out_w, uint32_t *out_h, size_t *out_stride)
{
	int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_PNGDEC);
	if (rc != 0) { printf("  SYSMODULE_PNGDEC load: 0x%08x\n", (unsigned)rc); return NULL; }

	CellPngDecThreadInParam tin = {
		.spuThreadEnable   = CELL_PNGDEC_SPU_THREAD_DISABLE,
		.ppuThreadPriority = 512,
		.spuThreadPriority = 200,
		.cbCtrlMallocFunc  = CELL_PNGDEC_CB_EA(png_malloc),
		.cbCtrlMallocArg   = 0,
		.cbCtrlFreeFunc    = CELL_PNGDEC_CB_EA(png_free),
		.cbCtrlFreeArg     = 0,
	};
	CellPngDecThreadOutParam tout = {0};
	CellPngDecMainHandle     main_h = 0;

	if (cellPngDecCreate(&main_h, &tin, &tout) != 0) goto unload;

	CellPngDecSrc src = {
		.srcSelect       = CELL_PNGDEC_BUFFER,
		.streamPtr       = (uint32_t)(uintptr_t)ICON0_PNG,
		.streamSize      = ICON0_PNG_size,
		.spuThreadEnable = CELL_PNGDEC_SPU_THREAD_DISABLE,
	};
	CellPngDecOpnInfo   info = {0};
	CellPngDecSubHandle sub_h = 0;
	if (cellPngDecOpen(main_h, &sub_h, &src, &info) != 0) goto destroy;

	CellPngDecInfo hdr = {0};
	if (cellPngDecReadHeader(main_h, sub_h, &hdr) != 0) goto close;

	CellPngDecInParam ip = {
		.outputMode        = CELL_PNGDEC_TOP_TO_BOTTOM,
		.outputColorSpace  = CELL_PNGDEC_RGBA,
		.outputBitDepth    = 8,
		.outputPackFlag    = CELL_PNGDEC_1BYTE_PER_1PIXEL,
		.outputAlphaSelect = CELL_PNGDEC_STREAM_ALPHA,
	};
	CellPngDecOutParam op = {0};
	if (cellPngDecSetParameter(main_h, sub_h, &ip, &op) != 0) goto close;

	size_t stride = (size_t)op.outputWidthByte;
	size_t bytes  = stride * op.outputHeight;
	uint8_t *rgba = (uint8_t *)memalign(16, bytes);
	if (!rgba) goto close;

	CellPngDecDataCtrlParam dcp  = { .outputBytesPerLine = stride };
	CellPngDecDataOutInfo   dout = {0};
	if (cellPngDecDecodeData(main_h, sub_h, rgba, &dcp, &dout) != 0
	    || dout.status != CELL_PNGDEC_DEC_STATUS_FINISH) {
		free(rgba);
		goto close;
	}

	cellPngDecClose(main_h, sub_h);
	cellPngDecDestroy(main_h);
	cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC);

	*out_w      = op.outputWidth;
	*out_h      = op.outputHeight;
	*out_stride = stride;
	return rgba;

close:    cellPngDecClose(main_h, sub_h);
destroy:  cellPngDecDestroy(main_h);
unload:   cellSysmoduleUnloadModule(CELL_SYSMODULE_PNGDEC);
	return NULL;
}

/* Convert a tightly-packed RGBA stream (R,G,B,A bytes per pixel) into
 * the A8R8G8B8 byte order RSX expects in memory (A,R,G,B). */
static void rgba_to_argb(const uint8_t *src, size_t src_stride,
                         uint32_t *dst, size_t dst_pitch_bytes,
                         uint32_t w, uint32_t h)
{
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *s = src + y * src_stride;
		uint32_t      *d = (uint32_t *)((uint8_t *)dst + y * dst_pitch_bytes);
		for (uint32_t x = 0; x < w; x++) {
			uint8_t r = s[x * 4 + 0];
			uint8_t g = s[x * 4 + 1];
			uint8_t b = s[x * 4 + 2];
			uint8_t a = s[x * 4 + 3];
			d[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16)
			     | ((uint32_t)g <<  8) | ((uint32_t)b);
		}
	}
}

static void init_texture(void)
{
	uint32_t w = 0, h = 0;
	size_t   src_stride = 0;
	uint8_t *rgba = decode_icon0(&w, &h, &src_stride);
	if (!rgba || w == 0 || h == 0) {
		printf("  ICON0 decode failed; falling back to 64x64 checker\n");
		w = h = 64;
		rgba = (uint8_t *)memalign(16, 64u * 64u * 4u);
		src_stride = 64 * 4;
		for (uint32_t y = 0; y < 64; y++)
			for (uint32_t x = 0; x < 64; x++) {
				int on = ((x >> 3) ^ (y >> 3)) & 1;
				uint8_t *p = rgba + y * src_stride + x * 4;
				if (on) { p[0] = 0x20; p[1] = 0xb0; p[2] = 0xc0; p[3] = 0xff; }
				else    { p[0] = 0xe0; p[1] = 0xa0; p[2] = 0x40; p[3] = 0xff; }
			}
	}

	texture_w = w;
	texture_h = h;

	size_t pitch = (size_t)w * sizeof(u32);
	texture_pixels = (u32 *)local_align(128, (u32)(pitch * h));
	rgba_to_argb(rgba, src_stride, texture_pixels, pitch, w, h);
	free(rgba);

	cellGcmAddressToOffset(texture_pixels, &texture_offset);
	printf("  texture: %ux%u (%u bytes) at rsx offset 0x%08x\n",
	       w, h, (unsigned)(pitch * h), texture_offset);
}

static void init_geometry(void)
{
	/* 24 vertices: 4 per face × 6 faces.  Each face has its own UV
	 * 0..1 quad so the texture is mapped independently per face,
	 * front-facing CCW. */
	static const vertex_t verts[24] = {
		{{-1, -1,  1}, {0, 1}}, {{ 1, -1,  1}, {1, 1}},
		{{ 1,  1,  1}, {1, 0}}, {{-1,  1,  1}, {0, 0}},
		{{ 1, -1, -1}, {0, 1}}, {{-1, -1, -1}, {1, 1}},
		{{-1,  1, -1}, {1, 0}}, {{ 1,  1, -1}, {0, 0}},
		{{ 1, -1,  1}, {0, 1}}, {{ 1, -1, -1}, {1, 1}},
		{{ 1,  1, -1}, {1, 0}}, {{ 1,  1,  1}, {0, 0}},
		{{-1, -1, -1}, {0, 1}}, {{-1, -1,  1}, {1, 1}},
		{{-1,  1,  1}, {1, 0}}, {{-1,  1, -1}, {0, 0}},
		{{-1,  1,  1}, {0, 1}}, {{ 1,  1,  1}, {1, 1}},
		{{ 1,  1, -1}, {1, 0}}, {{-1,  1, -1}, {0, 0}},
		{{-1, -1, -1}, {0, 1}}, {{ 1, -1, -1}, {1, 1}},
		{{ 1, -1,  1}, {1, 0}}, {{-1, -1,  1}, {0, 0}},
	};
	static const u16 idx[36] = {
		 0,  1,  2,    0,  2,  3,
		 4,  5,  6,    4,  6,  7,
		 8,  9, 10,    8, 10, 11,
		12, 13, 14,   12, 14, 15,
		16, 17, 18,   16, 18, 19,
		20, 21, 22,   20, 22, 23,
	};

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

	printf("hello-ppu-cellgcm-textured-cube: ICON0 on a rotating cube\n");

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
	init_texture();
	init_geometry();

	float mvp[16], proj[16], view[16], rx[16], ry[16], rot[16], mv[16];
	mat_perspective(1.0472f, (float)display_width / (float)display_height,
	                0.1f, 100.0f, proj);
	mat_translate(0.0f, 0.0f, -4.0f, view);

	mat_identity(mvp);

	/* First-time draw env / render state install + render-target set. */
	set_draw_env(ctx);
	set_render_state(ctx, mvp);
	set_render_target(ctx, g_frame_index);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	const int   total      = 1800;    /* 30 s @ 60 Hz — long enough to spot stutters */
	const float ang_per_fr = 0.008f;  /* ~0.48 rad/s → ~13 s per full revolution,
	                                   * comfortable for visually inspecting each face */
	int   frame = 0;
	float a     = 0.0f;

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

		/* Texture cache invalidate per frame stops cross-face UV bleed
		 * during rotation.  Vertex-fetch cache doesn't need flushing —
		 * the VBO bytes never change. */
		rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);

		rsxDrawIndexArray(ctx, GCM_TYPE_TRIANGLES,
		                  index_buffer_offset, 36,
		                  GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);

		flip(ctx, mvp);
		a += ang_per_fr;
		frame++;
	}

	printf("  drew %d frames\n", frame);
	if (g_exit_request) printf("  early exit on user request\n");

	program_exit(ctx);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-textured-cube: done\n");
	return 0;
}
