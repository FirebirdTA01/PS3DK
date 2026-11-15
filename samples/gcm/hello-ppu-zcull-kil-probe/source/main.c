/*
 * hello-ppu-zcull-kil-probe — characterisation probe for RSX Zcull and KIL interaction.
 *
 * Measures RSX ticks, ZPASS pixel count, and ZCULL_STATS across eight cases
 * (visible vs occluded, heavy nokil vs heavy kil, zcull on vs off).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <cell/gcm.h>
#include <cell/gcm/ps3tc_gcm_labels.h>
#include <cell/sysutil.h>
#include <Cg/cg.h>

#include "vp_quad_vpo.h"
#include "fp_flat_fpo.h"
#include "fp_heavy_nokil_fpo.h"
#include "fp_heavy_kil_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define SCREEN_W            1280
#define SCREEN_H            720
#define ZCULL_W             1280
#define ZCULL_H             768                  /* 64-aligned height covering 720 target */
#define COLOR_PITCH         (SCREEN_W * 8u)      /* FP16: 8 bytes/px */
#define COLOR_SIZE          (COLOR_PITCH * SCREEN_H)
#define DEPTH_PITCH         (SCREEN_W * 4u)      /* Z24S8: 4 bytes/px */
#define DEPTH_SIZE          (DEPTH_PITCH * ZCULL_H)

#define CB_SIZE             0x40000
#define HOST_HEAP_SIZE      (8 * 1024 * 1024)

#define GCM_FENCE_LABEL     64u

#define REPORT_TS_START     512u
#define REPORT_TS_END       513u
#define REPORT_ZPASS        514u
#define REPORT_ZCULL_STATS0 515u
#define REPORT_ZCULL_STATS1 516u
#define REPORT_ZCULL_STATS2 517u
#define REPORT_ZCULL_STATS3 518u

#define REPORT_SENTINEL_U32 0xDEADBEEFu
#define REPORT_SENTINEL_U64 0xDEADBEEFDEADBEEFULL

static uint32_t *g_color;
static uint32_t  g_color_offset;
static uint32_t *g_depth;
static uint32_t  g_depth_offset;

static uint32_t g_local_heap;
static void *local_align(uint32_t align, uint32_t size)
{
	uint32_t sz = (size + 0x3ff) & ~0x3ffu;
	g_local_heap = (g_local_heap + align - 1) & ~(align - 1u);
	void *p = (void *)(uintptr_t)g_local_heap;
	g_local_heap += sz;
	return p;
}

static volatile int g_quit_requested = 0;
static void on_sysutil(uint64_t status, uint64_t param, void *userdata)
{
	(void)param;
	(void)userdata;
	if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
		g_quit_requested = 1;
}

static uint32_t g_fence_counter = 1;
static int wait_rsx(CellGcmContextData *ctx)
{
	uint32_t target = g_fence_counter++;
	cellGcmSetWriteBackEndLabel(ctx, GCM_FENCE_LABEL, target);
	cellGcmFlush(ctx);
	for (uint32_t i = 0; i < 2000000u; ++i) {
		if (cellGcmIsLabelPassed(GCM_FENCE_LABEL, target)) {
			__asm__ __volatile__("sync" ::: "memory");
			return 1;
		}
		usleep(30);
	}
	printf("ERROR: timeout waiting for RSX fence label %u\n", target);
	return 0;
}

typedef struct {
	float pos[4];
} vertex_t;

static vertex_t *g_vb_near;
static uint32_t  g_vb_near_offset;
static vertex_t *g_vb_far;
static uint32_t  g_vb_far_offset;

static CGprogram g_vp;
static void     *g_vp_ucode;
static int       g_position_attr;

static CGprogram g_fp_flat;
static uint32_t  g_fp_flat_offset;
static CGparameter g_param_flat_color;

static CGprogram g_fp_heavy_nokil;
static uint32_t  g_fp_heavy_nokil_offset;

static CGprogram g_fp_heavy_kil;
static uint32_t  g_fp_heavy_kil_offset;
static CGparameter g_param_kil_cutoff;

