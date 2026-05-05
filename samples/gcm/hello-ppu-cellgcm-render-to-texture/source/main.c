/*
 * hello-ppu-cellgcm-render-to-texture
 *
 * Render-target switch test.  After standard display init we allocate
 * a small off-screen surface from the same RSX local-memory bump
 * allocator the display + depth buffers came from, switch the render
 * target to it via cellGcmSetSurface, clear it to a known colour, and
 * read the memory back through the PPU mapping to verify the clear
 * landed.
 *
 * ---- Memory model on PS3 -------------------------------------------
 *
 * The RSX has its own 256 MiB pool of GDDR3 ("RSX local memory")
 * physically separate from main memory.  PSL1GHT and the cell SDK
 * both expose it through a single base pointer + size pair returned
 * by cellGcmGetConfiguration:
 *
 *     CellGcmConfig cfg;
 *     cellGcmGetConfiguration(&cfg);
 *     // cfg.localAddress  : PPU virtual pointer to start of RSX RAM
 *     // cfg.localSize     : bytes available in that region
 *     // cfg.ioAddress     : PPU virtual pointer to the IO-map
 *     // cfg.ioSize        : bytes in the IO map (main memory mapped
 *     //                     into RSX's address space)
 *
 * The PPU sees RSX local memory as ordinary writable memory through
 * `cfg.localAddress`; the RSX sees the same bytes through *RSX
 * offsets* in NV40 commands.  cellGcmAddressToOffset translates a PPU
 * pointer back to the RSX offset the GPU expects.
 *
 * For RSX-local addresses the translation is pure subtraction:
 *
 *     rsx_offset = (uintptr_t)ptr - (uintptr_t)cfg.localAddress;
 *
 * which is why bumping a u32 through the local-memory base produces
 * the same result as `cellGcmAddressToOffset` and never aliases — as
 * long as everything that lives in RSX local memory comes out of the
 * *same* allocator.
 *
 * ---- Bump allocator pattern ---------------------------------------
 *
 * The reference SDK ships `cellGcmUtilAllocateLocalMemory` (in the
 * gcmutil sample helper, not the runtime) which is a one-pointer
 * bump allocator over [localAddress, localAddress + localSize).
 * No free, no metadata: the lifetime of every RSX-local allocation is
 * the lifetime of the GCM context itself, torn down by cellGcmFinish.
 *
 * We replicate that in `local_align` below.  Mixing this with PSL1GHT's
 * `rsxMemalign` would be unsafe — both want exclusive control of the
 * local-memory region and don't see each other's allocations, so the
 * bumps would overlap on the second one and silently trash the first.
 *
 * ---- Alignment requirements ---------------------------------------
 *
 * NV40 command-buffer rules dictate the minimum alignments:
 *
 *   - Color/depth surface bases : 64 B   (NV40TCL_DMA alignment)
 *   - Display buffer bases      : 64 B   (cellGcmSetDisplayBuffer)
 *   - Texture bases             : 128 B  (NV40TCL_TEX_OFFSET; we use
 *                                         128 throughout for safety)
 *   - Vertex / index buffers    : 128 B  (matches reference samples)
 *   - Surface row pitch         : 64 B   (multiple of 64 for LINEAR)
 *
 * The bump allocator below rounds *both* the cursor (alignment-up)
 * and the size (rounded to 1 KiB to be defensive), so consecutive
 * allocations land on at-least-1-KiB boundaries even when the caller
 * asks for 64-byte alignment.  Adopting a stricter pad keeps tile-
 * aligned regions easy to reason about and matches what the reference
 * `cellGcmUtilAllocateLocalMemory` does.
 *
 * ---- Surface-format pitch invariants ------------------------------
 *
 * `colorPitch` and `depthPitch` in CellGcmSurface are *bytes per
 * row*, not pixels.  RPCS3 strict mode validates pitch against the
 * declared format and the surface width, so:
 *
 *   - X8R8G8B8 / A8R8G8B8 colour   →  pitch = width * 4
 *   - Z16   depth                  →  pitch = width * 2
 *   - Z24S8 depth                  →  pitch = width * 4
 *
 * Mismatched pitch is a hard reject on RPCS3 and undefined-output on
 * real hardware.  We pick Z24S8 below specifically so the colour and
 * depth pitches share the same `width * 4` formula and there's only
 * one number to keep in sync.
 *
 * ---- Why bump-anchor at cfg.localAddress, not cfg.localAddress+N --
 *
 * `cellGcmInit` reserves a slice at the *top* of local memory for its
 * own bookkeeping (display reports, tile descriptors, the cursor
 * texture region in some configurations).  The reference SDK pulls
 * 32 MiB off the end via `cellGcmUtilInitializeLocalMemory` and only
 * hands out [base, base+size-32MiB).  We don't bother here because
 * the test only needs ~16 KiB of RTT plus a couple of MiB of frame
 * buffers; the hard ceiling is well above what we'll touch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sysutil/video.h>
#include <cell/gcm.h>

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

static u32  display_pitch;
static u32  depth_pitch;
static u32  depth_offset;
static u32 *depth_buffer;

/* ---- RSX local-memory bump allocator (reference SDK style) --------
 *
 * `g_local_mem_heap` is anchored at cfg.localAddress in init_screen
 * once cellGcmInit has handed us a configuration.  Every RSX-resident
 * allocation in this sample (display buffers, depth buffer, RTT
 * colour, RTT depth) flows through `local_align`, which means:
 *
 *   1. Allocations are issued in declaration order, head-to-tail.
 *   2. No two allocations ever overlap.
 *   3. Each returned pointer is already 64-byte aligned (or stronger
 *      if the caller requests it), so cellGcmAddressToOffset gives us
 *      a 64-byte aligned RSX offset suitable for any surface or
 *      texture base register.
 *
 * The 1 KiB size-roundup is overkill for the small RTT here but
 * keeps adjacent allocations on tile-aligned boundaries — matters
 * once tile/zcull regions enter the picture (cellGcmSetTile expects
 * the region to start on a tile-stride boundary).
 *
 * No free path: this whole pool is reclaimed when `cellGcmFinish`
 * tears down the GCM context.  Trying to free individual entries
 * back into the bump cursor would require LIFO discipline and
 * doesn't buy us anything inside a one-shot test. */
