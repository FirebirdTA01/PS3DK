/*
 * hello-ppu-cellgcm-clear — cellGcm-name port of hello-ppu-rsx-clear.
 *
 * Same RSX init + clear-and-flip flow as the rsx-clear sample, but
 * spelled with cellGcm* identifiers everywhere a forwarder exists in
 * <cell/gcm.h> + <cell/gcm/gcm_command_c.h>.  The two functions still
 * spelled rsxFoo are PSL1GHT extensions with no cellGcm equivalent
 * (rsxMemalign / rsxFree are the local-memory allocator; the cellGcm
 * pattern is to manage cellGcmGetConfiguration().localAddress by hand).
 *
 * Validates the system + command-emitter forwarders end-to-end:
 *   - cellGcmInit                  — system init (mirrors gGcmContext)
 *   - cellGcmAddressToOffset       — RSX-local-addr -> RSX-offset
 *   - cellGcmSetDisplayBuffer      — register flip buffer
 *   - cellGcmSetFlipMode           — VSYNC mode
 *   - cellGcmGetFlipStatus / Reset — flip-pending poll
 *   - cellGcmSetFlip               — emit flip command
 *   - cellGcmSetWaitFlip           — fence the flip
 *   - cellGcmFlush                 — push command buffer to RSX
 *   - cellGcmFinish                — full sync on exit
 *   - cellGcmSetSurface            — bind render target (CellGcmSurface
 *                                    aliased onto gcmSurface, byte-equal)
 *   - cellGcmSetClearColor         — set ARGB clear value
 *   - cellGcmSetClearSurface       — submit clear command (uses
 *                                    GCM_CLEAR_R|G|B|A mask)
 *   - cellGcmSetWriteBackEndLabel  — write a label (note "BackEnd"
 *                                    capitalisation — cell-SDK style)
 *   - cellGcmSetWaitLabel          — wait on a label
 *   - cellGcmGetLabelAddress       — read label slot for the busy-wait
 *
 * Cycles through six colors on a 60-frame period each (~6 seconds at
 * 60Hz), then exits.  Press START to exit early.
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
#include <rsx/rsx.h>     /* for rsxMemalign / rsxFree (PSL1GHT extensions) */

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
} display_buffer;

static u32  depth_pitch;
static u32  depth_offset;
static u32 *depth_buffer;

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
		usleep(200);
	cellGcmResetFlipStatus();
}

static int do_flip(CellGcmContextData *ctx, uint8_t buffer_id)
{
	if (cellGcmSetFlip(ctx, buffer_id) == 0) {
		cellGcmFlush(ctx);
		cellGcmSetWaitFlip(ctx);
		return 1;
	}
	return 0;
}

static int make_buffer(display_buffer *b, uint16_t width, uint16_t height, int id)
{
	int pitch = (int)(width * sizeof(uint32_t));
	int size  = pitch * height;

	b->ptr = (uint32_t *)rsxMemalign(64, size);
	if (!b->ptr)
		return 0;

	if (cellGcmAddressToOffset(b->ptr, &b->offset) != 0)
		return 0;

	if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, pitch, width, height) != 0)
		return 0;

	b->width  = width;
	b->height = height;
	b->id     = id;
	return 1;
}

static int init_screen(void *host_addr, uint32_t size, uint16_t *out_w, uint16_t *out_h)
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

	depth_pitch  = res.width * sizeof(uint32_t);
	depth_buffer = (u32 *)rsxMemalign(64, res.height * depth_pitch * 2);
	cellGcmAddressToOffset(depth_buffer, &depth_offset);

	cellGcmResetFlipStatus();

	*out_w = res.width;
	*out_h = res.height;
	return 1;
}

static void set_render_target(CellGcmContextData *ctx, display_buffer *b)
{
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
	sf.x                = 0;
	sf.y                = 0;
	cellGcmSetSurface(ctx, &sf);
}

int main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	void                *host_addr;
	display_buffer       buffers[MAX_BUFFERS];
	int                  cur = 0;
	uint16_t             width = 0, height = 0;
	padInfo              padinfo;
	padData              paddata;
	CellGcmContextData  *ctx;

	printf("hello-ppu-cellgcm-clear: cellGcm-name validation\n");

	host_addr = memalign(1024 * 1024, HOST_SIZE);
	if (!init_screen(host_addr, HOST_SIZE, &width, &height)) {
		printf("  init_screen failed\n");
		free(host_addr);
		return 1;
	}
	ctx = CELL_GCM_CURRENT;
	printf("  RSX up at %ux%u (ctx=%p)\n", width, height, (void *)ctx);

	ioPadInit(7);

	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!make_buffer(&buffers[i], width, height, i)) {
			printf("  make_buffer[%d] failed\n", i);
			cellGcmFinish(ctx, 0);
			free(host_addr);
			return 1;
		}
	}
	do_flip(ctx, MAX_BUFFERS - 1);

	static const uint32_t palette[] = {
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

		uint32_t color = palette[(frame / frames_each) % n_colors];
		cellGcmSetClearColor(ctx, color);
		cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		wait_flip();
		do_flip(ctx, (uint8_t)buffers[cur].id);

		cur = (cur + 1) % MAX_BUFFERS;
		frame++;
	}

	printf("  drew %d frames; cycled through %d colors\n", frame, n_colors);
	if (exit_request)
		printf("  early exit on START button\n");

	cellGcmSetWaitFlip(ctx);
	for (int i = 0; i < MAX_BUFFERS; i++)
		rsxFree(buffers[i].ptr);
	cellGcmFinish(ctx, 1);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-clear: done\n");
	return 0;
}
