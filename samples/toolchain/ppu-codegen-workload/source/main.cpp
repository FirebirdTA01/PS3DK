/*
 * ppu-codegen-workload: an artificial frame-based workload for comparing two
 * builds (compiler, library or code-generation option) on hardware by frame
 * rate.
 *
 * Each frame runs a fixed amount of PPU work from one phase (work.cpp), then
 * clears the screen to the phase's colour and flips.  Flips are in HSYNC mode,
 * so the frame rate is not capped at the display refresh: it follows the PPU
 * work.  The phases run in order, PHASE_FRAMES frames each, ROUNDS times.
 *
 * Output (TTY): one "WORKLOAD phase=<name> ..." line per phase with frames,
 * mean/p50/p99 frame time, fps and mean PPU work time, then "WORKLOAD_SUM"
 * with a checksum of every phase's results and "WORKLOAD_DONE".
 *
 * Built with the profiler (PS3_PROF_AUTO=ON), the whole run is captured to
 * /dev_hdd0/tmp/<executable>-<timebase>.prof; `ps3dk-prof frames` reads it.
 * Press START to exit early.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../gcm/common/gcm_host_buffer.h"
#include "work.h"

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <sysutil/video.h>
#include <io/pad.h>
#include <cell/gcm.h>
#include <ps3dk/prof.h> /* PS3DK_PROF_CAPTURE_WHOLE_RUN: inert unless profiled */

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE      0x100000
#define HOST_SIZE    (32 * 1024 * 1024)
#define MAX_BUFFERS  2
#define GCM_LABEL_INDEX 255

#ifndef PHASE_FRAMES
#define PHASE_FRAMES 300
#endif
#ifndef ROUNDS
#define ROUNDS 2
#endif
/* work_frame calls per frame, per phase: about 9.5 to 13 ms of PPU work on
 * the console, so every frame is above the flip path's ~8 ms floor and the
 * frame rate follows the work (measured on v0.29.0). */
static const int work_reps[WORK_PHASES] = { 5, 3, 9, 3, 8, 2 };
#define MAX_SAMPLES (PHASE_FRAMES * ROUNDS)

typedef struct {
	uint32_t *ptr;
	uint32_t offset;
	uint16_t width, height;
	int id;
} display_buffer;

static u32 depth_pitch, depth_offset;
static u32 *depth_buffer;

/* RSX local memory from a bump allocator (no PSL1GHT rsx library: the
 * automatic profiler accepts only the SDK's GCM archives). */
static uint8_t *local_next, *local_end;

static void *local_alloc(uint32_t align, uint32_t size)
{
	uintptr_t p = ((uintptr_t)local_next + align - 1) & ~(uintptr_t)(align - 1);
	if (p + size > (uintptr_t)local_end)
		return NULL;
	local_next = (uint8_t *)(p + size);
	return (void *)p;
}

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

static void wait_flip(void)
{
	while (cellGcmGetFlipStatus() != 0)
		usleep(20);
	cellGcmResetFlipStatus();
}

static void do_flip(CellGcmContextData *ctx, uint8_t id)
{
	if (cellGcmSetFlip(ctx, id) == 0) {
		cellGcmFlush(ctx);
		cellGcmSetWaitFlip(ctx);
	}
}

static int make_buffer(display_buffer *b, uint16_t w, uint16_t h, int id)
{
	int pitch = (int)(w * sizeof(uint32_t));
	b->ptr = (uint32_t *)local_alloc(64, pitch * h);
	if (!b->ptr || cellGcmAddressToOffset(b->ptr, &b->offset) != 0)
		return 0;
	if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, pitch, w, h) != 0)
		return 0;
	b->width = w;
	b->height = h;
	b->id = id;
	return 1;
}

static int init_screen(void *host, uint32_t size, uint16_t *ow, uint16_t *oh)
{
	videoState st;
	videoConfiguration vc;
	videoResolution res;
	if (cellGcmInit(CB_SIZE, size, host) != 0)
		return 0;
	CellGcmConfig cfg;
	cellGcmGetConfiguration(&cfg);
	local_next = (uint8_t *)cfg.localAddress;
	local_end = local_next + cfg.localSize;
	if (videoGetState(0, 0, &st) != 0 || st.state != 0)
		return 0;
	if (videoGetResolution(st.displayMode.resolution, &res) != 0)
		return 0;
	memset(&vc, 0, sizeof vc);
	vc.resolution = st.displayMode.resolution;
	vc.format = VIDEO_BUFFER_FORMAT_XRGB;
	vc.pitch = res.width * sizeof(uint32_t);
	vc.aspect = st.displayMode.aspect;
	wait_rsx_idle(CELL_GCM_CURRENT);
	if (videoConfigure(0, &vc, NULL, 0) != 0 || videoGetState(0, 0, &st) != 0)
		return 0;
	/* HSYNC: the flip does not wait for vertical blank, so the frame rate is
	 * not capped at the refresh rate (the image may tear; that is expected). */
	cellGcmSetFlipMode(GCM_FLIP_HSYNC);
	depth_pitch = res.width * sizeof(uint32_t);
	depth_buffer = (u32 *)local_alloc(64, res.height * depth_pitch * 2);
	cellGcmAddressToOffset(depth_buffer, &depth_offset);
	cellGcmResetFlipStatus();
	*ow = res.width;
	*oh = res.height;
	return 1;
}

