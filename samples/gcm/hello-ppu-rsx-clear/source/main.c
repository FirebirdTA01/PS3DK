/*
 * hello-ppu-rsx-clear — GCM bring-up smoke test.
 *
 * Brings up the RSX command stream end-to-end and cycles a clear-color
 * over the framebuffer.  No shaders involved — uses the fixed-function
 * `rsxClearSurface` path that hits the ROP unit directly.  This is the
 * simplest possible "GCM works" smoke test:
 *
 *   - rsxInit              — set up command buffer + I/O memory
 *   - videoConfigure       — set display mode (XRGB, current resolution)
 *   - rsxMemalign          — allocate two display buffers in RSX local memory
 *   - gcmSetDisplayBuffer  — register them with the flip controller
 *   - per frame:
 *       rsxSetSurface           — bind the active back-buffer
 *       rsxSetClearColor        — set the ARGB clear value
 *       rsxClearSurface         — submit the clear command
 *       gcmSetFlip + flush      — present
 *       wait for flip status    — sync to ~60Hz
 *
 * Cycles through six colors on a 60-frame period each (~6 seconds total
 * at 60Hz), then exits.  Press START on a controller to exit early.
 *
 * Adapted from PSL1GHT's samples/graphics/cairo/source/{main.c,rsxutil.c}.
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
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2

#define GCM_LABEL_INDEX 255

typedef struct {
	uint32_t *ptr;
	uint32_t  offset;
	uint16_t  width;
	uint16_t  height;
	int       id;
} rsxBuffer;

static u32  depth_pitch;
static u32  depth_offset;
static u32 *depth_buffer;

static void wait_rsx_idle(gcmContextData *ctx)
{
	u32 label = 1;
	rsxSetWriteBackendLabel(ctx, GCM_LABEL_INDEX, label);
	rsxSetWaitLabel(ctx, GCM_LABEL_INDEX, label);
	label++;
	rsxSetWriteBackendLabel(ctx, GCM_LABEL_INDEX, label);
	rsxFlushBuffer(ctx);
	while (*(vu32 *)gcmGetLabelAddress(GCM_LABEL_INDEX) != label)
		usleep(30);
}

static void wait_flip(void)
{
	while (gcmGetFlipStatus() != 0)
		usleep(200);
	gcmResetFlipStatus();
}

static int flip(gcmContextData *ctx, s32 buffer_id)
{
	if (gcmSetFlip(ctx, buffer_id) == 0) {
		rsxFlushBuffer(ctx);
		gcmSetWaitFlip(ctx);
		return 1;
	}
	return 0;
}

static int make_buffer(rsxBuffer *b, u16 width, u16 height, int id)
{
	int pitch = (int)(width * sizeof(u32));
	int size  = pitch * height;

	b->ptr = (uint32_t *)rsxMemalign(64, size);
	if (!b->ptr)
		return 0;

	if (rsxAddressToOffset(b->ptr, &b->offset) != 0)
		return 0;

	if (gcmSetDisplayBuffer(id, b->offset, pitch, width, height) != 0)
		return 0;

	b->width  = width;
	b->height = height;
	b->id     = id;
	return 1;
}

static gcmContextData *init_screen(void *host_addr, u32 size, u16 *out_w, u16 *out_h)
{
	gcmContextData    *ctx = NULL;
	videoState         state;
	videoConfiguration vcfg;
	videoResolution    res;

	rsxInit(&ctx, CB_SIZE, size, host_addr);
	if (!ctx)
		return NULL;

	if (videoGetState(0, 0, &state) != 0 || state.state != 0)
		return NULL;
	if (videoGetResolution(state.displayMode.resolution, &res) != 0)
		return NULL;

	memset(&vcfg, 0, sizeof(vcfg));
	vcfg.resolution = state.displayMode.resolution;
	vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vcfg.pitch      = res.width * sizeof(u32);
	vcfg.aspect     = state.displayMode.aspect;

	wait_rsx_idle(ctx);

	if (videoConfigure(0, &vcfg, NULL, 0) != 0)
		return NULL;
	if (videoGetState(0, 0, &state) != 0)
		return NULL;

	gcmSetFlipMode(GCM_FLIP_VSYNC);

	depth_pitch  = res.width * sizeof(u32);
	depth_buffer = (u32 *)rsxMemalign(64, res.height * depth_pitch * 2);
	rsxAddressToOffset(depth_buffer, &depth_offset);

	gcmResetFlipStatus();

	*out_w = res.width;
	*out_h = res.height;
	return ctx;
}

static void set_render_target(gcmContextData *ctx, rsxBuffer *b)
{
	gcmSurface sf = {0};
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
	sf.x                = 0;
	sf.y                = 0;
	rsxSetSurface(ctx, &sf);
}

int main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	gcmContextData *ctx;
	void           *host_addr;
	rsxBuffer       buffers[MAX_BUFFERS];
	int             cur = 0;
	u16             width = 0, height = 0;
	padInfo         padinfo;
	padData         paddata;

	printf("hello-ppu-rsx-clear: GCM smoke test\n");

	host_addr = memalign(1024 * 1024, HOST_SIZE);
	ctx       = init_screen(host_addr, HOST_SIZE, &width, &height);
	if (!ctx) {
		printf("  init_screen failed\n");
		free(host_addr);
		return 1;
	}
	printf("  RSX up at %ux%u\n", width, height);

	ioPadInit(7);

	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!make_buffer(&buffers[i], width, height, i)) {
			printf("  make_buffer[%d] failed\n", i);
			rsxFinish(ctx, 0);
			free(host_addr);
			return 1;
		}
	}
	flip(ctx, MAX_BUFFERS - 1);

	/* Six colors, 60 frames each ~ 6 seconds at 60Hz. */
	static const u32 palette[] = {
		0xff800000,  /* dim red */
		0xff408000,  /* dim green */
		0xff004080,  /* dim blue */
		0xff808000,  /* dim yellow */
		0xff800080,  /* dim magenta */
		0xff008080,  /* dim cyan */
	};
	const int n_colors    = (int)(sizeof(palette) / sizeof(palette[0]));
	const int frames_each = 60;
	const int total       = n_colors * frames_each;

	int          frame = 0;
	volatile int exit_request = 0;

	while (frame < total && !exit_request) {
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				ioPadGetData(i, &paddata);
				if (paddata.BTN_START) {
					exit_request = 1;
					break;
				}
			}
		}

		set_render_target(ctx, &buffers[cur]);

		u32 color = palette[(frame / frames_each) % n_colors];
		rsxSetClearColor(ctx, color);
		rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		wait_flip();
		flip(ctx, buffers[cur].id);

		cur = (cur + 1) % MAX_BUFFERS;
		frame++;
	}

	printf("  drew %d frames; cycled through %d colors\n", frame, n_colors);
	if (exit_request)
		printf("  early exit on START button\n");

	gcmSetWaitFlip(ctx);
	for (int i = 0; i < MAX_BUFFERS; i++)
		rsxFree(buffers[i].ptr);
	rsxFinish(ctx, 1);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-rsx-clear: done\n");
	return 0;
}
