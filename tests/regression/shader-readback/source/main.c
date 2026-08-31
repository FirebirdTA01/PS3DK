/*
 * shader-readback — tier-c judged shader tests.
 *
 * Renders a fullscreen quad into a 64x64 off-screen RSX render target
 * with each test fragment program, reads the pixels back through the
 * PPU mapping of RSX local memory, and compares every pixel against
 * values the PPU computes independently for the same interpolants.
 * Prints PASS/FAIL per test plus a final SHADER_READBACK_OK / _FAIL
 * sentinel, and exits non-zero on any failure — this row *judges*
 * shader output rather than eyeballing it.
 *
 * Structure is a merge of two proven samples:
 *   - hello-ppu-cellgcm-render-to-texture: RT switch to an off-screen
 *     surface allocated from the local-memory bump allocator, then
 *     direct PPU readback of the rendered bytes.
 *   - hello-ppu-cellgcm-quad: CgBinary program init/bind and the
 *     screen-space quad draw path.
 *
 * Expected-value convention: the quad maps UV (0,0) at the RT's
 * top-left to (1,1) at bottom-right; a pixel (px,py) with 0,0 the top
 * left rasterizes at center ((px+0.5)/W, (py+0.5)/H).  Comparison is
 * per channel in 8-bit space with a small tolerance: the RT is
 * A8R8G8B8, so quantization contributes up to 1 LSB and fragment
 * interpolation/precision a little more.  TOLERANCE below is the
 * per-channel allowance in 8-bit steps.
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

#include "rb_pos_uv_vpo.h"
#include "rb_solid_fpo.h"
#include "rb_interp_fpo.h"
#include "rb_arith_fpo.h"
#include "rb_angles_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2

#define GCM_LABEL_INDEX 255

#define RT_W 64
#define RT_H 64
#define CLEAR_SENTINEL 0xFF000000u  /* opaque black: no test expects it */
#define GPU_CLEAR_MARK 0xFF102030u  /* GPU-clear control color, see run_test */
#define TOLERANCE 3                 /* per-channel, 8-bit steps */

typedef struct {
	float pos[2];
	float uv[2];
} vertex_t;

typedef void (*expect_fn)(float u, float v, float out[4]);

typedef struct {
	const char *name;
	const unsigned char *fp_blob;
	expect_fn   expect;
} rb_test;

/* ---- expected-value functions (PPU-side mirror of each shader) ---- */

static void expect_solid(float u, float v, float out[4])
{
	(void)u; (void)v;
	out[0] = 0.25f; out[1] = 0.5f; out[2] = 0.75f; out[3] = 1.0f;
}

static void expect_interp(float u, float v, float out[4])
{
	out[0] = u; out[1] = v; out[2] = 0.0f; out[3] = 1.0f;
}

static float saturatef(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static void expect_arith(float u, float v, float out[4])
{
	out[0] = u * v;
	out[1] = u * 0.5f + 0.25f;
	out[2] = 1.0f - u;
	out[3] = 1.0f;
}

static void expect_angles(float u, float v, float out[4])
{
	/* Mirrors radians(u*45) and degrees(v*0.0125) with the same
	 * float32 constants the compiler derives.  Associativity note for
	 * whoever tightens TOLERANCE someday: this computes (u*45)*k while
	 * the compiler may fold to u*(45*k) — up to an ulp apart in
	 * float32, invisible at 8-bit with the current tolerance, but a
	 * source of drift unrelated to the shader if the tolerance ever
	 * chases sub-LSB differences. */
	out[0] = (u * 45.0f) * 0.017453292519943295769f;
	out[1] = (v * 0.0125f) * 57.295779513082320876f;
	out[2] = 0.0f;
	out[3] = 1.0f;
}

/* ---- RSX local-memory bump allocator (see render-to-texture) ---- */

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
	/* The label slot's initial content is whatever was there before we
	 * booted, so a fixed-sequence wait can match stale data and return
	 * before the GPU has run anything.  Seed each wait from the
	 * CURRENT slot value instead: write current+1 through the GPU
	 * pipeline and wait for it — a value the slot cannot already hold. */
	vu32 *slot = (vu32 *)cellGcmGetLabelAddress(GCM_LABEL_INDEX);
	uint32_t target = *slot + 1u;
	cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, target);
	cellGcmFlush(ctx);
	while (*slot != target)
		usleep(30);
}

