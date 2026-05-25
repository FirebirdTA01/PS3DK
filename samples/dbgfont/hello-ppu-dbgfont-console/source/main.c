/*
 * hello-ppu-dbgfont-console -- exercise the Console API subsystem.
 *
 * Opens a custom console (lower-right, green) alongside the
 * auto-created stdout console (slot 0, top-left, white).  Writes
 * lines to both every frame and renders via cellDbgFontDrawGcm.
 * Exits cleanly after ~3 seconds.
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

static void wait_flip(void)
{
	while (cellGcmGetFlipStatus() != 0) usleep(200);
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

static int make_buffer(display_buffer *b, uint16_t w, uint16_t h, int id)
{
	int pitch = (int)(w * sizeof(uint32_t));
	b->ptr = (uint32_t *)rsxMemalign(64, pitch * h);
	if (!b->ptr) return 0;
	if (cellGcmAddressToOffset(b->ptr, &b->offset) != 0) return 0;
	if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, pitch, w, h) != 0) return 0;
	b->width = w; b->height = h; b->id = id;
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
	sf.width  = b->width;  sf.height = b->height;
	sf.x = 0; sf.y = 0;
	cellGcmSetSurface(ctx, &sf);

	float scale_vp[4]  = {  b->width / 2.0f, -(float)b->height / 2.0f, 0.5f, 0.0f };
	float offset_vp[4] = {  b->width / 2.0f,   b->height / 2.0f,      0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, b->width, b->height, 0.0f, 1.0f, scale_vp, offset_vp);
	cellGcmSetScissor(ctx, 0, 0, b->width, b->height);
}

int main(void)
{
	printf("hello-ppu-dbgfont-console: starting\n");

	void *host_addr = memalign(1024*1024, HOST_SIZE);
	if (!host_addr) { printf("FAILURE: host memalign\n"); return 1; }

	videoState state; videoConfiguration vcfg; videoResolution res;
	if (cellGcmInit(CB_SIZE, HOST_SIZE, host_addr) != 0) return 1;
	if (videoGetState(0,0,&state) != 0 || state.state != 0) return 1;
	if (videoGetResolution(state.displayMode.resolution, &res) != 0) return 1;
	memset(&vcfg,0,sizeof(vcfg));
	vcfg.resolution = state.displayMode.resolution;
	vcfg.format = VIDEO_BUFFER_FORMAT_XRGB;
	vcfg.pitch  = res.width * sizeof(uint32_t);
	vcfg.aspect = state.displayMode.aspect;
	if (videoConfigure(0,&vcfg,NULL,0) != 0) return 1;
	cellGcmSetFlipMode(GCM_FLIP_VSYNC);

	depth_pitch  = res.width * sizeof(uint32_t);
	depth_buffer = (u32 *)rsxMemalign(64, res.height * depth_pitch * 2);
	cellGcmAddressToOffset(depth_buffer, &depth_offset);

	cellGcmResetFlipStatus();

	printf("  RSX up at %ux%u\n", res.width, res.height);

	CellGcmContextData *ctx = CELL_GCM_CURRENT;
	display_buffer bufs[MAX_BUFFERS];
	memset(bufs,0,sizeof(bufs));
	for (int i = 0; i < MAX_BUFFERS; i++)
		if (!make_buffer(&bufs[i], res.width, res.height, (uint8_t)i))
			{ printf("FAILURE: buf %d\n", i); return 1; }
	int cur = 0;

	/* dbgfont init buffer. 9000 verts * 36 bytes + ~50% margin
	 * for future console additions without re-exhaustion. */
	size_t dbg_buf_size = CELL_DBGFONT_FRAGMENT_SIZE
	                    + CELL_DBGFONT_TEXTURE_SIZE
	                    + 9000 * CELL_DBGFONT_VERTEX_SIZE;
	void *dbg_buf = rsxMemalign(128, dbg_buf_size);
	CellDbgFontConfigGcm cfg = {0};
	cfg.localBufAddr = (sys_addr_t)(uintptr_t)dbg_buf;
	cfg.localBufSize = (uint32_t)dbg_buf_size;
	cfg.option = CELL_DBGFONT_VERTEX_LOCAL | CELL_DBGFONT_TEXTURE_LOCAL;
	int rc = cellDbgFontInitGcm(&cfg);
	printf("  cellDbgFontInitGcm -> 0x%08x\n", (unsigned)rc);
	if (rc) { printf("FAILURE\n"); return 1; }

	/* Open a custom console (lower-right, green). */
	CellDbgFontConsoleConfig cc = {
		.posLeft=0.50f, .posTop=0.55f,
		.cnsWidth=40, .cnsHeight=10,
		.scale=0.75f, .color=0xff00ff00u
	};
	CellDbgFontConsoleId cc_id = cellDbgFontConsoleOpen(&cc);
	printf("  cellDbgFontConsoleOpen -> %d\n", (int)cc_id);

	/* Render loop: ~3 seconds at 60 fps. */
	int frame = 0;
	while (frame < 6000) {  /* ~100 seconds at 60 fps */
		frame++;
		if (frame == 1) printf("  render loop started\n");
		set_render_target(ctx, &bufs[cur]);
		cellGcmSetClearColor(ctx, 0xff101822);
		cellGcmSetClearSurface(ctx, GCM_CLEAR_R|GCM_CLEAR_G|GCM_CLEAR_B|GCM_CLEAR_A);

		cellDbgFontConsolePrintf(CELL_DBGFONT_STDOUT_ID,
			"=== STDOUT CONSOLE (frame %d) ===", frame);
		cellDbgFontConsolePrintf(cc_id,
			"=== CUSTOM CONSOLE (frame %d) ===", frame);

		cellDbgFontDrawGcm();
		cellGcmFlush(ctx);
		do_flip(ctx, (uint8_t)cur);
		cur = (cur + 1) % MAX_BUFFERS;
	}

	cellDbgFontConsoleClose(cc_id);
	cellDbgFontExitGcm();
	free(dbg_buf);
	cellGcmFinish(CELL_GCM_CURRENT, 0);
	free(host_addr);
	printf("SUCCESS\n");
	return 0;
}