static u32 g_local_mem_heap;
static void *local_align(u32 alignment, u32 size)
{
	/* Round the cursor up to `alignment` first.  alignment must be a
	 * power of two — 64 / 128 / 4096 are the values we care about.
	 * The mask form below is faster than a divide and matches what
	 * cellGcmUtilAllocateLocalMemory does. */
	g_local_mem_heap = (g_local_mem_heap + alignment - 1u) & ~(alignment - 1u);
	void *p = (void *)(uintptr_t)g_local_mem_heap;
	/* Round the *size* up to 1 KiB so the next allocation lands on a
	 * 1 KiB boundary regardless of what alignment it requests.  This
	 * is what the reference helper does to keep tile regions clean. */
	g_local_mem_heap += (size + 1023u) & ~1023u;
	return p;
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

/* ---- FIFO instrumentation -----------------------------------------
 *
 * NV40 method header word: bits[31:18] = count, bits[17:0] = method
 * (byte offset into the channel's method space).  Each subsequent
 * data word writes to (method + 4*i); the GPU auto-increments unless
 * a NO_INCREMENT flag is set in the upper bits of the header.
 *
 * We dump the FIFO bytes around each surface-related call so we can
 * compare the actual on-the-wire NV40 stream against what a known-
 * working setup should emit. */
static const char *nv40_method_name(uint32_t m)
{
	switch (m) {
	case 0x18c: return "DMA_COLOR1";
	case 0x194: return "DMA_COLOR0";
	case 0x198: return "DMA_ZETA";
	case 0x1b4: return "DMA_COLOR2";
	case 0x1b8: return "DMA_COLOR3";
	case 0x200: return "RT_HORIZ";
	case 0x204: return "RT_VERT";
	case 0x208: return "RT_FORMAT";
	case 0x20c: return "COLOR0_PITCH";
	case 0x210: return "COLOR0_OFFSET";
	case 0x214: return "ZETA_OFFSET";
	case 0x218: return "COLOR1_OFFSET";
	case 0x21c: return "COLOR1_PITCH";
	case 0x220: return "RT_ENABLE";
	case 0x22c: return "ZETA_PITCH";
	case 0x280: return "COLOR2_PITCH";
	case 0x284: return "COLOR3_PITCH";
	case 0x288: return "COLOR2_OFFSET";
	case 0x28c: return "COLOR3_OFFSET";
	case 0x2b8: return "WINDOW_OFFSET";
	case 0x1d88: return "SHADER_WINDOW";
	case 0x1d8c: return "CLEAR_VALUE_DEPTH";
	case 0x1d90: return "CLEAR_VALUE_COLOR";
	case 0x1d94: return "CLEAR_BUFFERS";
	default: return NULL;
	}
}

static void dump_fifo_window(const char *label, uint32_t *before, uint32_t *after)
{
	if (after == before) {
		printf("    [FIFO %s] empty\n", label);
		return;
	}
	if (after < before) {
		printf("    [FIFO %s] WRAPPED before=%p after=%p — skipping\n",
		       label, before, after);
		return;
	}
	size_t n = (size_t)(after - before);
	printf("    [FIFO %s] %zu word(s) at %p:\n", label, n, before);
	size_t i = 0;
	while (i < n) {
		uint32_t hdr     = before[i];
		uint32_t count   = (hdr >> 18) & 0x07ff;
		uint32_t method  = hdr & 0xffff;          /* low 16 bits */
		uint32_t channel = (hdr >> 13) & 7;
		uint32_t no_inc  = (hdr >> 30) & 1;
		const char *nm   = nv40_method_name(method);
		printf("      +%04zu  hdr=%08x  ch=%u m=0x%04x %-18s count=%u%s",
		       i * 4, hdr, channel, method,
		       nm ? nm : "(?)", count, no_inc ? " NO_INC" : "");
		size_t take = count;
		if (i + 1 + take > n) take = n - i - 1;
		for (size_t j = 0; j < take; j++) {
			if ((j & 3) == 0) printf("\n            ");
			printf(" %08x", before[i + 1 + j]);
		}
		printf("\n");
		i += 1 + count;
		if (count == 0) {
			/* possible JUMP/CALL/NOP single-word commands */
			i += 0;
			if (hdr == 0) break;  /* zero word — stop */
		}
	}
}

#define FIFO_MARK_BEFORE(ctx)   ((uint32_t *)(uintptr_t)(ctx)->current)
#define FIFO_DUMP(label, ctx, before)  do {                          \
	uint32_t *_after = (uint32_t *)(uintptr_t)(ctx)->current;        \
	dump_fifo_window((label), (before), _after);                     \
} while (0)

