/*
 * hello-ppu-cellgcm-cursor — HW-cursor sample.
 *
 * Validates the cellGcmInitCursor / SetCursor* family added in this
 * commit.  The PS3 RSX has a hardware cursor overlay that draws on top
 * of the framebuffer without any RSX commands or shaders — it's
 * configured purely through the cellGcm cursor API.
 *
 * Sequence:
 *   1. Standard cellGcmInit + display-buffer setup (same as
 *      hello-ppu-cellgcm-clear).
 *   2. Allocate a 32x32 BGRA8 cursor texture in RSX local memory at
 *      a 2048-byte alignment (CELL_GCM_CURSOR_ALIGN_OFFSET).  Fill it
 *      with a checker pattern so the cursor is visible against any
 *      background.
 *   3. cellGcmInitCursor + SetCursorImageOffset + SetCursorEnable.
 *   4. Per-frame loop:
 *        - Clear surface to a slowly-cycling background color.
 *        - Move cursor in a smooth Lissajous pattern across the screen.
 *        - cellGcmUpdateCursor pushes new position to HW.
 *        - Flip.
 *   5. SetCursorDisable + clean exit after ~6 seconds.
 *
 * No shaders, no draw calls.  If the cursor appears and animates,
 * every cellGcm cursor function works on RPCS3.
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
#include <rsx/rsx.h>     /* rsxMemalign / rsxFree (PSL1GHT extensions) */

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2

#define GCM_LABEL_INDEX 255

#define CURSOR_W    32
#define CURSOR_H    32
#define CURSOR_BYTES (CURSOR_W * CURSOR_H * 4)

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

/* Generate a 32x32 BGRA cursor: yellow filled square + black border +
 * a single-pixel red hot-spot at the centre. */
static void fill_cursor(uint32_t *pixels)
{
	for (int y = 0; y < CURSOR_H; y++) {
		for (int x = 0; x < CURSOR_W; x++) {
			uint32_t argb;
			if (x == 0 || x == CURSOR_W - 1 || y == 0 || y == CURSOR_H - 1) {
				argb = 0xff000000;  /* black border */
			} else if (x == CURSOR_W / 2 && y == CURSOR_H / 2) {
				argb = 0xffff0000;  /* red centre */
			} else {
				argb = 0xffffff00;  /* yellow fill */
			}
			pixels[y * CURSOR_W + x] = argb;
		}
	}
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

	printf("hello-ppu-cellgcm-cursor: HW-cursor validation\n");

	host_addr = memalign(1024 * 1024, HOST_SIZE);
	if (!init_screen(host_addr, HOST_SIZE, &width, &height)) {
		printf("  init_screen failed\n");
		free(host_addr);
		return 1;
	}
	ctx = CELL_GCM_CURRENT;
	printf("  RSX up at %ux%u\n", width, height);

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

	/* Cursor texture in RSX local memory, 2048-aligned. */
	uint32_t *cursor_pixels =
		(uint32_t *)rsxMemalign(CELL_GCM_CURSOR_ALIGN_OFFSET, CURSOR_BYTES);
	if (!cursor_pixels) {
		printf("  cursor texture alloc failed\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 1;
	}
	fill_cursor(cursor_pixels);

	uint32_t cursor_offset = 0;
	if (cellGcmAddressToOffset(cursor_pixels, &cursor_offset) != 0) {
		printf("  cursor AddressToOffset failed\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 1;
	}
	printf("  cursor at RSX offset 0x%08x (%u-byte aligned: %s)\n",
	       cursor_offset, CELL_GCM_CURSOR_ALIGN_OFFSET,
	       (cursor_offset % CELL_GCM_CURSOR_ALIGN_OFFSET) == 0 ? "yes" : "no");

	int rc = cellGcmInitCursor();
	printf("  cellGcmInitCursor -> 0x%08x\n", (unsigned)rc);
	if (rc != 0) goto cleanup;

	rc = cellGcmSetCursorImageOffset(cursor_offset);
	printf("  cellGcmSetCursorImageOffset -> 0x%08x\n", (unsigned)rc);

	rc = cellGcmSetCursorEnable();
	printf("  cellGcmSetCursorEnable -> 0x%08x\n", (unsigned)rc);

	const int total = 360;  /* ~6 seconds at 60Hz */
	int       frame = 0;
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

		/* Slow background-color sweep so the cursor is always visible. */
		float t = (float)frame / (float)total;
		uint32_t bg = 0xff000000 |
		              ((uint8_t)(64 + 64 * t)        << 16) |
		              ((uint8_t)(96 + 64 * (1 - t))  <<  8) |
		              ((uint8_t)(128));
		cellGcmSetClearColor(ctx, bg);
		cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		/* Lissajous: x = a + r*cos(t*wx), y = b + r*sin(t*wy). */
		float ang = (float)frame * 0.03f;
		int32_t cx = (int32_t)(width  / 2 + (width  / 3) * cosf(ang * 1.0f));
		int32_t cy = (int32_t)(height / 2 + (height / 3) * sinf(ang * 1.4f));
		cellGcmSetCursorPosition(cx, cy);
		cellGcmUpdateCursor();

		wait_flip();
		do_flip(ctx, (uint8_t)buffers[cur].id);

		cur = (cur + 1) % MAX_BUFFERS;
		frame++;
	}

	rc = cellGcmSetCursorDisable();
	printf("  cellGcmSetCursorDisable -> 0x%08x\n", (unsigned)rc);

	printf("  drew %d frames; cursor swept Lissajous\n", frame);
	if (exit_request)
		printf("  early exit on START button\n");

cleanup:
	cellGcmSetWaitFlip(ctx);
	rsxFree(cursor_pixels);
	for (int i = 0; i < MAX_BUFFERS; i++)
		rsxFree(buffers[i].ptr);
	cellGcmFinish(ctx, 1);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-cursor: done\n");
	return 0;
}
