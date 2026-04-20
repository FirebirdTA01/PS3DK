/*
 * hello-ppu-dbgfont — exercise the cellDbgFont* overlay API.
 *
 * Sets up a minimal GCM context (clear + flip, no geometry) and calls
 * cellDbgFontPrintf four times per frame with varying position, scale,
 * and colour so the overlay stays legible over a plain background.
 *
 * Lifecycle:
 *   cellGcmInit → videoConfigure → cellGcmSetDisplayBuffer ×2
 *   cellDbgFontInitGcm with a caller-provided buffer
 *   per-frame:  clear, Printf a few lines, DrawGcm, flip
 *   cellDbgFontExitGcm at shutdown
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
#include <cell/dbgfont.h>
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

	if (cellGcmInit(CB_SIZE, size, host_addr) != 0) return 0;
	if (videoGetState(0, 0, &state) != 0 || state.state != 0) return 0;
	if (videoGetResolution(state.displayMode.resolution, &res) != 0) return 0;

	memset(&vcfg, 0, sizeof(vcfg));
	vcfg.resolution = state.displayMode.resolution;
	vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vcfg.pitch      = res.width * sizeof(uint32_t);
	vcfg.aspect     = state.displayMode.aspect;

	wait_rsx_idle(CELL_GCM_CURRENT);
	if (videoConfigure(0, &vcfg, NULL, 0) != 0) return 0;
	if (videoGetState(0, 0, &state) != 0) return 0;

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
	cellGcmSetSurface(ctx, &sf);
}

int main(int argc, const char **argv)
{
	(void)argc; (void)argv;

	printf("hello-ppu-dbgfont: starting\n");

	void                *host_addr = memalign(1024 * 1024, HOST_SIZE);
	display_buffer       buffers[MAX_BUFFERS];
	int                  cur = 0;
	uint16_t             width = 0, height = 0;
	CellGcmContextData  *ctx;

	if (!init_screen(host_addr, HOST_SIZE, &width, &height)) {
		printf("  init_screen failed\n");
		free(host_addr);
		return 1;
	}
	ctx = CELL_GCM_CURRENT;
	printf("  RSX up at %ux%u\n", width, height);

	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!make_buffer(&buffers[i], width, height, i)) {
			printf("  make_buffer[%d] failed\n", i);
			cellGcmFinish(ctx, 0);
			free(host_addr);
			return 1;
		}
	}
	do_flip(ctx, MAX_BUFFERS - 1);

	/* Dedicated libdbgfont scratch buffer.  Sized to hold the fragment
	 * program (CELL_DBGFONT_FRAGMENT_SIZE) + the 256×256 alpha atlas
	 * (CELL_DBGFONT_TEXTURE_SIZE) + room for ~1024 per-glyph vertex
	 * records (CELL_DBGFONT_VERTEX_SIZE each), 128-byte aligned. */
	size_t dbg_buf_size  = CELL_DBGFONT_FRAGMENT_SIZE
	                     + CELL_DBGFONT_TEXTURE_SIZE
	                     + 1024 * CELL_DBGFONT_VERTEX_SIZE;
	void  *dbg_local_buf = rsxMemalign(128, dbg_buf_size);

	CellDbgFontConfigGcm cfg = {0};
	cfg.localBufAddr = (sys_addr_t)(uintptr_t)dbg_local_buf;
	cfg.localBufSize = (uint32_t)dbg_buf_size;
	cfg.mainBufAddr  = 0;
	cfg.mainBufSize  = 0;
	cfg.option       = CELL_DBGFONT_VERTEX_LOCAL | CELL_DBGFONT_TEXTURE_LOCAL;

	int32_t rc = cellDbgFontInitGcm(&cfg);
	printf("  cellDbgFontInitGcm -> 0x%08x\n", (unsigned)rc);

	ioPadInit(7);

	const int frames        = 60 * 12;  /* ~12 s at 60 fps */
	volatile int exit_flag  = 0;

	for (int frame = 0; frame < frames && !exit_flag; frame++) {
		padInfo padinfo;
		padData paddata;
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				ioPadGetData(i, &paddata);
				if (paddata.BTN_START) { exit_flag = 1; break; }
			}
		}

		set_render_target(ctx, &buffers[cur]);

		cellGcmSetClearColor(ctx, 0xff101822);  /* dark navy background */
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		float t = frame / 60.0f;

		/* ---- Top row: title + stats -------------------------------- */
		cellDbgFontPrintf(0.03f, 0.03f, 1.40f, 0xffffffff,
		                  "hello-ppu-dbgfont");
		cellDbgFontPrintf(0.03f, 0.10f, 0.90f, 0xffc0c0c0,
		                  "cellDbgFont* on-screen overlay demo");
		cellDbgFontPrintf(0.75f, 0.04f, 0.80f, 0xff80ffff,
		                  "frame %4d", frame);
		cellDbgFontPrintf(0.75f, 0.09f, 0.80f, 0xff80ffff,
		                  "t = %5.2f s", t);

		/* ---- Left column: colour swatches -------------------------- */
		cellDbgFontPrintf(0.04f, 0.22f, 1.00f, 0xffff4040, "red");
		cellDbgFontPrintf(0.04f, 0.28f, 1.00f, 0xff40ff40, "green");
		cellDbgFontPrintf(0.04f, 0.34f, 1.00f, 0xff4080ff, "blue");
		cellDbgFontPrintf(0.04f, 0.40f, 1.00f, 0xffffff40, "yellow");
		cellDbgFontPrintf(0.04f, 0.46f, 1.00f, 0xffff40ff, "magenta");
		cellDbgFontPrintf(0.04f, 0.52f, 1.00f, 0xff40ffff, "cyan");
		cellDbgFontPrintf(0.04f, 0.58f, 1.00f, 0xffff8040, "orange");
		cellDbgFontPrintf(0.04f, 0.64f, 1.00f, 0xffc080ff, "violet");

		/* ---- Centre: size ramp ------------------------------------ */
		cellDbgFontPrintf(0.36f, 0.22f, 0.60f, 0xffffffff, "scale 0.60");
		cellDbgFontPrintf(0.36f, 0.27f, 0.90f, 0xffffffff, "scale 0.90");
		cellDbgFontPrintf(0.36f, 0.34f, 1.30f, 0xffffffff, "scale 1.30");
		cellDbgFontPrintf(0.36f, 0.43f, 1.80f, 0xffffffff, "scale 1.80");
		cellDbgFontPrintf(0.36f, 0.56f, 2.40f, 0xffffffff, "scale 2.40");

		/* ---- Right column: animated tints ------------------------- */
		float pulse = 0.5f + 0.5f * sinf(t * 3.0f);
		uint8_t cv  = (uint8_t)(80 + 175 * pulse);
		uint32_t pulse_col = 0xff000000u | ((uint32_t)cv << 16)
		                                 | ((uint32_t)cv << 0);
		cellDbgFontPrintf(0.70f, 0.22f, 1.00f, pulse_col,
		                  "pulsing   %3d", cv);
		cellDbgFontPrintf(0.70f, 0.28f, 1.00f, 0xff40ff80,
		                  "sin(t*3)  %+0.2f", sinf(t * 3.0f));
		cellDbgFontPrintf(0.70f, 0.34f, 1.00f, 0xffff8040,
		                  "cos(t*2)  %+0.2f", cosf(t * 2.0f));

		/* ---- Mid-screen moving banner ----------------------------- */
		float bx = 0.25f + 0.15f * sinf(t * 1.5f);
		float by = 0.74f + 0.03f * sinf(t * 2.3f);
		cellDbgFontPrintf(bx, by, 1.60f, 0xffffe040,
		                  "** drifting text **");

		/* ---- Bottom row: corners + footer ------------------------- */
		cellDbgFontPrintf(0.03f, 0.90f, 0.80f, 0xff80ff80,
		                  "bottom-left (x=0.03, y=0.90)");
		cellDbgFontPrintf(0.38f, 0.92f, 0.90f, 0xffffc0c0,
		                  "press START to exit early");
		cellDbgFontPrintf(0.75f, 0.90f, 0.80f, 0xff80c0ff,
		                  "bottom-right");

		cellDbgFontDrawGcm();

		wait_flip();
		do_flip(ctx, (uint8_t)buffers[cur].id);
		cur = (cur + 1) % MAX_BUFFERS;
	}

	cellDbgFontExitGcm();
	printf("  cellDbgFontExitGcm — done\n");

	cellGcmSetWaitFlip(ctx);
	for (int i = 0; i < MAX_BUFFERS; i++) rsxFree(buffers[i].ptr);
	rsxFree(dbg_local_buf);
	cellGcmFinish(ctx, 1);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-dbgfont: done\n");
	return 0;
}