static void load_fp(CGprogram prog, uint32_t *offset_out)
{
	void *ucode_blob = NULL;
	uint32_t ucode_size = 0;
	cellGcmCgInitProgram(prog);
	cellGcmCgGetUCode(prog, &ucode_blob, &ucode_size);
	uint32_t copy_size = (ucode_size + 63u) & ~63u;
	void *dst = local_align(64, copy_size ? copy_size : 64);
	memcpy(dst, ucode_blob, ucode_size);
	cellGcmAddressToOffset(dst, offset_out);
}

static int init_rsx(void *host_addr)
{
	CellGcmConfig cfg;
	if (cellGcmInit(CB_SIZE, HOST_HEAP_SIZE, host_addr) != 0) {
		printf("cellGcmInit failed\n");
		return 0;
	}
	cellGcmGetConfiguration(&cfg);
	g_local_heap = (uint32_t)(uintptr_t)cfg.localAddress;

	g_color = (uint32_t *)local_align(128, COLOR_SIZE);
	g_depth = (uint32_t *)local_align(4096, DEPTH_SIZE);

	if (cellGcmAddressToOffset(g_color, &g_color_offset) != 0) return 0;
	if (cellGcmAddressToOffset(g_depth, &g_depth_offset) != 0) return 0;

	return 1;
}

static void init_shaders(void)
{
	uint32_t vpsize = 0;
	g_vp = (CGprogram)vp_quad_vpo;
	cellGcmCgInitProgram(g_vp);
	cellGcmCgGetUCode(g_vp, &g_vp_ucode, &vpsize);

	CGparameter pos = cellGcmCgGetNamedParameter(g_vp, "in_position");
	g_position_attr = pos ? (int)cellGcmCgGetParameterResource(g_vp, pos) - CG_ATTR0 : 0;

	g_fp_flat = (CGprogram)fp_flat_fpo;
	load_fp(g_fp_flat, &g_fp_flat_offset);
	g_param_flat_color = cellGcmCgGetNamedParameter(g_fp_flat, "in_color");

	g_fp_heavy_nokil = (CGprogram)fp_heavy_nokil_fpo;
	load_fp(g_fp_heavy_nokil, &g_fp_heavy_nokil_offset);

	g_fp_heavy_kil = (CGprogram)fp_heavy_kil_fpo;
	load_fp(g_fp_heavy_kil, &g_fp_heavy_kil_offset);
	g_param_kil_cutoff = cellGcmCgGetNamedParameter(g_fp_heavy_kil, "cutoff");
}

static void init_geometry(void)
{
	/* Near quad: NDC z = -0.5f -> window depth zw = 0.25f */
	g_vb_near = (vertex_t *)local_align(64, 4 * sizeof(vertex_t));
	g_vb_near[0].pos[0] = -1.0f; g_vb_near[0].pos[1] = -1.0f; g_vb_near[0].pos[2] = -0.5f; g_vb_near[0].pos[3] = 1.0f;
	g_vb_near[1].pos[0] =  1.0f; g_vb_near[1].pos[1] = -1.0f; g_vb_near[1].pos[2] = -0.5f; g_vb_near[1].pos[3] = 1.0f;
	g_vb_near[2].pos[0] =  1.0f; g_vb_near[2].pos[1] =  1.0f; g_vb_near[2].pos[2] = -0.5f; g_vb_near[2].pos[3] = 1.0f;
	g_vb_near[3].pos[0] = -1.0f; g_vb_near[3].pos[1] =  1.0f; g_vb_near[3].pos[2] = -0.5f; g_vb_near[3].pos[3] = 1.0f;
	cellGcmAddressToOffset(g_vb_near, &g_vb_near_offset);

	/* Far quad: NDC z = 0.5f -> window depth zw = 0.75f */
	g_vb_far = (vertex_t *)local_align(64, 4 * sizeof(vertex_t));
	g_vb_far[0].pos[0] = -1.0f; g_vb_far[0].pos[1] = -1.0f; g_vb_far[0].pos[2] =  0.5f; g_vb_far[0].pos[3] = 1.0f;
	g_vb_far[1].pos[0] =  1.0f; g_vb_far[1].pos[1] = -1.0f; g_vb_far[1].pos[2] =  0.5f; g_vb_far[1].pos[3] = 1.0f;
	g_vb_far[2].pos[0] =  1.0f; g_vb_far[2].pos[1] =  1.0f; g_vb_far[2].pos[2] =  0.5f; g_vb_far[2].pos[3] = 1.0f;
	g_vb_far[3].pos[0] = -1.0f; g_vb_far[3].pos[1] =  1.0f; g_vb_far[3].pos[2] =  0.5f; g_vb_far[3].pos[3] = 1.0f;
	cellGcmAddressToOffset(g_vb_far, &g_vb_far_offset);
}

