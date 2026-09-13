/*
 * hello-ppu-cellgcm-sysinfo — the GCM system-query surface, value-checked.
 *
 * Issue #7: <rsx/gcm_sys.h> declared 28 gcm* functions that nothing in the
 * SDK defined, so calling any of them - cellGcmInitDefaultFifoMode in the
 * report - compiled clean and failed at link.  27 now have shims.  This
 * sample is the runtime half of that fix: it CALLS every one of them and,
 * wherever the result can be predicted or cross-checked, requires the
 * value rather than merely surviving the call.  A sample that only proves
 * "it linked and did not crash" would pass just as happily against a shim
 * that returned garbage.
 *
 * Names are spelled cellGcm* where <cell/gcm.h> has a forwarder (that is
 * the path the reporter took) and gcm* where it does not.
 *
 * Screen is GREEN when every gated check passed and RED otherwise; the
 * failing check indices are printed and also drawn as the red channel's
 * low bits, so a headless capture still names them.  Ungated calls are
 * made and logged but not judged - they are marked "log" below, and
 * saying which is which is the point.
 *
 * gcmSetUserCommand is deliberately absent: it is the one name in the set
 * with an unresolved contract (see the commit that added the shims).
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
#include <rsx/rsx.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2

#define GCM_LABEL_INDEX 255

/* Tile slot used for the set/read-back check.  High index, and the region
 * is never bound, so nothing the RSX is scanning out can be affected. */
#define PROBE_TILE_INDEX 7

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

/* Every gated check has to have RUN for a verdict to mean anything: an
 * early START exit that skipped one would otherwise leave fail_mask at
 * zero and print PASS on partial coverage. */
#define EXPECTED_CHECKS 12

/* One bit per gated check.  Bit set == that check failed. */
static uint32_t fail_mask;
static int      checks_run;

static void check(int index, int ok, const char *what)
{
	checks_run++;
	if (!ok)
		fail_mask |= (1u << index);
	printf("  [%2d] %-46s %s\n", index, what, ok ? "ok" : "FAILED");
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
	sf.type             = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias        = GCM_SURFACE_CENTER_1;
	sf.width            = b->width;
	sf.height           = b->height;
	sf.x                = 0;
	sf.y                = 0;
	cellGcmSetSurface(ctx, &sf);
}

/* A userland effective address: non-null and inside the 32-bit space.
 * Under -mlp64 a pointer is 8 bytes, so a shim that returned the raw
 * register without widening correctly shows up here. */
static int plausible_ea(const void *p)
{
	uintptr_t u = (uintptr_t)p;
	return u != 0 && (uint64_t)u <= 0xffffffffull;
}