static int do_flip(CellGcmContextData *ctx, uint8_t buffer_id)
{
	if (cellGcmSetFlip(ctx, buffer_id) == 0) {
		cellGcmFlush(ctx);
		cellGcmSetWaitFlip(ctx);
		return 1;
	}
	return 0;
}

/*
 * Allocate one display buffer in RSX local memory and hand its RSX
 * offset to cellGcmSetDisplayBuffer for buffer slot `id`.
 *
 * Layout: width*4 (X8R8G8B8) bytes per row, height rows.  The 64-byte
 * `local_align` request matches NV40's display-buffer alignment rule.
 * cellGcmAddressToOffset converts the PPU pointer back to the RSX
 * offset (a simple subtract from cfg.localAddress for local-mem
 * pointers) which is what the GPU's display engine reads.
 */
static int make_buffer(display_buffer *b, uint16_t width, uint16_t height, int id)
{
	int pitch = (int)(width * sizeof(uint32_t));
	int size  = pitch * height;

	b->ptr = (uint32_t *)local_align(64, (u32)size);
	if (!b->ptr)
		return 0;

	if (cellGcmAddressToOffset(b->ptr, &b->offset) != 0)
		return 0;

	if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, (u32)pitch, width, height) != 0)
		return 0;

	b->width  = width;
	b->height = height;
	b->id     = id;
	return 1;
}