static void bind_surface(CellGcmContextData *ctx)
{
	CellGcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat      = CELL_GCM_SURFACE_F_W16Z16Y16X16;
	sf.colorTarget      = CELL_GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = CELL_GCM_LOCATION_LOCAL;
	sf.colorOffset[0]   = g_color_offset;
	sf.colorPitch[0]    = COLOR_PITCH;
	sf.colorLocation[1] = CELL_GCM_LOCATION_LOCAL;
	sf.colorLocation[2] = CELL_GCM_LOCATION_LOCAL;
	sf.colorLocation[3] = CELL_GCM_LOCATION_LOCAL;
	sf.colorPitch[1]    = 64; sf.colorPitch[2] = 64; sf.colorPitch[3] = 64;
	sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation    = CELL_GCM_LOCATION_LOCAL;
	sf.depthOffset      = g_depth_offset;
	sf.depthPitch       = DEPTH_PITCH;
	sf.type             = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias        = CELL_GCM_SURFACE_CENTER_1;
	sf.width            = SCREEN_W;
	sf.height           = SCREEN_H;
	sf.x                = 0;
	sf.y                = 0;
	cellGcmSetSurface(ctx, &sf);

	float minz = 0.0f, maxz = 1.0f;
	float scale[4]  = { SCREEN_W * 0.5f, SCREEN_H * -0.5f, 0.5f, 0.0f };
	float offset[4] = { SCREEN_W * 0.5f, SCREEN_H *  0.5f, 0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, SCREEN_W, SCREEN_H, minz, maxz, scale, offset);
	cellGcmSetScissor(ctx, 0, 0, SCREEN_W, SCREEN_H);

	cellGcmSetColorMask(ctx, CELL_GCM_COLOR_MASK_R | CELL_GCM_COLOR_MASK_G |
	                         CELL_GCM_COLOR_MASK_B | CELL_GCM_COLOR_MASK_A);
	cellGcmSetColorMaskMrt(ctx, 0);

	/* Depth test LESS, depth write ON */
	cellGcmSetDepthTestEnable(ctx, CELL_GCM_TRUE);
	cellGcmSetDepthFunc(ctx, CELL_GCM_LESS);
	cellGcmSetDepthMask(ctx, CELL_GCM_TRUE);

	cellGcmSetStencilTestEnable(ctx, CELL_GCM_FALSE);
	cellGcmSetTwoSidedStencilTestEnable(ctx, CELL_GCM_FALSE);

	cellGcmSetBlendEnable(ctx, CELL_GCM_FALSE);
	cellGcmSetLogicOpEnable(ctx, CELL_GCM_FALSE);

	cellGcmSetCullFaceEnable(ctx, CELL_GCM_FALSE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);

	cellGcmSetVertexProgram(ctx, g_vp, g_vp_ucode);
	cellGcmSetReportLocation(ctx, CELL_GCM_LOCATION_LOCAL);
}

static void bind_quad_vb(CellGcmContextData *ctx, uint32_t vb_offset)
{
	cellGcmSetVertexDataArray(ctx, g_position_attr, 0, (uint8_t)sizeof(vertex_t), 4,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vb_offset);
	cellGcmSetVertexAttribInputMask(ctx, (uint16_t)(1u << g_position_attr));
}

static int bind_zcull_hardware(void)
{
	int32_t rc = cellGcmBindZcull(0,
	                              g_depth_offset,
	                              ZCULL_W,
	                              ZCULL_H,
	                              0,
	                              CELL_GCM_ZCULL_Z24S8,
	                              CELL_GCM_SURFACE_CENTER_1,
	                              CELL_GCM_ZCULL_LESS,
	                              CELL_GCM_ZCULL_LONES,
	                              CELL_GCM_SCULL_SFUNC_ALWAYS,
	                              0,
	                              0xff);
	printf("cellGcmBindZcull rc=%d\n", (int)rc);
	if (rc != 0) {
		printf("ERROR: cellGcmBindZcull failed rc=%d\n", (int)rc);
		return (int)rc;
	}
	return 0;
}

