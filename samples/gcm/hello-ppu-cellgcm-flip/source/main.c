/*
 * hello-ppu-cellgcm-flip — 4-buffer flip_immediate demonstration.
 *
 * Builds the minimum render loop that exercises the full flip-queue
 * pattern: 4 display buffers, VBlank handler that reads a
 * prepared-buffer label and issues cellGcmSetFlipImmediate, flip
 * handler that releases scanned-out buffers back to the IDLE pool.
 * PPU is throttled to MAX_QUEUE_FRAMES=2 so the label-based back-
 * pressure is also visible.
 *
 * No shaders, no geometry — the animation is produced by clearing a
 * dark background, then issuing a scissor-clipped bright clear for a
 * narrow vertical stripe whose X position advances one step per
 * frame.  Because there is no draw-call overhead the output is a
 * clean demonstration of how the flip queue presents frames.
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
#include <rsx/rsx.h>

SYS_PROCESS_PARAM(1001, 0x100000);

/* 1 MB / 32 MB so the FIFO wrap is rare; see docs/known-issues.md
 * for the wrap-stutter discussion. */
#define CB_SIZE              0x100000
#define HOST_SIZE            (32 * 1024 * 1024)
#define COLOR_BUFFER_NUM     4
#define MAX_QUEUE_FRAMES     2

#define LABEL_PREPARED_BUFFER_OFFSET   0x41
#define LABEL_BUFFER_STATUS_OFFSET     0x42
#define BUFFER_IDLE                    0
#define BUFFER_BUSY                    1

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

static void present(CellGcmContextData *ctx) {
	s32 qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index);
	while (qid < 0) { usleep(1000); qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index); }
	cellGcmSetWriteBackEndLabel(ctx, LABEL_PREPARED_BUFFER_OFFSET,
	                            (g_frame_index << 8) | (u32)qid);
	cellGcmFlush(ctx);
	while (ppu_too_fast()) usleep(3000);

	g_frame_index = (g_frame_index + 1) % COLOR_BUFFER_NUM;
	cellGcmSetWaitLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_IDLE);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);
	set_render_target(ctx, g_frame_index);
}

static void program_exit(CellGcmContextData *ctx) {
	cellGcmFinish(ctx, 1);
	u32 last = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) >> 8;
	while (g_buffer_on_display != last) usleep(1000);
}

int main(int argc, const char **argv) {
	(void)argc; (void)argv;
	printf("hello-ppu-cellgcm-flip: 4-buffer flip_immediate\n");

	void               *host_addr = memalign(1024 * 1024, HOST_SIZE);
	CellGcmContextData *ctx;
	if (cellGcmInit(CB_SIZE, HOST_SIZE, host_addr) != 0) { free(host_addr); return 1; }
	if (!init_display())                                 { free(host_addr); return 1; }
	ctx = CELL_GCM_CURRENT;
	printf("  RSX up at %ux%u, %d buffers, queue depth %d\n",
	       display_width, display_height, COLOR_BUFFER_NUM, MAX_QUEUE_FRAMES + 1);

	ioPadInit(7);
	cellSysutilRegisterCallback(0, on_sysutil_event, NULL);

	set_render_target(ctx, g_frame_index);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	const int bar_width = 48;
	const int total     = 60 * 10;     /* 10 s @ 60 Hz */
	int       frame     = 0;

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

		/* Background: dark navy.  Full-surface clear via cellGcm. */
		rsxSetScissor(ctx, 0, 0, display_width, display_height);
		cellGcmSetClearColor(ctx, 0xff101828);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		/* Animated stripe: a narrow scissor region cleared to a pulsing
		 * bright colour.  Frame N parks the stripe at X = N*4 px (mod
		 * display width minus bar width) so you see it march left to
		 * right at a steady visible rate. */
		int    x_pos = (frame * 4) % (display_width - bar_width);
		u32    hue   = (u32)((frame * 3) & 0xff);
		u32    stripe_color =
			0xff000000u |
			((u32)(0xff - hue) << 16) |
			((u32)(0x80 + hue / 2) << 8) |
			(0xff - hue / 3);
		rsxSetScissor(ctx, x_pos, 0, bar_width, display_height);
		cellGcmSetClearColor(ctx, stripe_color);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		/* A thin stationary reference strip along the bottom so the
		 * flip is obviously showing different buffers — if the flip
		 * queue stalled the stripe above would stop moving but the
		 * bottom strip would remain. */
		rsxSetScissor(ctx, 0, display_height - 8, display_width, 8);
		cellGcmSetClearColor(ctx, 0xff808080);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		present(ctx);
		frame++;
	}

	printf("  drew %d frames\n", frame);
	program_exit(ctx);
	free(host_addr);
	ioPadEnd();
	printf("hello-ppu-cellgcm-flip: done\n");
	return 0;
}