static int init_screen(void *host_addr, uint32_t size,
                       uint16_t *out_w, uint16_t *out_h)
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

	/* Anchor the bump allocator to RSX local-memory base.  Display
	 * buffers, depth buffer, and the RTT all bump from here.  After
	 * this assignment the heap cursor advances monotonically with
	 * every `local_align` call, never overlapping a previous range.
	 *
	 * Storing as u32 (not a pointer): we'll bump it by integer byte
	 * counts.  cfg.localAddress is a fully-extended 64-bit pointer
	 * provided by our cellGcmGetConfiguration shim — but the RSX
	 * local-memory region is a single 256 MiB span so the bottom 32
	 * bits are sufficient to address every byte in it. */
	CellGcmConfig cfg;
	cellGcmGetConfiguration(&cfg);
	g_local_mem_heap = (u32)(uintptr_t)cfg.localAddress;

	/* X8R8G8B8 colour and Z24S8 depth both use 4 bytes/pixel, so a
	 * single `width * 4` pitch holds for both color and depth.  We
	 * call out the difference in identifiers anyway so future readers
	 * don't need to remember the sizeof match. */
	display_pitch = res.width * sizeof(uint32_t);  /* X8R8G8B8 = 4 B/px */
	depth_pitch   = res.width * sizeof(uint32_t);  /* Z24S8    = 4 B/px */

	/* Depth allocated up-front, before the display buffers, so its
	 * offset stays the same across the entire run — set_render_target
	 * reuses `depth_offset` every frame.  The render-target depth
	 * region only needs `height * depth_pitch` bytes; we don't
	 * overscan. */
	depth_buffer  = (u32 *)local_align(64, res.height * depth_pitch);
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
	sf.colorPitch[0]    = display_pitch;
	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1]    = 64;
	sf.colorPitch[2]    = 64;
	sf.colorPitch[3]    = 64;
	sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
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

/* ---- RTT test ---- */

#define RTT_W     64
#define RTT_H     64
#define RTT_COLOR 0xFF004488
#define RTT_FILL  0xCDCDCDCD