static void configure_zcull_state(CellGcmContextData *ctx, int zcull_on)
{
	if (zcull_on) {
		cellGcmSetZcullEnable(ctx, CELL_GCM_TRUE, CELL_GCM_TRUE);
		cellGcmSetZcullControl(ctx, CELL_GCM_ZCULL_LESS, CELL_GCM_ZCULL_LONES);
		cellGcmSetZcullLimit(ctx, 0x80, 0x180);
		cellGcmSetZcullStatsEnable(ctx, CELL_GCM_TRUE);
	} else {
		cellGcmSetZcullEnable(ctx, CELL_GCM_FALSE, CELL_GCM_FALSE);
		cellGcmSetZcullStatsEnable(ctx, CELL_GCM_FALSE);
	}
}

static void clear_surface(CellGcmContextData *ctx, int zcull_on)
{
	cellGcmSetClearColor(ctx, 0x00000000u);
	cellGcmSetClearDepthStencil(ctx, 0xffffff00u);
	if (zcull_on) {
		cellGcmSetInvalidateZcull(ctx);
		cellGcmSetClearZcullSurface(ctx, CELL_GCM_TRUE, CELL_GCM_TRUE);
	}
	cellGcmSetClearSurface(ctx, CELL_GCM_CLEAR_R | CELL_GCM_CLEAR_G | CELL_GCM_CLEAR_B |
	                            CELL_GCM_CLEAR_A | CELL_GCM_CLEAR_Z | CELL_GCM_CLEAR_S);
}

static void draw_occluder(CellGcmContextData *ctx)
{
	cellGcmSetDepthMask(ctx, CELL_GCM_TRUE);
	cellGcmSetFragmentProgram(ctx, g_fp_flat, g_fp_flat_offset);
	static const float s_flat_color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	if (g_param_flat_color) {
		cellGcmSetFragmentProgramParameter(ctx, g_fp_flat, g_param_flat_color,
		                                   s_flat_color, g_fp_flat_offset);
	}
	bind_quad_vb(ctx, g_vb_near_offset);
	cellGcmSetInvalidateVertexCache(ctx);
	cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
}