/* ---- display init (proven path from render-to-texture) ---- */

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

	void *dd = local_align(64, display_h * display_pitch); /* Z24S8, 4 B/px */
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

/* ---- render-target + draw-env plumbing ---- */

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

/* ---- per-test run ---- */

static CGprogram vpo;
static void     *vp_ucode;
static int       position_index;
static int       texcoord_index;
static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;

/* Main-memory readback buffer (inside the io-mapped host region).
 * RPCS3 keeps render targets on the host GPU and does not write them
 * back to guest local memory on the default configuration, so a direct
 * PPU read of the RT sees stale bytes.  The transfer engine copying
 * LOCAL -> MAIN materializes the pixels into guest main memory, which
 * is also the hardware-fast path for CPU readback. */
static u32 *g_readback;
static u32  g_readback_off;

static void transfer_rt_to_main(CellGcmContextData *ctx, u32 rt_off, u32 rt_pitch)
{
	/* NV3089 image blit, not cellGcmSetTransferData: the TransferData
	 * emitter is broken in every copy we ship (u32 pitches compared
	 * against signed MIN_PITCH promote to unsigned, sending every call
	 * into a --rowCount>=0 loop that cannot terminate on a u32) — an
	 * inherited upstream defect this harness was the first caller to
	 * hit.  The image path is the one the blitting work proved. */
	cellGcmSetTransferImage(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN,
	                        g_readback_off, rt_pitch, 0, 0,
	                        rt_off, rt_pitch, 0, 0,
	                        RT_W, RT_H, 4);
	wait_rsx_idle(ctx);
}