int main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-cellgcm-render-to-texture\n");

	void               *host_addr;
	display_buffer      buffers[MAX_BUFFERS];
	uint16_t            w = 0, h = 0;
	CellGcmContextData *ctx;
	CellGcmConfig       gcm_cfg;

	host_addr = memalign(1024 * 1024, HOST_SIZE);
	if (!init_screen(host_addr, HOST_SIZE, &w, &h)) {
		printf("  init_screen failed\n");
		free(host_addr);
		return 1;
	}

	ctx = (CellGcmContextData *)gCellGcmCurrentContext;
	cellGcmGetConfiguration(&gcm_cfg);

	printf("  RSX %ux%u  local base=%p  depth_off=0x%x\n",
	       w, h, gcm_cfg.localAddress, depth_offset);

	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!make_buffer(&buffers[i], w, h, i)) {
			printf("  make_buffer[%d] failed\n", i);
			cellGcmFinish(ctx, 0);
			free(host_addr);
			return 1;
		}
	}
	do_flip(ctx, (uint8_t)buffers[MAX_BUFFERS - 1].id);

	/* ---- allocate off-screen RTT surface ----
	 *
	 * Both the RTT colour surface and its private depth buffer go
	 * through the same `local_align` bump allocator that placed the
	 * display + depth buffers above.  The cursor is monotonically
	 * increasing, so:
	 *
	 *     cfg.localAddress
	 *       ├─ depth (init_screen)             ─┐
	 *       ├─ display buffer 0 (make_buffer)  ─┤  same allocator,
	 *       ├─ display buffer 1 (make_buffer)  ─┤  same cursor —
	 *       ├─ rtt colour          (here)      ─┤  no overlap is
	 *       └─ rtt depth           (here)      ─┘  possible.
	 *
	 * Each region was issued at 64-byte alignment plus 1 KiB pad,
	 * so the corresponding RSX offsets are clean for surface set.
	 *
	 * Format choices below use Z24S8 for the RTT depth so colour and
	 * depth share the same `RTT_W * 4` pitch (see top-of-file note on
	 * pitch invariants).  Z16 would need pitch RTT_W*2 = 128 — easy
	 * to set right but easy to get wrong, and RPCS3 strict mode
	 * rejects mismatched depth pitch. */
	uint32_t rtt_pitch = RTT_W * 4;     /* bytes per row, both colour and depth */
	uint32_t rtt_sz    = rtt_pitch * RTT_H;

	/* Colour: 64-byte alignment is the NV40 minimum for surface
	 * bases.  cellGcmAddressToOffset translates the PPU pointer back
	 * to the RSX-side offset (= ptr - cfg.localAddress for
	 * local-resident allocations). */
	void    *rtt_ptr   = local_align(64, rtt_sz);
	uint32_t rtt_off   = 0;
	if (!rtt_ptr || cellGcmAddressToOffset(rtt_ptr, &rtt_off) != 0) {
		printf("  RTT colour alloc failed\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 1;
	}

	/* Depth: same alignment story; its base is independent of the
	 * colour base and lives further down the bump cursor.  Sharing a
	 * single buffer for colour-and-depth is illegal — the GPU writes
	 * both simultaneously. */
	void    *rtt_depth_ptr = local_align(64, rtt_sz);
	uint32_t rtt_depth_off = 0;
	if (!rtt_depth_ptr || cellGcmAddressToOffset(rtt_depth_ptr, &rtt_depth_off) != 0) {
		printf("  RTT depth alloc failed\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 1;
	}

	memset(rtt_ptr, (RTT_FILL & 0xFF), rtt_sz);
	printf("  RTT colour @ off=0x%x  PPU=%p  fill=0x%08x\n",
	       rtt_off, rtt_ptr, ((uint32_t *)rtt_ptr)[0]);
	printf("  RTT depth  @ off=0x%x  PPU=%p\n",
	       rtt_depth_off, rtt_depth_ptr);

	/* ---- Set the backbuffer EXPLICITLY first, then switch to RTT ---- */
	{
		uint32_t *_before = FIFO_MARK_BEFORE(ctx);
		set_render_target(ctx, &buffers[0]);
		FIFO_DUMP("set_render_target(backbuffer)", ctx, _before);
	}
	wait_rsx_idle(ctx);
	printf("  backbuffer surface set\n");

	/* now switch to RTT surface and clear */
	{
		CellGcmSurface sf;
		memset(&sf, 0, sizeof(sf));
		sf.colorFormat      = GCM_SURFACE_A8R8G8B8;
		sf.colorTarget      = GCM_SURFACE_TARGET_0;
		sf.colorLocation[0] = GCM_LOCATION_RSX;
		sf.colorOffset[0]   = rtt_off;
		sf.colorPitch[0]    = rtt_pitch;
		sf.colorLocation[1] = GCM_LOCATION_RSX;
		sf.colorLocation[2] = GCM_LOCATION_RSX;
		sf.colorLocation[3] = GCM_LOCATION_RSX;
		sf.colorPitch[1]    = 64;
		sf.colorPitch[2]    = 64;
		sf.colorPitch[3]    = 64;
		sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
		sf.depthLocation    = GCM_LOCATION_RSX;
		sf.depthOffset      = rtt_depth_off;
		sf.depthPitch       = rtt_pitch;
		sf.type             = GCM_SURFACE_TYPE_LINEAR;
		sf.antiAlias        = GCM_SURFACE_CENTER_1;
		sf.width            = RTT_W;
		sf.height           = RTT_H;
		sf.x                = 0;
		sf.y                = 0;

		printf("  RTT surface: colorOff=0x%x pitch=%u depthOff=0x%x depthPitch=%u %ux%u\n",
		       sf.colorOffset[0], sf.colorPitch[0],
		       sf.depthOffset, sf.depthPitch,
		       sf.width, sf.height);

		uint32_t *_before = FIFO_MARK_BEFORE(ctx);
		cellGcmSetSurface(ctx, &sf);
		FIFO_DUMP("cellGcmSetSurface(RTT 64x64 A8R8G8B8)", ctx, _before);
	}

	{
		uint32_t *_before = FIFO_MARK_BEFORE(ctx);
		cellGcmSetClearColor(ctx, RTT_COLOR);
		FIFO_DUMP("cellGcmSetClearColor(0xFF004488)", ctx, _before);
	}
	{
		uint32_t *_before = FIFO_MARK_BEFORE(ctx);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
		FIFO_DUMP("cellGcmSetClearSurface(R|G|B|A)", ctx, _before);
	}

	/* wait for RSX to complete the clear */
	wait_rsx_idle(ctx);

	/* ---- verify ---- */
	uint32_t *pix = (uint32_t *)rtt_ptr;
	int pass = 1;
	for (int i = 0; i < 8; i++) {
		int ok = (pix[i] == RTT_COLOR);
		printf("    pixel[%d] = 0x%08x  %s\n",
		       i, pix[i], ok ? "PASS" : "FAIL");
		if (!ok) pass = 0;
	}

	if (pass)
		printf("  *** RTT switch: PASS ***\n");
	else {
		printf("  *** RTT switch: FAIL ***\n");
		printf("  dump:");
		for (int i = 0; i < 16; i++)
			printf(" 0x%08x", pix[i]);
		printf("\n");
	}

	/* cleanup — bump-allocator memory belongs to the lifetime of the
	 * RSX local-memory pool; cellGcmFinish tears that down. */
	cellGcmSetWaitFlip(ctx);
	cellGcmFinish(ctx, 1);
	free(host_addr);

	printf("hello-ppu-cellgcm-render-to-texture: done\n");
	return pass ? 0 : 1;
}