static int run_case_rep(CellGcmContextData *ctx,
                        int is_occluded,
                        int is_kil,
                        int zcull_on,
                        uint64_t *out_ticks,
                        uint32_t *out_zpass,
                        uint32_t *out_zs0,
                        uint32_t *out_zs1,
                        uint32_t *out_zs2,
                        uint32_t *out_zs3)
{
	bind_surface(ctx);
	configure_zcull_state(ctx, zcull_on);
	clear_surface(ctx, zcull_on);

	if (is_occluded) {
		draw_occluder(ctx);
	}

	CGprogram fp = is_kil ? g_fp_heavy_kil : g_fp_heavy_nokil;
	uint32_t fp_offset = is_kil ? g_fp_heavy_kil_offset : g_fp_heavy_nokil_offset;
	cellGcmSetFragmentProgram(ctx, fp, fp_offset);
	if (is_kil && g_param_kil_cutoff) {
		static const float s_cutoff[4] = { -100.0f, -100.0f, -100.0f, -100.0f };
		cellGcmSetFragmentProgramParameter(ctx, fp, g_param_kil_cutoff,
		                                   s_cutoff, fp_offset);
	}

	bind_quad_vb(ctx, g_vb_far_offset);

	/* Turn depth write OFF for the 16 heavy draws, keeping depth test LESS.
	 * This prevents draw 0 from writing depth (0.75f) and causing draws 1..15
	 * to fail the LESS test, allowing all 16 visible draws to pass while
	 * occluded draws are still rejected against the occluder's depth (0.25f). */
	cellGcmSetDepthMask(ctx, CELL_GCM_FALSE);

	/* Write sentinel values into report slots before each case so a stale
	 * slot or a fence passing prematurely without RSX report write is caught. */
	CellGcmReportData *rd_start = cellGcmGetReportDataAddressLocation(REPORT_TS_START, CELL_GCM_LOCATION_LOCAL);
	CellGcmReportData *rd_end   = cellGcmGetReportDataAddressLocation(REPORT_TS_END, CELL_GCM_LOCATION_LOCAL);
	CellGcmReportData *rd_zpass = cellGcmGetReportDataAddressLocation(REPORT_ZPASS, CELL_GCM_LOCATION_LOCAL);
	if (rd_start) {
		rd_start->timer = REPORT_SENTINEL_U64;
		rd_start->value = REPORT_SENTINEL_U32;
	}
	if (rd_end) {
		rd_end->timer = REPORT_SENTINEL_U64;
		rd_end->value = REPORT_SENTINEL_U32;
	}
	if (rd_zpass) {
		rd_zpass->timer = REPORT_SENTINEL_U64;
		rd_zpass->value = REPORT_SENTINEL_U32;
	}
	if (zcull_on) {
		CellGcmReportData *rd0 = cellGcmGetReportDataAddressLocation(REPORT_ZCULL_STATS0, CELL_GCM_LOCATION_LOCAL);
		CellGcmReportData *rd1 = cellGcmGetReportDataAddressLocation(REPORT_ZCULL_STATS1, CELL_GCM_LOCATION_LOCAL);
		CellGcmReportData *rd2 = cellGcmGetReportDataAddressLocation(REPORT_ZCULL_STATS2, CELL_GCM_LOCATION_LOCAL);
		CellGcmReportData *rd3 = cellGcmGetReportDataAddressLocation(REPORT_ZCULL_STATS3, CELL_GCM_LOCATION_LOCAL);
		if (rd0) { rd0->timer = REPORT_SENTINEL_U64; rd0->value = REPORT_SENTINEL_U32; }
		if (rd1) { rd1->timer = REPORT_SENTINEL_U64; rd1->value = REPORT_SENTINEL_U32; }
		if (rd2) { rd2->timer = REPORT_SENTINEL_U64; rd2->value = REPORT_SENTINEL_U32; }
		if (rd3) { rd3->timer = REPORT_SENTINEL_U64; rd3->value = REPORT_SENTINEL_U32; }
	}
	__asm__ volatile("sync" ::: "memory");

	cellGcmSetZpassPixelCountEnable(ctx, CELL_GCM_TRUE);
	cellGcmSetClearReport(ctx, CELL_GCM_ZPASS_PIXEL_CNT);
	if (zcull_on) {
		cellGcmSetClearReport(ctx, CELL_GCM_ZCULL_STATS);
	}

	cellGcmSetTimeStamp(ctx, REPORT_TS_START);

	cellGcmSetInvalidateVertexCache(ctx);
	for (int i = 0; i < 16; ++i) {
		cellGcmSetDrawArrays(ctx, GCM_TYPE_QUADS, 0, 4);
	}

	cellGcmSetTimeStamp(ctx, REPORT_TS_END);

	cellGcmSetReport(ctx, CELL_GCM_ZPASS_PIXEL_CNT, REPORT_ZPASS);
	if (zcull_on) {
		cellGcmSetReport(ctx, CELL_GCM_ZCULL_STATS, REPORT_ZCULL_STATS0);
		cellGcmSetReport(ctx, CELL_GCM_ZCULL_STATS1, REPORT_ZCULL_STATS1);
		cellGcmSetReport(ctx, CELL_GCM_ZCULL_STATS2, REPORT_ZCULL_STATS2);
		cellGcmSetReport(ctx, CELL_GCM_ZCULL_STATS3, REPORT_ZCULL_STATS3);
	}

	cellGcmSetZpassPixelCountEnable(ctx, CELL_GCM_FALSE);

	if (!wait_rsx(ctx)) {
		return -1;
	}

	/* Poll briefly after fence for report write to settle from sentinel */
	uint64_t t_start = REPORT_SENTINEL_U64;
	uint64_t t_end   = REPORT_SENTINEL_U64;
	uint32_t zpass   = REPORT_SENTINEL_U32;
	for (int n = 0; n < 2000; ++n) {
		t_start = rd_start ? rd_start->timer : REPORT_SENTINEL_U64;
		t_end   = rd_end   ? rd_end->timer   : REPORT_SENTINEL_U64;
		zpass   = rd_zpass ? rd_zpass->value : REPORT_SENTINEL_U32;
		if (t_start != REPORT_SENTINEL_U64 && t_end != REPORT_SENTINEL_U64 && zpass != REPORT_SENTINEL_U32)
			break;
		usleep(100);
	}

	if (t_start == REPORT_SENTINEL_U64 || t_end == REPORT_SENTINEL_U64) {
		printf("ERROR: timestamp report holds sentinel after wait: t_start=0x%llx t_end=0x%llx\n",
		       (unsigned long long)t_start, (unsigned long long)t_end);
		return -1;
	}
	if (t_start == 0 || t_end == 0 || t_end < t_start) {
		printf("ERROR: zero or invalid timestamp readback t_start=%llu t_end=%llu\n",
		       (unsigned long long)t_start, (unsigned long long)t_end);
		return -1;
	}

	if (zpass == REPORT_SENTINEL_U32) {
		printf("ERROR: zpass report not written by RSX (holds sentinel)\n");
		return -1;
	}

	*out_ticks = t_end - t_start;
	*out_zpass = zpass;

	if (zcull_on) {
		CellGcmReportData *rd0 = cellGcmGetReportDataAddressLocation(REPORT_ZCULL_STATS0, CELL_GCM_LOCATION_LOCAL);
		CellGcmReportData *rd1 = cellGcmGetReportDataAddressLocation(REPORT_ZCULL_STATS1, CELL_GCM_LOCATION_LOCAL);
		CellGcmReportData *rd2 = cellGcmGetReportDataAddressLocation(REPORT_ZCULL_STATS2, CELL_GCM_LOCATION_LOCAL);
		CellGcmReportData *rd3 = cellGcmGetReportDataAddressLocation(REPORT_ZCULL_STATS3, CELL_GCM_LOCATION_LOCAL);
		if (!rd0 || rd0->value == REPORT_SENTINEL_U32 ||
		    !rd1 || rd1->value == REPORT_SENTINEL_U32 ||
		    !rd2 || rd2->value == REPORT_SENTINEL_U32 ||
		    !rd3 || rd3->value == REPORT_SENTINEL_U32) {
			printf("ERROR: zcull stats report still holds sentinel\n");
			return -1;
		}
		*out_zs0 = rd0->value;
		*out_zs1 = rd1->value;
		*out_zs2 = rd2->value;
		*out_zs3 = rd3->value;
	} else {
		*out_zs0 = 0;
		*out_zs1 = 0;
		*out_zs2 = 0;
		*out_zs3 = 0;
	}
	return 0;
}