static int run_test(CellGcmContextData *ctx, const rb_test *t,
                    u32 *rt_ptr, u32 rt_off, u32 rt_depth_off, u32 rt_pitch)
{
	/* Fragment ucode must sit in RSX-visible memory; copy the blob's
	 * ucode section into local memory and hand its offset to the bind. */
	CGprogram fpo = (CGprogram)t->fp_blob;
	cellGcmCgInitProgram(fpo);

	void *fp_blob_ucode; u32 fpsize = 0;
	cellGcmCgGetUCode(fpo, &fp_blob_ucode, &fpsize);
	void *fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_blob_ucode, fpsize);
	u32 fp_offset = 0;
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	/* Sentinel-fill the READBACK buffer (the judged memory), so a
	 * transfer that never lands reads as a clean, attributable failure
	 * rather than stale data from the previous test passing by
	 * accident.  The RT itself gets the GPU clear below. */
	(void)rt_ptr;
	for (u32 i = 0; i < (rt_pitch / 4u) * RT_H; i++)
		g_readback[i] = CLEAR_SENTINEL;

	set_rt_surface(ctx, rt_off, rt_depth_off, rt_pitch, RT_W, RT_H);
	set_draw_env(ctx, RT_W, RT_H);

	/* Discriminating control: a GPU-side clear to a color distinct
	 * from both the sentinel and every expectation.  If a test fails
	 * with pixels at GPU_CLEAR_MARK, the surface bind and the sync
	 * both work and the fault is in the draw path; pixels still at
	 * the sentinel mean the RT/sync plumbing itself is broken. */
	cellGcmSetClearColor(ctx, GPU_CLEAR_MARK);
	cellGcmSetClearSurface(ctx,
		GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	wait_rsx_idle(ctx);
	transfer_rt_to_main(ctx, rt_off, rt_pitch);
	printf("  %s: gpu clear %s (readback[0]=0x%08x)\n", t->name,
	       g_readback[0] == GPU_CLEAR_MARK ? "LANDED" : "MISSING", g_readback[0]);

	cellGcmSetVertexProgram(ctx, vpo, vp_ucode);
	cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, texcoord_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, uv));
	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);

	/* Draw with warm-up retries: on a cold shader cache RPCS3's async
	 * shader compiler SKIPS draws whose fragment program is still
	 * compiling — the command stream completes (labels land) but no
	 * pixels change, and a single-draw judge then fails spuriously on
	 * first boot and passes on the next (measured: identical self,
	 * 0/3 cold then 2/3 warm).  Redraw until output appears or the
	 * budget runs out; a test whose CORRECT output equals the clear
	 * mark would spend the whole budget and still judge correctly,
	 * just slowly — keep test expectations away from GPU_CLEAR_MARK. */
	int tries = 0;
	for (tries = 1; tries <= 10; tries++) {
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
		wait_rsx_idle(ctx);
		transfer_rt_to_main(ctx, rt_off, rt_pitch);
		if (g_readback[0] != GPU_CLEAR_MARK)
			break;
		usleep(200000);
	}
	if (tries > 1)
		printf("  %s: drew %d times (shader warm-up)\n", t->name, tries);

	/* ---- judge every pixel (from the main-memory copy) ---- */
	int max_diff = 0, bad = 0;
	int first_bad_x = -1, first_bad_y = -1;
	u32 first_bad_px = 0; float first_bad_exp[4] = {0};

	for (int py = 0; py < RT_H; py++) {
		const u32 *row = g_readback + (u32)py * (rt_pitch / 4u);
		for (int px = 0; px < RT_W; px++) {
			float u = ((float)px + 0.5f) / (float)RT_W;
			float v = ((float)py + 0.5f) / (float)RT_H;
			float e[4];
			t->expect(u, v, e);

			u32 p = row[px];
			int actual[4] = {
				(int)((p >> 16) & 0xff),  /* R */
				(int)((p >>  8) & 0xff),  /* G */
				(int)( p        & 0xff),  /* B */
				(int)((p >> 24) & 0xff),  /* A */
			};
			int ok = 1;
			for (int c = 0; c < 4; c++) {
				int want = (int)(saturatef(e[c]) * 255.0f + 0.5f);
				int diff = actual[c] - want;
				if (diff < 0) diff = -diff;
				if (diff > max_diff) max_diff = diff;
				if (diff > TOLERANCE) ok = 0;
			}
			if (!ok) {
				bad++;
				if (first_bad_x < 0) {
					first_bad_x = px; first_bad_y = py;
					first_bad_px = p;
					memcpy(first_bad_exp, e, sizeof(e));
				}
			}
		}
	}

	if (bad == 0) {
		printf("readback %s: PASS max_diff=%d\n", t->name, max_diff);
		return 1;
	}
	printf("readback %s: FAIL bad_pixels=%d max_diff=%d first=(%d,%d) got=0x%08x expected=(%.3f %.3f %.3f %.3f)\n",
	       t->name, bad, max_diff, first_bad_x, first_bad_y,
	       first_bad_px,
	       first_bad_exp[0], first_bad_exp[1], first_bad_exp[2], first_bad_exp[3]);
	return 0;
}

