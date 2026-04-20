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

/* HSV → ARGB (u32) with saturation and value locked at 1.0.  Used by the
 * rainbow line in the main loop — each letter gets its own hue phase so
 * colours ripple across the word as `t` advances. */
static uint32_t hsv_to_argb(float h)
{
	float hp = h * 6.0f;
	int   i  = (int)floorf(hp);
	float f  = hp - (float)i;
	float p  = 0.0f;          /* (1 - s) with s=1 */
	float q  = 1.0f - f;      /* (1 - s*f) with s=1 */
	float k  = f;             /* (1 - s*(1-f)) with s=1 */
	float r, g, b;
	switch (((i % 6) + 6) % 6) {
		case 0: r = 1.0f; g = k;    b = p;    break;
		case 1: r = q;    g = 1.0f; b = p;    break;
		case 2: r = p;    g = 1.0f; b = k;    break;
		case 3: r = p;    g = q;    b = 1.0f; break;
		case 4: r = k;    g = p;    b = 1.0f; break;
		default:r = 1.0f; g = p;    b = q;    break;
	}
	uint32_t ur = (uint32_t)(r * 255.0f + 0.5f) & 0xff;
	uint32_t ug = (uint32_t)(g * 255.0f + 0.5f) & 0xff;
	uint32_t ub = (uint32_t)(b * 255.0f + 0.5f) & 0xff;
	return 0xff000000u | (ur << 16) | (ug << 8) | ub;
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

	/* SetSurface fills NV40TCL_RT_HORIZ (render-target clip) but NOT the
	 * viewport — that's a separate register (NV40TCL_VIEWPORT_HORIZ) set
	 * via cellGcmSetViewport.  Without it NDC [-1,+1] maps to zero
	 * pixels and every draw clips to nothing.  Map full-screen with the
	 * conventional y-flipped scale/offset so NDC +y renders up the
	 * display and depth maps [0..1] to [min_z..max_z]. */
	float scale[4]  = {  b->width / 2.0f, -(float)b->height / 2.0f, 0.5f, 0.0f };
	float offset[4] = {  b->width / 2.0f,   b->height / 2.0f,      0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, b->width, b->height, 0.0f, 1.0f, scale, offset);
	cellGcmSetScissor(ctx, 0, 0, b->width, b->height);
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

	volatile int exit_flag = 0;
	int          frame     = 0;

	/* Runs until the user presses START on a connected pad — no
	 * auto-exit timer.  `frame` still increments per iteration so the
	 * animated layout has a monotonic time base. */
	while (!exit_flag) {
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

		/* 3x3 grid of static labels (all Printf so they go through the
		 * same rendering path) plus one slowly-animated line.  Short
		 * strings and positions well inside the safe area so they
		 * stay on-screen regardless of where the library draws its
		 * origin. */

		/* ---- top line: animated rainbow greeting --------------- */
		/* A PS3-themed greeting where each letter gets its own hue
		 * that sweeps around the colour wheel on a time-offset phase,
		 * producing a ripple travelling along the word. */
		const char *rainbow = "HELLO FROM THE RSX";
		const float rb_x0   = 0.08f;
		const float rb_y    = 0.05f;
		const float rb_sc   = 1.50f;
		const float char_w  = 0.022f;   /* empirical stride at scale 1.5 */
		for (int i = 0; rainbow[i]; i++) {
			float   hue   = fmodf(t * 0.20f + (float)i * 0.06f, 1.0f);
			uint32_t col  = hsv_to_argb(hue);
			char    one[2] = { rainbow[i], 0 };
			cellDbgFontPrintf(rb_x0 + (float)i * char_w, rb_y, rb_sc, col,
			                  "%s", one);
		}

		/* ---- top row -------------------------------------------- */
		cellDbgFontPrintf(0.10f, 0.15f, 1.50f, 0xffffffff, "WHITE");
		cellDbgFontPrintf(0.40f, 0.15f, 1.50f, 0xffff4040, "RED");
		cellDbgFontPrintf(0.70f, 0.15f, 1.50f, 0xff40ff40, "GREEN");

		/* ---- rotating rigid word -------------------------------- */
		/* The letters march along a direction vector that rotates at
		 * `spin_speed` rad/s around (cx, cy).  Kerning between letters
		 * stays constant in the word's local frame, so the word stays
		 * a single recognisable string — it orbits its pivot instead
		 * of smearing.  Glyphs themselves stay upright (our shader is
		 * a passthrough and the Printf API emits axis-aligned quads);
		 * per-glyph tilt would need a renderer extension.
		 *
		 * The y component of the direction is multiplied by the screen
		 * aspect ratio so the letter spacing looks the same in pixel
		 * space whether the word is horizontal or vertical.  Without
		 * that, a 16:9 display squashes the vertical sweep visually. */
		const char *spin_text  = "SPIN";
		const float cx         = 0.50f;
		const float cy         = 0.35f;
		const float spin_speed = 1.80f;
		const float spin_step  = 0.020f;      /* letter-to-letter stride */
		const float aspect     = 16.0f / 9.0f;
		int spin_len = 0;
		for (; spin_text[spin_len]; spin_len++) { }
		float spin_angle = t * spin_speed;
		float dir_x      = cosf(spin_angle);
		float dir_y      = sinf(spin_angle) * aspect;
		float word_half  = (spin_step * (float)(spin_len - 1)) * 0.5f;
		float start_x    = cx - dir_x * word_half;
		float start_y    = cy - dir_y * word_half;
		for (int i = 0; i < spin_len; i++) {
			float lx = start_x + dir_x * spin_step * (float)i - 0.006f;
			float ly = start_y + dir_y * spin_step * (float)i - 0.012f;
			char  one[2] = { spin_text[i], 0 };
			cellDbgFontPrintf(lx, ly, 1.20f, 0xffffc040, "%s", one);
		}

		/* ---- middle row ----------------------------------------- */
		cellDbgFontPrintf(0.10f, 0.45f, 1.50f, 0xff40a0ff, "BLUE");
		cellDbgFontPrintf(0.40f, 0.45f, 2.00f, 0xffffff40, "YELLOW");
		cellDbgFontPrintf(0.70f, 0.45f, 1.50f, 0xffff40ff, "PINK");

		/* ---- pulsing scale --------------------------------------- */
		/* Scale breathes between ~0.5 and ~2.1 on a 1.5 rad/s sine. */
		float pulse_sc = 1.30f + 0.80f * sinf(t * 1.50f);
		cellDbgFontPrintf(0.44f, 0.60f, pulse_sc, 0xff40ffff, "PULSE");

		/* ---- bottom row ----------------------------------------- */
		cellDbgFontPrintf(0.10f, 0.75f, 1.20f, 0xff40ffff, "CYAN");
		cellDbgFontPrintf(0.40f, 0.75f, 1.20f, 0xffff8040, "ORANGE");
		cellDbgFontPrintf(0.70f, 0.75f, 1.20f, 0xffc0c0c0, "GREY");

		/* ---- slow animated frame counter ------------------------ */
		float bx = 0.35f + 0.08f * sinf(t * 0.4f);
		cellDbgFontPrintf(bx, 0.90f, 1.00f, 0xffffffff,
		                  "frame %d", frame);

		cellDbgFontDrawGcm();

		wait_flip();
		do_flip(ctx, (uint8_t)buffers[cur].id);
		cur = (cur + 1) % MAX_BUFFERS;
		frame++;
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