typedef struct {
	uint64_t ticks;
	uint32_t zpass;
	uint32_t zs[4];
} rep_result_t;

static int run_case(CellGcmContextData *ctx,
                    int is_occluded,
                    int is_kil,
                    int zcull_on)
{
	rep_result_t results[3];
	for (int r = 0; r < 3; ++r) {
		if (cellSysutilCheckCallback() != 0 || g_quit_requested)
			return -1;
		int rc = run_case_rep(ctx, is_occluded, is_kil, zcull_on,
		                      &results[r].ticks,
		                      &results[r].zpass,
		                      &results[r].zs[0],
		                      &results[r].zs[1],
		                      &results[r].zs[2],
		                      &results[r].zs[3]);
		if (rc != 0) {
			printf("ZKP case=%s-%s zcull=%s ABORTED: %s rc=%d\n",
			       is_occluded ? "O" : "V",
			       is_kil ? "kil" : "nokil",
			       zcull_on ? "on" : "off",
			       (rc == -1) ? "fence timeout" : "error",
			       rc);
			return rc;
		}
	}

	/* Sort results by ticks to select the median (index 1) */
	for (int i = 0; i < 2; ++i) {
		for (int j = i + 1; j < 3; ++j) {
			if (results[i].ticks > results[j].ticks) {
				rep_result_t tmp = results[i];
				results[i] = results[j];
				results[j] = tmp;
			}
		}
	}
	rep_result_t *median = &results[1];

	printf("ZKP case=%s-%s zcull=%s rsx_ticks=%llu zpass=%u zcull_stats=%u,%u,%u,%u\n",
	       is_occluded ? "O" : "V",
	       is_kil ? "kil" : "nokil",
	       zcull_on ? "on" : "off",
	       (unsigned long long)median->ticks,
	       median->zpass,
	       median->zs[0], median->zs[1], median->zs[2], median->zs[3]);

	if (!is_occluded) {
		printf("ZPASS V case=%s zcull=%s: total=%u per_draw=%u (expected ~921600 per draw x 16 visible = 14745600)\n",
		       is_kil ? "kil" : "nokil",
		       zcull_on ? "on" : "off",
		       median->zpass,
		       median->zpass / 16u);
	}
	return 0;
}