int main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	printf("shader-readback: tier-c judged shader tests\n");

	void *host_addr = memalign(1024 * 1024, HOST_SIZE);
	if (!init_screen(host_addr, HOST_SIZE)) {
		printf("shader-readback: init failed\nSHADER_READBACK_FAIL\n");
		free(host_addr);
		return 1;
	}
	CellGcmContextData *ctx = (CellGcmContextData *)gCellGcmCurrentContext;

	display_buffer buffers[MAX_BUFFERS];
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!make_buffer(&buffers[i], i)) {
			printf("shader-readback: buffer init failed\nSHADER_READBACK_FAIL\n");
			cellGcmFinish(ctx, 0);
			free(host_addr);
			return 1;
		}
	}
	if (cellGcmSetFlip(ctx, (uint8_t)(MAX_BUFFERS - 1)) == 0) {
		cellGcmFlush(ctx);
		cellGcmSetWaitFlip(ctx);
	}

	/* Proven-sequence step from the render-to-texture sample: bind the
	 * backbuffer surface explicitly ONCE and idle before ever switching
	 * to the off-screen RT.  The first surface set after boot also
	 * establishes window state the later RT switch inherits. */
	set_rt_surface(ctx, buffers[0].offset, disp_depth_offset,
	               display_pitch, (u16)display_w, (u16)display_h);
	wait_rsx_idle(ctx);

	/* ---- vertex program + geometry (shared across tests) ---- */
	vpo = (CGprogram)rb_pos_uv_vpo;
	cellGcmCgInitProgram(vpo);
	u32 vpsize = 0;
	cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);

	CGparameter pos = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter tc  = cellGcmCgGetNamedParameter(vpo, "in_texcoord");
	position_index  = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	texcoord_index  = tc  ? (int)cellGcmCgGetParameterResource(vpo, tc)  - CG_ATTR0 : 8;

	vertex_buffer = (vertex_t *)local_align(128, 4 * sizeof(vertex_t));
	/* Fullscreen strip; UV (0,0) top-left -> (1,1) bottom-right.  With
	 * the negative-y viewport scale, NDC +1 y is the TOP row. */
	vertex_buffer[0] = (vertex_t){{-1.0f,  1.0f}, {0.0f, 0.0f}};
	vertex_buffer[1] = (vertex_t){{ 1.0f,  1.0f}, {1.0f, 0.0f}};
	vertex_buffer[2] = (vertex_t){{-1.0f, -1.0f}, {0.0f, 1.0f}};
	vertex_buffer[3] = (vertex_t){{ 1.0f, -1.0f}, {1.0f, 1.0f}};
	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);

	cellGcmCgUploadInternalConsts(ctx, vpo);

	/* ---- main-memory readback buffer, carved from the io-mapped
	 * host region's tail (the command buffer owns the head).  Its
	 * offset is in the RSX IO space, which is what LOCAL_TO_MAIN
	 * transfers write through. ---- */
	g_readback = (u32 *)((char *)host_addr + HOST_SIZE - (64 * 1024));
	if (cellGcmAddressToOffset(g_readback, &g_readback_off) != 0) {
		printf("shader-readback: readback offset failed\nSHADER_READBACK_FAIL\n");
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
		printf("shader-readback: RT alloc failed\nSHADER_READBACK_FAIL\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 1;
	}

	/* ---- the judged tests ---- */
	const rb_test tests[] = {
		{ "solid",  rb_solid_fpo,  expect_solid  },
		{ "interp", rb_interp_fpo, expect_interp },
		{ "arith",  rb_arith_fpo,  expect_arith  },
		{ "angles", rb_angles_fpo, expect_angles },
	};
	const int n_tests = (int)(sizeof(tests) / sizeof(tests[0]));

	/* Sentinel-collision assert: the retry loop keys on pixel (0,0)
	 * differing from GPU_CLEAR_MARK, and the transfer control keys on
	 * CLEAR_SENTINEL — a test whose EXPECTED (0,0) equals either would
	 * burn the retry budget every run or mask a dead transfer.  A
	 * comment asking authors to avoid those colors is not a check;
	 * this is. */
	for (int i = 0; i < n_tests; i++) {
		float e[4];
		tests[i].expect(0.5f / RT_W, 0.5f / RT_H, e);
		u32 packed = ((u32)(saturatef(e[3]) * 255.0f + 0.5f) << 24)
		           | ((u32)(saturatef(e[0]) * 255.0f + 0.5f) << 16)
		           | ((u32)(saturatef(e[1]) * 255.0f + 0.5f) <<  8)
		           |  (u32)(saturatef(e[2]) * 255.0f + 0.5f);
		if (packed == GPU_CLEAR_MARK || packed == CLEAR_SENTINEL) {
			printf("shader-readback: test '%s' expected (0,0) 0x%08x collides with a sentinel — pick different test values\nSHADER_READBACK_FAIL\n",
			       tests[i].name, packed);
			cellGcmFinish(ctx, 0);
			free(host_addr);
			return 1;
		}
	}

	int passed = 0;
	for (int i = 0; i < n_tests; i++)
		passed += run_test(ctx, &tests[i], rt_ptr, rt_off, rt_depth_off, rt_pitch);

	printf("shader-readback: %d/%d tests passed\n", passed, n_tests);
	printf("%s\n", passed == n_tests ? "SHADER_READBACK_OK" : "SHADER_READBACK_FAIL");

	cellGcmSetWaitFlip(ctx);
	cellGcmFinish(ctx, 1);
	free(host_addr);
	return passed == n_tests ? 0 : 1;
}