int main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	void               *host_addr;
	display_buffer      buffers[MAX_BUFFERS];
	int                 cur = 0;
	uint16_t            width = 0, height = 0;
	padInfo             padinfo;
	CellGcmContextData *ctx;
	int32_t             fifo_rc;

	printf("hello-ppu-cellgcm-sysinfo: GCM system-query surface (issue #7)\n");

	/*
	 * [0] The reported call, in the reported spelling, before cellGcmInit
	 * as the FIFO mode setter requires.  This is the one that used to fail
	 * to link at all.
	 */
	fifo_rc = cellGcmInitDefaultFifoMode(CELL_GCM_DEFAULT_FIFO_MODE_CONDITIONAL);
	printf("  cellGcmInitDefaultFifoMode(CONDITIONAL) -> %d\n", (int)fifo_rc);

	host_addr = memalign(1024 * 1024, HOST_SIZE);
	if (!init_screen(host_addr, HOST_SIZE, &width, &height)) {
		printf("  init_screen failed\n");
		free(host_addr);
		return 1;
	}
	ctx = CELL_GCM_CURRENT;
	printf("  RSX up at %ux%u (ctx=%p), sizeof(void*)=%u\n",
	       width, height, (void *)ctx, (unsigned)sizeof(void *));

	check(0, fifo_rc == 0, "cellGcmInitDefaultFifoMode(CONDITIONAL) == 0");

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

	/*
	 * [1][2] gcmGetOffsetTable is the one wrapper whose SHAPE changed:
	 * lv2 writes two 32-bit EAs, and under -mlp64 the caller's struct is
	 * two 8-byte pointers, so the shim widens field by field.  A wrong
	 * widening leaves the second field holding the first field's low half,
	 * zero, or a value outside the 32-bit space - all three are caught here.
	 */
	{
		gcmOffsetTable table;
		memset(&table, 0, sizeof(table));
		gcmGetOffsetTable(&table);
		printf("  offset table: io=%p ea=%p\n", (void *)table.io, (void *)table.ea);
		check(1, plausible_ea(table.io) && plausible_ea(table.ea),
		      "gcmGetOffsetTable both fields are userland EAs");
		check(2, table.io != table.ea,
		      "gcmGetOffsetTable io and ea are distinct");
	}

	/*
	 * [3][4] The pointer-returning getters.  cellGcmGetDisplayInfo is the
	 * one with predictable contents: we configured the display ourselves a
	 * moment ago, so its width/height/pitch must be the values we set.
	 * That distinguishes a correctly widened pointer from one that merely
	 * looks non-null.
	 */
	{
		const gcmTileInfo    *tiles = (const gcmTileInfo *)cellGcmGetTileInfo();
		const gcmZcullInfo   *zcull = (const gcmZcullInfo *)cellGcmGetZcullInfo();
		const gcmDisplayInfo *disp  = gcmGetDisplayInfo();

		printf("  tiles=%p zcull=%p display=%p\n",
		       (const void *)tiles, (const void *)zcull, (const void *)disp);
		check(3, plausible_ea(tiles) && plausible_ea(zcull) && plausible_ea(disp),
		      "tile / zcull / display info pointers are EAs");

		if (plausible_ea(disp)) {
			printf("  display[0]: %ux%u pitch=%u offset=0x%08x\n",
			       (unsigned)disp[0].width, (unsigned)disp[0].height,
			       (unsigned)disp[0].pitch, (unsigned)disp[0].offset);
			check(4, disp[0].width == width && disp[0].height == height &&
			         disp[0].pitch == (uint32_t)(width * 4),
			      "gcmGetDisplayInfo[0] matches the configured mode");
		} else {
			check(4, 0, "gcmGetDisplayInfo[0] matches the configured mode");
		}
	}

	/*
	 * [5] gcmSetFlipStatus is a void function, so the only way to judge it
	 * is a round trip through the getter.  The polarity is the one this
	 * sample's own wait_flip() relies on and the first boot confirmed:
	 * ZERO means the flip has completed, so SetFlipStatus marks complete
	 * and ResetFlipStatus marks pending.  This was written the other way
	 * round first and the measurement corrected it, not the reverse.
	 *
	 * The flip queued above has to have completed first - otherwise its
	 * asynchronous completion races the reads and the check becomes a
	 * coin toss rather than a test of the setter.
	 */
	{
		wait_flip();
		cellGcmResetFlipStatus();
		uint32_t after_reset = cellGcmGetFlipStatus();
		cellGcmSetFlipStatus();
		uint32_t after_set = cellGcmGetFlipStatus();
		printf("  flip status: after reset=%u after set=%u\n",
		       (unsigned)after_reset, (unsigned)after_set);
		check(5, after_reset != 0 && after_set == 0,
		      "cellGcmSetFlipStatus round-trips through the getter");
		/* Leave the status at COMPLETE.  wait_flip() below spins until the
		 * status reads zero and only then queues the next flip, so leaving
		 * it PENDING here deadlocks the draw loop against a flip that was
		 * never issued - which is exactly what the first long boot did. */
		cellGcmSetFlipStatus();
	}

	/*
	 * [6] gcmGetCurrentDisplayBufferId names a buffer we registered, so
	 * the id has to be one of ours.
	 */
	{
		u8  id = 0xff;
		s32 rc = gcmGetCurrentDisplayBufferId(&id);
		printf("  current display buffer: rc=%d id=%u\n", (int)rc, (unsigned)id);
		check(6, rc == 0 && id < MAX_BUFFERS,
		      "gcmGetCurrentDisplayBufferId returns one of our buffers");
	}

	/*
	 * [7] The IO-map reservation round trip: reserve then unreserve the
	 * same size.  Both have to succeed, and unreserving a size that was
	 * never reserved would not.
	 */
	{
		const u32 reserve = 1024 * 1024;
		s32 r1 = gcmReserveIoMapSize(reserve);
		s32 r2 = gcmUnreserveIoMapSize(reserve);
		printf("  io map reserve/unreserve: %d / %d\n", (int)r1, (int)r2);
		check(7, r1 == 0 && r2 == 0, "gcmReserve/UnreserveIoMapSize round trip");
	}

	/*
	 * [8] gcmSetTile fills a slot in the tile table that
	 * cellGcmGetTileInfo hands back, so setting one and reading it back is
	 * a value check across both functions at once.  The slot is never
	 * bound, so this does not touch anything the RSX is scanning out.
	 * Spelled gcmSetTile, not cellGcmSetTile: the forwarder for that name
	 * in <cell/gcm.h> goes to cellGcmSetTileInfo, which was never missing,
	 * so the cell spelling would not reach the restored shim at all.
	 *
	 * The assertion is that the slot CHANGES, and changes differently for
	 * two different pitches - not that any field equals the raw argument.
	 * The firmware packs these fields, and asserting an encoding we have
	 * not measured would be guessing an expected value, which is worse
	 * than not checking.  Distinct inputs producing distinct stored slots
	 * still catches a shim that does nothing or writes a constant.
	 */
	{
		const gcmTileInfo *tiles = (const gcmTileInfo *)cellGcmGetTileInfo();
		if (plausible_ea(tiles)) {
			gcmTileInfo before, after_a, after_b;
			before = tiles[PROBE_TILE_INDEX];

			gcmSetTile(PROBE_TILE_INDEX, CELL_GCM_LOCATION_LOCAL,
			           0x100000, 0x10000, 0x400, 0, 0, 0);
			after_a = tiles[PROBE_TILE_INDEX];

			gcmSetTile(PROBE_TILE_INDEX, CELL_GCM_LOCATION_LOCAL,
			           0x100000, 0x10000, 0x800, 0, 0, 0);
			after_b = tiles[PROBE_TILE_INDEX];

			printf("  tile[%d]: before %08x/%08x/%08x/%08x\n"
			       "            pitch 0x400 %08x/%08x/%08x/%08x\n"
			       "            pitch 0x800 %08x/%08x/%08x/%08x\n",
			       PROBE_TILE_INDEX,
			       (unsigned)before.tile,  (unsigned)before.limit,
			       (unsigned)before.pitch, (unsigned)before.format,
			       (unsigned)after_a.tile,  (unsigned)after_a.limit,
			       (unsigned)after_a.pitch, (unsigned)after_a.format,
			       (unsigned)after_b.tile,  (unsigned)after_b.limit,
			       (unsigned)after_b.pitch, (unsigned)after_b.format);

			check(8, memcmp(&after_a, &before, sizeof(before)) != 0 &&
			         memcmp(&after_b, &after_a, sizeof(before)) != 0,
			      "gcmSetTile changes the slot cellGcmGetTileInfo reads");
		} else {
			check(8, 0, "gcmSetTile changes the slot cellGcmGetTileInfo reads");
		}
		gcmSetInvalidateTile(PROBE_TILE_INDEX);
	}

	/*
	 * [11] The same shape for gcmSetZcull / cellGcmGetZcullInfo, which
	 * is otherwise the one restored name with no call site in this sample.
	 * Again the slot is filled but never bound - and again the legacy
	 * spelling is the deliberate one: <cell/gcm.h>'s cellGcmSetZcull
	 * forwards to gcmBindZcull, which both misses the restored shim and
	 * actually binds the region.
	 */
	{
		const gcmZcullInfo *zc = (const gcmZcullInfo *)cellGcmGetZcullInfo();
		if (plausible_ea(zc)) {
			gcmZcullInfo before, after_a, after_b;
			before = zc[PROBE_TILE_INDEX];

			gcmSetZcull(PROBE_TILE_INDEX, 0x200000, 64, 64, 0,
			                CELL_GCM_ZCULL_Z16, CELL_GCM_SURFACE_CENTER_1,
			                CELL_GCM_ZCULL_LESS, CELL_GCM_ZCULL_LONES,
			                CELL_GCM_SCULL_SFUNC_ALWAYS, 0, 0xff);
			after_a = zc[PROBE_TILE_INDEX];

			gcmSetZcull(PROBE_TILE_INDEX, 0x200000, 128, 128, 0,
			                CELL_GCM_ZCULL_Z16, CELL_GCM_SURFACE_CENTER_1,
			                CELL_GCM_ZCULL_LESS, CELL_GCM_ZCULL_LONES,
			                CELL_GCM_SCULL_SFUNC_ALWAYS, 0, 0xff);
			after_b = zc[PROBE_TILE_INDEX];

			printf("  zcull[%d]: before %08x/%08x/%08x  64px %08x/%08x/%08x"
			       "  128px %08x/%08x/%08x\n",
			       PROBE_TILE_INDEX,
			       (unsigned)before.region,  (unsigned)before.size,  (unsigned)before.start,
			       (unsigned)after_a.region, (unsigned)after_a.size, (unsigned)after_a.start,
			       (unsigned)after_b.region, (unsigned)after_b.size, (unsigned)after_b.start);

			check(11, memcmp(&after_a, &before, sizeof(before)) != 0 &&
			          memcmp(&after_b, &after_a, sizeof(before)) != 0,
			      "gcmSetZcull changes the slot cellGcmGetZcullInfo reads");
		} else {
			check(11, 0, "gcmSetZcull changes the slot cellGcmGetZcullInfo reads");
		}
	}

	/*
	 * [9] The report data address must land inside the report area, which
	 * is reachable through the same getter at a different index: two
	 * consecutive report slots are 16 bytes apart.  That catches a shim
	 * that returns a fixed or truncated address.
	 */
	{
		const CellGcmReportData *r0 = cellGcmGetReportDataAddress(0);
		const CellGcmReportData *r1 = cellGcmGetReportDataAddress(1);
		printf("  report data: [0]=%p [1]=%p delta=%ld\n",
		       (const void *)r0, (const void *)r1,
		       (long)((const char *)r1 - (const char *)r0));
		check(9, plausible_ea(r0) && plausible_ea(r1) &&
		         ((const char *)r1 - (const char *)r0) == (long)sizeof(CellGcmReportData),
		      "cellGcmGetReportDataAddress strides by one report");
	}

	/*
	 * Ungated calls: these are made so the shims are exercised and their
	 * results logged, but nothing here has a value this sample can predict
	 * without building more state than it is worth.  They are NOT judged,
	 * and that is deliberate - a check whose expected value is a guess is
	 * worse than no check.  gcmGetNotifyDataAddress in particular returns
	 * NULL until a notify region is mapped, which this sample does not do.
	 */
	printf("  --- called and logged, not judged ---\n");
	printf("  gcmGetLastSecondVTime      = %lld\n", (long long)gcmGetLastSecondVTime());
	printf("  gcmGetReport(0,0)          = %u\n", (unsigned)gcmGetReport(0, 0));
	printf("  cellGcmGetReportDataLocation(0,LOCAL) = %u\n",
	       (unsigned)cellGcmGetReportDataLocation(0, CELL_GCM_LOCATION_LOCAL));
	printf("  cellGcmGetReportDataAddressLocation(0,LOCAL) = %p\n",
	       (void *)cellGcmGetReportDataAddressLocation(0, CELL_GCM_LOCATION_LOCAL));
	printf("  gcmGetTimeStamp(0)         = %llu\n", (unsigned long long)gcmGetTimeStamp(0));
	printf("  gcmGetTimeStampLocation(0,LOCAL) = %llu\n",
	       (unsigned long long)gcmGetTimeStampLocation(0, CELL_GCM_LOCATION_LOCAL));
	printf("  gcmGetNotifyDataAddress(0) = %p (NULL is expected: no notify region mapped)\n",
	       (void *)gcmGetNotifyDataAddress(0));
	printf("  gcmGetDisplayBufferByFlipIndex(0) = %d\n",
	       (int)gcmGetDisplayBufferByFlipIndex(0));
	printf("  gcmSortRemapEaIoAddress()  = %d\n", (int)gcmSortRemapEaIoAddress());
	printf("  cellGcmUnbindTile(%d)       = %d\n", PROBE_TILE_INDEX,
	       (int)cellGcmUnbindTile(PROBE_TILE_INDEX));
	printf("  cellGcmUnbindZcull(%d)      = %d\n", PROBE_TILE_INDEX,
	       (int)cellGcmUnbindZcull(PROBE_TILE_INDEX));
	/* The frequency setters take one of the reference SDK's
	 * CELL_GCM_DISPLAY_FREQUENCY_* values (59_94HZ=1, SCANOUT=2,
	 * DISABLE=3), NOT a frequency in Hz.  Our <cell/gcm/gcm_enum.h> does
	 * not carry that enum yet, so the literal is spelled with the
	 * reference's value and its name written down here rather than
	 * guessed at the call site.  Called after the draw loop, below, so
	 * that changing the vblank rate cannot disturb the flips the other
	 * checks depend on. */

	/*
	 * [10] gcmGetLastFlipTime has to advance across real flips.  Sampled
	 * over the draw loop below rather than here, because it needs flips to
	 * have happened.
	 */
	s64 flip_time_first = cellGcmGetLastFlipTime();

	const int total = 180;   /* ~3 s at 60 Hz */
	int          frame = 0;
	volatile int exit_request = 0;

	while (frame < total && !exit_request) {
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				padData paddata = {0};
				ioPadGetData(i, &paddata);
				static uint16_t prev_start[MAX_PADS];
				uint16_t now = paddata.BTN_START;
				if (now && !prev_start[i]) { exit_request = 1; break; }
				prev_start[i] = now;
			}
		}

		set_render_target(ctx, &buffers[cur]);

		/* Judgement is drawn, not just printed: green when every gated
		 * check passed, otherwise red with the failure mask in the low
		 * bits of the green and blue channels so a screenshot names it. */
		uint32_t color;
		if (fail_mask == 0)
			color = 0xff008000;
		else
			color = 0xffc00000 | ((fail_mask & 0xff) << 8) | ((fail_mask >> 8) & 0xff);

		cellGcmSetClearColor(ctx, color);
		cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		wait_flip();
		do_flip(ctx, (uint8_t)buffers[cur].id);

		cur = (cur + 1) % MAX_BUFFERS;
		frame++;

		/* Judged a third of the way in, not after the loop: the verdict
		 * has to reach the screen, and a check evaluated after the last
		 * flip could only ever be printed. */
		if (frame == total / 3) {
			s64 flip_time_last = cellGcmGetLastFlipTime();
			printf("  last flip time: first=%lld after %d frames=%lld\n",
			       (long long)flip_time_first, frame, (long long)flip_time_last);
			check(10, flip_time_last > 0 && flip_time_last > flip_time_first,
			      "cellGcmGetLastFlipTime advances across flips");
		}
	}

	/* CELL_GCM_DISPLAY_FREQUENCY_59_94HZ - see the note above the ungated
	 * block.  Left to the end so the flips the checks depend on are done. */
	gcmSetVBlankFrequency(1);
	gcmSetSecondVFrequency(1);
	printf("  gcmSetVBlankFrequency / gcmSetSecondVFrequency returned\n");

	printf("  drew %d frames; %d of %d gated checks ran, fail mask 0x%08x\n",
	       frame, checks_run, EXPECTED_CHECKS, (unsigned)fail_mask);
	if (checks_run != EXPECTED_CHECKS)
		printf("  INCOMPLETE: %d gated checks did not run (early exit?)\n",
		       EXPECTED_CHECKS - checks_run);
	if (fail_mask) {
		for (int i = 0; i < 32; i++)
			if (fail_mask & (1u << i))
				printf("  check %d FAILED\n", i);
	}

	cellGcmSetWaitFlip(ctx);
	cellGcmFinish(ctx, 1);
	ioPadEnd();

	/* The verdict is printed AFTER teardown completes, so a hang anywhere
	 * above - including in Finish - cannot leave a PASS line in the log. */
	printf("hello-ppu-cellgcm-sysinfo: %s\n",
	       fail_mask ? "FAIL"
	                 : (checks_run == EXPECTED_CHECKS ? "PASS" : "INCOMPLETE"));

	return (fail_mask || checks_run != EXPECTED_CHECKS) ? 1 : 0;
}