int main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	void *host_addr = memalign(1024 * 1024, (size_t)HOST_HEAP_SIZE);
	if (!host_addr) {
		printf("host heap alloc failed\n");
		return 1;
	}
	if (!init_rsx(host_addr)) {
		free(host_addr);
		return 1;
	}

	if (cellSysutilRegisterCallback(0, on_sysutil, NULL) != 0) {
		printf("cellSysutilRegisterCallback failed\n");
		free(host_addr);
		return 1;
	}

	init_shaders();
	init_geometry();

	CellGcmContextData *ctx = CELL_GCM_CURRENT;
	if (!ctx) {
		printf("no gcm context\n");
		cellSysutilUnregisterCallback(0);
		free(host_addr);
		return 1;
	}

	/* Reset fence label memory to 0 with sync and confirm before any GPU work */
	volatile uint32_t *label_addr = (volatile uint32_t *)cellGcmGetLabelAddress(GCM_FENCE_LABEL);
	*label_addr = 0;
	__asm__ volatile("sync" ::: "memory");
	if (*label_addr != 0) {
		printf("ERROR: label %u failed to zero (read 0x%08x)\n", GCM_FENCE_LABEL, *label_addr);
		cellSysutilUnregisterCallback(0);
		free(host_addr);
		return 1;
	}

	/* Initial unbind to guarantee clean zcull hardware state */
	cellGcmUnbindZcull(0);

	int failed = 0;

	/* ---- Block 1: zcull=off (4 cases) ---- */
	if (run_case(ctx, 0, 0, 0) != 0) { failed = 1; goto exit_block; } /* V-nokil off */
	if (run_case(ctx, 0, 1, 0) != 0) { failed = 1; goto exit_block; } /* V-kil   off */
	if (run_case(ctx, 1, 0, 0) != 0) { failed = 1; goto exit_block; } /* O-nokil off */
	if (run_case(ctx, 1, 1, 0) != 0) { failed = 1; goto exit_block; } /* O-kil   off */

	/* ---- Block 2: zcull=on (4 cases, bound once) ---- */
	if (bind_zcull_hardware() != 0) { failed = 1; goto exit_block; }
	if (run_case(ctx, 0, 0, 1) != 0) { failed = 1; goto exit_block; } /* V-nokil on */
	if (run_case(ctx, 0, 1, 1) != 0) { failed = 1; goto exit_block; } /* V-kil   on */
	if (run_case(ctx, 1, 0, 1) != 0) { failed = 1; goto exit_block; } /* O-nokil on */
	if (run_case(ctx, 1, 1, 1) != 0) { failed = 1; goto exit_block; } /* O-kil   on */

exit_block:
	/* Step 1: Unbind Zcull */
	cellGcmSetZcullStatsEnable(ctx, CELL_GCM_FALSE);
	cellGcmSetZcullEnable(ctx, CELL_GCM_FALSE, CELL_GCM_FALSE);
	cellGcmSetInvalidateZcull(ctx);
	int rc = (int)cellGcmUnbindZcull(0);
	printf("ZKP EXIT unbind rc=%d\n", rc);
	fflush(stdout);

	/* Step 2: Fresh label value written with cellGcmSetWriteBackEndLabel, bounded 2 s fence */
	uint32_t exit_target = g_fence_counter++;
	cellGcmSetWriteBackEndLabel(ctx, GCM_FENCE_LABEL, exit_target);
	cellGcmFlush(ctx);
	int fence_ok = 0;
	for (int n = 0; n < 2000; ++n) {
		if (cellGcmIsLabelPassed(GCM_FENCE_LABEL, exit_target)) {
			fence_ok = 1;
			break;
		}
		usleep(1000);
	}
	printf("ZKP EXIT fence=%s\n", fence_ok ? "ok" : "timeout");
	fflush(stdout);

	/* Step 3: cellGcmFinish */
	cellGcmFinish(ctx, 0);

	/* Step 4: Unregister sysutil callback */
	cellSysutilUnregisterCallback(0);

	/* Step 5: print clean exit */
	if (!fence_ok || failed) {
		free(host_addr);
		return 1;
	}

	printf("ZKP EXIT clean\n");
	fflush(stdout);

	free(host_addr);
	return 0;
}