static void set_target(CellGcmContextData *ctx, display_buffer *b)
{
	CellGcmSurface sf;
	memset(&sf, 0, sizeof sf);
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = b->offset;
	sf.colorPitch[0] = depth_pitch;
	sf.colorLocation[1] = sf.colorLocation[2] = sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1] = sf.colorPitch[2] = sf.colorPitch[3] = 64;
	sf.depthFormat = GCM_SURFACE_ZETA_Z16;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = depth_offset;
	sf.depthPitch = depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = b->width;
	sf.height = b->height;
	cellGcmSetSurface(ctx, &sf);
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static uint32_t frame_tb[WORK_PHASES][MAX_SAMPLES];
static uint32_t work_tb[WORK_PHASES][MAX_SAMPLES];

int main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	static const uint32_t colours[WORK_PHASES] = {
		0xff800000, 0xff408000, 0xff004080, 0xff808000, 0xff800080, 0xff008080,
	};
	sys_addr_t host = 0;
	display_buffer bufs[MAX_BUFFERS];
	uint16_t w = 0, h = 0;
	padInfo pi;
	int cur = 0, quit = 0, count[WORK_PHASES] = { 0 };
	uint32_t sum[WORK_PHASES] = { 0 };

	(void)PS3DK_PROF_CAPTURE_WHOLE_RUN();
	uint64_t f = sys_time_get_timebase_frequency();
	printf("WORKLOAD_BEGIN compiler=%s timebase=%llu phase_frames=%d rounds=%d\n", __VERSION__,
	       (unsigned long long)f, PHASE_FRAMES, ROUNDS);
	if (work_init() != 0) {
		printf("WORKLOAD_FAIL work_init\n");
		return 1;
	}
	if (gcm_host_buffer_alloc(HOST_SIZE, &host) != 0) {
		printf("WORKLOAD_FAIL host buffer\n");
		return 1;
	}
	if (!init_screen((void *)(uintptr_t)host, HOST_SIZE, &w, &h)) {
		printf("WORKLOAD_FAIL init_screen\n");
		return 1;
	}
	CellGcmContextData *ctx = CELL_GCM_CURRENT;
	ioPadInit(7);
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (!make_buffer(&bufs[i], w, h, i)) {
			printf("WORKLOAD_FAIL make_buffer\n");
			cellGcmFinish(ctx, 0);
			return 1;
		}
	do_flip(ctx, MAX_BUFFERS - 1);

	uint64_t prev = __builtin_ppc_get_timebase();
	for (int r = 0; r < ROUNDS && !quit; r++) {
		for (int p = 0; p < WORK_PHASES && !quit; p++) {
			printf("WORKLOAD_PHASE round=%d phase=%s\n", r, work_phase_names[p]);
			for (int fr = 0; fr < PHASE_FRAMES && !quit; fr++) {
				ioPadGetInfo(&pi);
				for (int i = 0; i < MAX_PADS; i++)
					if (pi.status[i]) {
						padData pd;
						memset(&pd, 0, sizeof pd);
						ioPadGetData(i, &pd);
						if (pd.BTN_START)
							quit = 1;
					}
				uint64_t t0 = __builtin_ppc_get_timebase();
				/* multiply-then-add: an odd multiplier keeps every result in
				 * the sum (xor-then-multiply collapses to 0 when a phase
				 * returns the same value every frame) */
				for (int k = 0; k < work_reps[p]; k++)
					sum[p] = sum[p] * 16777619u + work_frame(p);
				uint64_t t1 = __builtin_ppc_get_timebase();

				set_target(ctx, &bufs[cur]);
				cellGcmSetClearColor(ctx, colours[p]);
				cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
				wait_flip();
				do_flip(ctx, (uint8_t)bufs[cur].id);
				cur ^= 1;

				uint64_t now = __builtin_ppc_get_timebase();
				/* The first frame of each phase carries the previous phase's
				 * tail and the phase switch: not counted. */
				if (fr > 0 && count[p] < MAX_SAMPLES) {
					frame_tb[p][count[p]] = (uint32_t)(now - prev);
					work_tb[p][count[p]] = (uint32_t)(t1 - t0);
					count[p]++;
				}
				prev = now;
			}
		}
	}
	cellGcmSetWaitFlip(ctx);
	cellGcmFinish(ctx, 1);
	ioPadEnd();

	uint32_t all = 2166136261u;
	for (int p = 0; p < WORK_PHASES; p++) {
		int n = count[p];
		if (n == 0)
			continue;
		uint64_t fs = 0, ws = 0;
		for (int i = 0; i < n; i++) {
			fs += frame_tb[p][i];
			ws += work_tb[p][i];
		}
		qsort(frame_tb[p], n, sizeof(uint32_t), cmp_u32);
		double us = 1e6 / (double)f;
		double mean = (double)fs / n * us;
		double work = (double)ws / n * us;
		/* fps: displayed frames (the flip path has a floor of its own);
		 * work_fps: frames per second if each frame were only its PPU work,
		 * the rate a code-generation change moves directly. */
		printf("WORKLOAD phase=%s frames=%d mean_us=%.1f p50_us=%.1f p99_us=%.1f fps=%.2f work_us=%.1f work_fps=%.2f sum=%08x\n",
		       work_phase_names[p], n, mean, frame_tb[p][n / 2] * us, frame_tb[p][(n * 99) / 100] * us,
		       1e6 / mean, work, 1e6 / work, sum[p]);
		all = all * 16777619u + sum[p];
	}
	printf("WORKLOAD_SUM %08x%s\n", all, quit ? " (early exit)" : "");
	printf("WORKLOAD_DONE\n");
	return 0;
}
