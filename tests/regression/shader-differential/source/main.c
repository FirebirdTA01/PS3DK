/*
 * shader-differential — A/B pixel-differential rig (increment 1).
 *
 * Judges PAIRS of compiled fragment-program containers: renders both
 * sides of each pair into off-screen RSX render targets in the SAME
 * run, reads both back through the transfer engine, and raw-byte
 * compares the two images.  The gate is bit-identical (max_delta == 0)
 * by default; a documented exception class (MAD-fusion contraction)
 * is probed by a dedicated manifest row, not assumed.
 *
 * What this app knows NOTHING about: which compiler produced either
 * container.  Pairs arrive as FILES staged to /dev_hdd0 by the
 * host-side stager, so reference-compiled material is runtime input
 * only and never enters the tree, the build, or this binary.  The app
 * judges "do these two containers paint the same pixels", full stop.
 *
 * Row protocol (one line per judged pair, scraped from the TTY):
 *   SDIFF|tier=B|role=<role>|shader=<name>|compiler=ab|uniform_set=<s>
 *        |target=<t>|status=<status>|max_delta=<n>|diff_pixels=<n>
 *        |total_pixels=<n>|diagnostic=<d>|elapsed_ms=<n>|artifact=<a>
 * roles:  control-identical | control-mismatch | control-uniform |
 *         control-texture | control-auto | corpus | probe |
 *         reference (ours vs a reference-compiled container; gated
 *         like corpus) | path-pair (our DEFAULT-path container vs our
 *         GENERAL-path container of the same shader; gated like corpus,
 *         but judged ONLY when the row immediately before it is a
 *         reference row that judged identical - that row is the
 *         premise that makes the default container an oracle for the
 *         general one; otherwise status path-pair-unoracled, counted
 *         on its own line, neither pass nor fail)
 * status: identical | mismatch | load-failed-a | load-failed-b |
 *         uniform-missing-a | uniform-missing-b | uniforms-invalid |
 *         textures-invalid | samplers-unvalidated |
 *         sampler-unsupported-a | sampler-unsupported-b |
 *         auto-invalid | uniform-unsupported-a | uniform-unsupported-b |
 *         internal-error (a render_side code the judge does not map) |
 *         vacuous (neither side painted a pixel: no verdict, never
 *         counted as identical) | path-pair-unoracled (the premise row
 *         before a path-pair row did not judge identical) |
 *         unstable-a | unstable-b (that side painted but no two
 *         consecutive readbacks agreed: output varies between draws)
 *
 * Sensitivity, in every judged row's diagnostic: `levels a=R/G/B/A
 * b=R/G/B/A` = distinct 8-bit values per channel per side, and `sat`
 * = share of pixels with any channel at 0 or 255.  A green row with
 * three levels per channel agreed on nothing worth quoting; the numbers
 * make that visible where the verdict is read (the vita-cg room's
 * false-pass shapes).
 * Cost, in every judged row's diagnostic: `size a=N b=N insn a=N b=N
 * params a=N b=N` - container bytes, ucode/16, container parameter
 * count - so the price of a green is on the row (t_3bf3ce95 compares
 * const promotion against folding on exactly these).
 *
 * Poison canary: after every row past the standing controls the rig
 * draws the identity control's container once; if that paints nothing
 * the run ends SHADER_DIFF_INVALID naming the row that poisoned the
 * state (measured: one malformed general-path program blanked every
 * draw after it).
 *
 * Uniform synthesis (increment 3b): a row whose uniform_set is `auto`
 * has every float/half vector uniform its containers declare set to
 * values derived from the PARAMETER NAME (FNV-1a, see auto_value), the
 * same on both sides.  The control-auto row (uniform output vs a twin
 * the stager baked from the same hash) proves guest and host agree;
 * while it is red no auto row is judged.  Matrix, int and bool uniforms
 * are refused as uniform-unsupported-a/b rather than left at their
 * embedded defaults.
 *
 * Auto-binder (increment 3a): every sampler2D a container declares is
 * bound to ONE procedural 64x64 texture (both sides alike), and the
 * shared VP drives every interpolated channel from the quad's (u,v).
 * The control-texture row (sampled vs arithmetic twin) proves the
 * binding lands; while it is red, or absent, no sampler-declaring pair
 * is judged -- the same shape as the uniform gate.
 * (`ours-refused` rows are emitted host-side by the stager for shaders
 * our compiler refused — no container exists, so the guest never sees
 * them; the scraper merges both sources.)
 *
 * Validity gate, enforced in-guest: the first two manifest rows MUST
 * be the standing controls, in order — control-identical (one
 * container byte-copied twice; must judge identical) then
 * control-mismatch (two different shaders; must judge mismatch).
 * BOTH controls are always judged — one run reports on both — and if
 * either judged wrong, no CORPUS or probe pair is judged and the run
 * ends SHADER_DIFF_INVALID: a comparator that cannot see a deliberate
 * difference, or sees one in a byte-copy, is judging nothing, and
 * "invalid" is not "failed" — different bugs, different sentinels.
 * (Verified by sabotage in co-lead review: a mis-pointed row 1 fired
 * the gate with the named diagnostic and left the probe unjudged.)
 *
 * Probe rows measure; they never gate.  A probe mismatch is the
 * ANSWER to a question (is the exception class populated as visible
 * to this rig?), not a failure.
 *
 * Increment-1 scope, stated: FP pairs only (both sides render with
 * one build-time-embedded pass-through VP that is not under test).
 * On mismatch both raw images are dumped to artifacts/ for host-side
 * inspection.
 *
 * Uniform application (increment 2b): uniform_set names a set in the
 * staged uniforms.txt (`set|name|x,y,z,w` per line); every entry of
 * the pair's set is applied to BOTH sides by parameter name before
 * the draw (cellGcmSetFragmentProgramParameter patches the embedded
 * constants of the COPIED ucode through the FIFO's inline-transfer
 * path).  A container missing a named parameter fails its side with
 * status uniform-missing-a/b.  uniform_set "0" means none.
 *
 * The uniform plumbing has its own CONTROL, because silent
 * non-application is invisible to an A/B rig (both sides would show
 * the same embedded defaults and still judge identical): the stager
 * pairs a shader whose output IS a uniform against a twin with the
 * same value BAKED as a literal, under a set carrying that value.
 * If application works the pair is identical; if it silently fails,
 * the uniform side shows the embedded default and the row judges
 * mismatch.  When that control fails, every later pair whose
 * uniform_set != 0 is SKIPPED with a named diagnostic (row status
 * uniforms-invalid) rather than judged against unapplied values —
 * plain pairs still run, so a uniform-plumbing bug cannot invalidate
 * the whole corpus.
 *
 * RSX plumbing is the shader-readback rig's proven path (RT switch,
 * label-seeded idle, NV3089 image transfer for readback — see that
 * rig's comments for why cellGcmSetTransferData is unusable and why
 * draws retry on a cold RPCS3 shader cache).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/time.h>

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sysutil/video.h>
#include <cell/gcm.h>
#include <rsx/rsx.h>

#include "sd_pos_allch_vpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2

#define GCM_LABEL_INDEX 255

#define RT_W 64
#define RT_H 64
/* Readback fill before every transfer.  Two values, alternating per
 * readback: a transfer that never lands leaves the fill behind, and
 * two consecutive fills DIFFER, so it can never satisfy the warm-up's
 * two-identical-frames rule and surfaces as `unstable`, which is what
 * an instrument that could not read is.  The fill is NOT excluded from
 * the paint count: the first fill was opaque black, and a shader whose
 * honest output is opaque black (sd_const_promotion under t_4584aa27,
 * K dropped to zero) read as "painted 0" for eleven draws and would
 * have read as vacuous against a black reference.  Any 32-bit colour
 * is a colour some shader can paint; only the clear mark, which the
 * rig itself wrote, is evidence of no draw. */
#define CLEAR_SENTINEL_EVEN 0xFF000000u
#define CLEAR_SENTINEL_ODD  0xFF000001u
#define GPU_CLEAR_MARK 0xFF102030u  /* GPU-clear control color */

#define SD_ROOT "/dev_hdd0/shader-differential/"
#define MAX_PAIRS 512
#define PATH_MAX_SD 256

typedef struct {
	char tier[8];
	char role[24];
	char name[64];
	char a_path[PATH_MAX_SD];
	char b_path[PATH_MAX_SD];
	char uniform_set[16];
} sd_pair;

typedef struct {
	float pos[2];
	float uv[2];
} vertex_t;

/* ---- RSX local-memory bump allocator (see render-to-texture) ---- */

static u32 g_local_mem_heap;
static void *local_align(u32 alignment, u32 size)
{
	g_local_mem_heap = (g_local_mem_heap + alignment - 1u) & ~(alignment - 1u);
	void *p = (void *)(uintptr_t)g_local_mem_heap;
	g_local_mem_heap += (size + 1023u) & ~1023u;
	return p;
}

static void wait_rsx_idle(CellGcmContextData *ctx)
{
	/* Seed each wait from the CURRENT label value: write current+1
	 * through the GPU pipeline and wait for it — a value the slot
	 * cannot already hold, so stale contents can't satisfy the wait. */
	vu32 *slot = (vu32 *)cellGcmGetLabelAddress(GCM_LABEL_INDEX);
	uint32_t target = *slot + 1u;
	cellGcmSetWriteBackEndLabel(ctx, GCM_LABEL_INDEX, target);
	cellGcmFlush(ctx);
	while (*slot != target)
		usleep(30);
}

/* ---- display init (proven path from render-to-texture) ---- */

typedef struct {
	uint32_t *ptr;
	uint32_t  offset;
} display_buffer;

static u32 display_pitch;
static u32 display_w, display_h;
static u32 disp_depth_offset;

static int init_screen(void *host_addr, uint32_t size)
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

	CellGcmConfig cfg;
	cellGcmGetConfiguration(&cfg);
	g_local_mem_heap = (u32)(uintptr_t)cfg.localAddress;

	display_w     = res.width;
	display_h     = res.height;
	display_pitch = res.width * sizeof(uint32_t);

	void *dd = local_align(64, display_h * display_pitch);
	cellGcmAddressToOffset(dd, &disp_depth_offset);
	cellGcmResetFlipStatus();
	return 1;
}

static int make_buffer(display_buffer *b, int id)
{
	int pitch = (int)display_pitch;
	int size  = pitch * (int)display_h;
	b->ptr = (uint32_t *)local_align(64, (u32)size);
	if (!b->ptr) return 0;
	if (cellGcmAddressToOffset(b->ptr, &b->offset) != 0) return 0;
	if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, (u32)pitch,
	                            display_w, display_h) != 0) return 0;
	return 1;
}

/* ---- render-target + draw-env plumbing (readback rig's path) ---- */

static void set_rt_surface(CellGcmContextData *ctx,
                           u32 color_off, u32 depth_off, u32 pitch,
                           u16 w, u16 h)
{
	CellGcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat      = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = color_off;
	sf.colorPitch[0]    = pitch;
	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1]    = 64;
	sf.colorPitch[2]    = 64;
	sf.colorPitch[3]    = 64;
	sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation    = GCM_LOCATION_RSX;
	sf.depthOffset      = depth_off;
	sf.depthPitch       = pitch;
	sf.type             = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias        = GCM_SURFACE_CENTER_1;
	sf.width            = w;
	sf.height           = h;
	sf.x                = 0;
	sf.y                = 0;
	cellGcmSetSurface(ctx, &sf);
}

static void set_draw_env(CellGcmContextData *ctx, u16 w, u16 h)
{
	cellGcmSetColorMask(ctx,
		GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	cellGcmSetColorMaskMrt(ctx, 0);

	float min = 0.0f, max = 1.0f;
	float scale[4]  = { w * 0.5f, h * -0.5f, (max - min) * 0.5f, 0.0f };
	float offset[4] = { w * 0.5f, h *  0.5f, (max + min) * 0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, w, h, min, max, scale, offset);
	rsxSetScissor(ctx, 0, 0, w, h);

	cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetBlendEnable(ctx, GCM_FALSE);
}

/* ---- shared state ---- */

static CGprogram vpo;
static void     *vp_ucode;
static int       position_index;
static int       texcoord_index;
static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;

static u32 *g_readback;
static u32  g_readback_off;

/* ---- auto-binder: one procedural texture for every sampler ---- */

#define TEX_W 64
#define TEX_H 64
static u32 g_tex_offset;

/* The texture control's identity (texel (px, py) == pixel (px, py))
 * depends on one texel per pixel AND on single-sample fragment centres:
 * the half-texel margin in floor(uv * 64) is exactly half a pixel, which
 * is exactly the displacement multisampling would introduce.  If the rig
 * ever multisamples its RTs, the control mismatches and would read as a
 * compiler regression (review note on the 3a commit). */
typedef char sd_tex_matches_rt_w[(TEX_W == RT_W) ? 1 : -1];
typedef char sd_tex_matches_rt_h[(TEX_H == RT_H) ? 1 : -1];

/* Texel (x, y) = (R, G, B, A) = (4x, 4y, 4(63 - x), 4y) in 8-bit units:
 * every channel a linear ramp the texture control's arithmetic twin
 * recomputes from the interpolated texcoord, and alpha ramping 0..~1
 * down the image so a discard-on-alpha shader exercises both branches.
 * 64x64 against the 64x64 RT with nearest sampling: pixel (px, py)
 * reads texel (px, py), no filtering in the loop. */
static int init_procedural_texture(void)
{
	u32 *px = (u32 *)local_align(128, TEX_W * TEX_H * sizeof(u32));
	for (u32 y = 0; y < TEX_H; y++)
		for (u32 x = 0; x < TEX_W; x++)
			px[y * TEX_W + x] = ((4u * y) << 24) | ((4u * x) << 16)
			                  | ((4u * y) << 8) | (4u * (63u - x));
	return cellGcmAddressToOffset(px, &g_tex_offset) == 0;
}

/* Bind recipe from the cellgcm discard-blend sample (proven on the
 * emulator), with NEAREST filtering so the control's texel arithmetic
 * holds exactly. */
static void bind_procedural_texture(CellGcmContextData *ctx, u32 unit)
{
	CellGcmTexture t = {0};
	t.format    = CELL_GCM_TEXTURE_A8R8G8B8
	            | CELL_GCM_TEXTURE_LN
	            | CELL_GCM_TEXTURE_NR;
	t.mipmap    = 1;
	t.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
	t.cubemap   = CELL_GCM_FALSE;
	t.remap     = (CELL_GCM_TEXTURE_REMAP_REMAP <<  8)
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 10)
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 12)
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 14)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_A <<  0)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_R <<  2)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_G <<  4)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_B <<  6);
	t.width     = (u16)TEX_W;
	t.height    = (u16)TEX_H;
	t.depth     = 1;
	t.location  = CELL_GCM_LOCATION_LOCAL;
	t.pitch     = (u32)(TEX_W * sizeof(u32));
	t.offset    = g_tex_offset;
	cellGcmSetTexture(ctx, (uint8_t)unit, &t);
	cellGcmSetTextureControl(ctx, (uint8_t)unit, CELL_GCM_TRUE,
	                         0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
	cellGcmSetTextureFilter(ctx, (uint8_t)unit, 0,
	                        CELL_GCM_TEXTURE_NEAREST,
	                        CELL_GCM_TEXTURE_NEAREST,
	                        CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	cellGcmSetTextureAddress(ctx, (uint8_t)unit,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
	                         CELL_GCM_TEXTURE_ZFUNC_LESS,
	                         0);
}

/* Every CG sampler kind, by exact value: the flat family 1065..1069
 * (1D, 2D, 3D, RECT, CUBE), the three array kinds 1138..1140 and the
 * generic CG_SAMPLER 1143.  NOT a range over 1138..1143: 1141 and 1142
 * are CG_VERTEXSHADER_TYPE / CG_PIXELSHADER_TYPE (review finding).
 * Any sampler kind that is not sampler2D on a texture unit is REFUSED,
 * so "unserved" is always loud (t_e230822b). */
static int is_sampler_type(u32 type)
{
	return (type >= CG_SAMPLER1D && type <= CG_SAMPLERCUBE) ||
	       type == CG_SAMPLER1DARRAY || type == CG_SAMPLER2DARRAY ||
	       type == CG_SAMPLERCUBEARRAY || type == CG_SAMPLER;
}

/* Walks the container's parameter table (a flat array; "leaf" is the
 * Cg API's word, there is no tree) and binds the procedural texture to
 * every sampler2D unit it declares.  Returns the count bound, or -1
 * when a REFERENCED sampler of a kind this binder does not serve, or
 * an out-of-range unit, is declared: such a pair cannot be judged, and
 * says so rather than sampling nothing.  A declared-but-unreferenced
 * sampler is skipped: refusal is use-based, not declaration-based. */
static int bind_container_samplers(CellGcmContextData *ctx, CGprogram fpo)
{
	int n = 0;
	for (CGparameter prm = cellGcmCgGetFirstLeafParameter(fpo); prm;
	     prm = cellGcmCgGetNextLeafParameter(fpo, prm)) {
		u32 type = cellGcmCgGetParameterType(fpo, prm);
		if (!is_sampler_type(type))
			continue;
		if (!cellGcmCgGetParameterReferenced(fpo, prm))
			continue;
		u32 res = cellGcmCgGetParameterResource(fpo, prm);
		if (type != CG_SAMPLER2D || res < CG_TEXUNIT0 || res > CG_TEXUNIT15)
			return -1;
		bind_procedural_texture(ctx, res - CG_TEXUNIT0);
		n++;
	}
	return n;
}

/* ---- auto-binder: uniform values from parameter names ---- */

/* auto_value(name, k) is the k-th component of the value the binder
 * gives a uniform called `name`.  FNV-1a over the name, one integer
 * mix per component, then 0.125 + m / 2^24 with m < 2^23 - a dyadic
 * rational every float32 holds EXACTLY, so the stager can bake the
 * identical number into the control-auto twin from its own copy of
 * this arithmetic (stage-differential.ps1, Auto-Value).  Range
 * [0.125, 0.625): finite, non-zero, inside a colour channel.  The
 * contract hashes the name's BYTES as unsigned (the host hashes
 * UTF-8); Cg identifiers are ASCII in practice, so the two agree
 * without either side asserting the container's encoding. */
static float auto_value(const char *name, unsigned k)
{
	u32 h = 2166136261u;
	for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
		h ^= (u32)*c;
		h *= 16777619u;
	}
	u32 x = h ^ ((u32)k * 0x9E3779B1u);
	x *= 0x85EBCA6Bu;
	x ^= x >> 13;
	return 0.125f + (float)(x >> 9) / 16777216.0f;
}

/* Applies auto values to every float/half vector uniform the container
 * declares.  Returns the count applied, or -1 on a uniform of a kind
 * the binder does not synthesise (matrix, int, bool, fixed): the pair
 * is refused rather than judged against embedded defaults. */
static int apply_auto_uniforms(CellGcmContextData *ctx, CGprogram fpo,
                               u32 fp_offset)
{
	int n = 0;
	for (CGparameter prm = cellGcmCgGetFirstLeafParameter(fpo); prm;
	     prm = cellGcmCgGetNextLeafParameter(fpo, prm)) {
		if (cellGcmCgGetParameterVariability(fpo, prm) != CG_UNIFORM)
			continue;
		u32 type = cellGcmCgGetParameterType(fpo, prm);
		if (is_sampler_type(type))
			continue;   /* the sampler half's business */
		unsigned words;
		if (type >= CG_FLOAT && type <= CG_FLOAT4)
			words = type - CG_FLOAT + 1u;
		else if (type >= CG_HALF && type <= CG_HALF4)
			words = type - CG_HALF + 1u;
		else
			return -1;
		const char *name = cellGcmCgGetParameterName(fpo, prm);
		if (!name)
			return -1;
		float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (unsigned k = 0; k < words; k++)
			v[k] = auto_value(name, k);
		cellGcmSetFragmentProgramParameter(ctx, fpo, prm, v, fp_offset);
		n++;
	}
	return n;
}

static char g_target[16] = "emulator";  /* @target manifest directive */

static void transfer_rt_to_main(CellGcmContextData *ctx, u32 rt_off, u32 rt_pitch)
{
	/* NV3089 image blit — see the readback rig for why the
	 * TransferData emitter cannot be used. */
	cellGcmSetTransferImage(ctx, CELL_GCM_TRANSFER_LOCAL_TO_MAIN,
	                        g_readback_off, rt_pitch, 0, 0,
	                        rt_off, rt_pitch, 0, 0,
	                        RT_W, RT_H, 4);
	wait_rsx_idle(ctx);
}

/* ---- container loading ---- */

/* Reads SD_ROOT-relative `rel` into a 128-aligned buffer.  Returns
 * NULL on any failure; *out_size untouched then. */
static void *load_container(const char *rel, u32 *out_size)
{
	char full[PATH_MAX_SD + sizeof(SD_ROOT)];
	snprintf(full, sizeof(full), SD_ROOT "%s", rel);

	FILE *f = fopen(full, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return NULL;
	}
	void *buf = memalign(128, (size_t)sz);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*out_size = (u32)sz;
	return buf;
}

/* ---- staged uniform sets (uniforms.txt: `set|name|x,y,z,w`) ---- */

#define MAX_UNIFORMS 64

typedef struct {
	char  set[16];
	char  name[48];
	float values[4];
} sd_uniform;

static sd_uniform g_uniforms[MAX_UNIFORMS];
static int        g_nuniforms;

/* Absent file is fine (no sets defined); a malformed LINE is not —
 * a silently dropped uniform is a pair judged against the wrong
 * values.  Returns 0 only on malformed content. */
static int load_uniforms(void)
{
	FILE *f = fopen(SD_ROOT "uniforms.txt", "r");
	if (!f)
		return 1;
	char line[256];
	int lineno = 0;
	while (fgets(line, sizeof(line), f)) {
		lineno++;
		size_t len = strlen(line);
		while (len && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = 0;
		if (!len || line[0] == '#')
			continue;
		if (g_nuniforms >= MAX_UNIFORMS) {
			printf("shader-differential: uniforms.txt exceeds %d entries\n",
			       MAX_UNIFORMS);
			fclose(f);
			return 0;
		}
		sd_uniform *u = &g_uniforms[g_nuniforms];
		float v0, v1, v2, v3;
		char setbuf[16], namebuf[48];
		if (sscanf(line, "%15[^|]|%47[^|]|%f,%f,%f,%f",
		           setbuf, namebuf, &v0, &v1, &v2, &v3) != 6) {
			printf("shader-differential: uniforms.txt line %d malformed: '%s'\n",
			       lineno, line);
			fclose(f);
			return 0;
		}
		snprintf(u->set,  sizeof(u->set),  "%s", setbuf);
		snprintf(u->name, sizeof(u->name), "%s", namebuf);
		u->values[0] = v0; u->values[1] = v1;
		u->values[2] = v2; u->values[3] = v3;
		g_nuniforms++;
	}
	fclose(f);
	return 1;
}

static int uniformSetIsNone(const char *set)
{
	return !set[0] || strcmp(set, "0") == 0 || strcmp(set, "-") == 0;
}

/* Sensitivity of one image, per channel: how many distinct 8-bit levels
 * it uses and what share of its pixels sit at 0 or 255.  An image with
 * three levels per channel, or nine-tenths of it saturated, can agree
 * with a completely wrong transcription of the same shader at
 * max_delta 0 - the comparator has nothing to disagree about.  Not a
 * verdict on its own: reported in every row so a green can be read for
 * what it is worth (the vita-cg room's five false-pass shapes, all
 * "not blank, no sensitivity"). */
typedef struct {
	int levels[4];    /* distinct values per channel, R G B A */
	int saturated;    /* pixels with any channel at 0 or 255, of RT_W*RT_H */
} sd_sensitivity;

static void measure_sensitivity(const u32 *img, u32 rt_pitch, sd_sensitivity *out)
{
	static unsigned char seen[4][256];
	memset(seen, 0, sizeof(seen));
	out->saturated = 0;
	for (u32 y = 0; y < RT_H; y++)
		for (u32 x = 0; x < RT_W; x++) {
			u32 p = img[y * (rt_pitch / 4u) + x];
			int sat = 0;
			unsigned ch[4] = { (p >> 16) & 0xffu, (p >> 8) & 0xffu, p & 0xffu, (p >> 24) & 0xffu };
			for (int c = 0; c < 4; c++) {
				seen[c][ch[c]] = 1;
				if (ch[c] == 0u || ch[c] == 255u)
					sat = 1;
			}
			out->saturated += sat;
		}
	for (int c = 0; c < 4; c++) {
		int n = 0;
		for (int v = 0; v < 256; v++)
			n += seen[c][v];
		out->levels[c] = n;
	}
}

/* Fill the readback buffer before a transfer; parity alternates the
 * fill so a transfer that never lands cannot repeat a frame. */
static void fill_readback(u32 rt_pitch, int parity)
{
	u32 fill = (parity & 1) ? CLEAR_SENTINEL_ODD : CLEAR_SENTINEL_EVEN;
	for (u32 i = 0; i < (rt_pitch / 4u) * RT_H; i++)
		g_readback[i] = fill;
}

/* Pixels a draw actually painted: anything but the GPU clear mark.  The
 * readback fill is deliberately not excluded (see CLEAR_SENTINEL_EVEN). */
static int painted_pixels(const u32 *img, u32 rt_pitch)
{
	int n = 0;
	for (u32 y = 0; y < RT_H; y++)
		for (u32 x = 0; x < RT_W; x++) {
			u32 p = img[y * (rt_pitch / 4u) + x];
			if (p != GPU_CLEAR_MARK)
				n++;
		}
	return n;
}

/* Poison detector.  A malformed fragment program can leave the RSX (or
 * the emulator's model of it) in a state where every later draw paints
 * nothing - measured: one general-path corpus container blanked the 61
 * rows after it, byte-copied constant pairs included, and they all read
 * "vacuous" (or, before the vacuity guard, "identical").  After every
 * gated row the rig draws the identity control's own container once;
 * if that paints nothing the state is poisoned, the run stops and
 * names the row that poisoned it, because no verdict after that point
 * would mean anything.  Cost: one small draw per row. */
static int render_side(CellGcmContextData *ctx, void *container,
                       const char *uniform_set,
                       int textures_ok, int have_tex_control,
                       u32 rt_off, u32 rt_depth_off, u32 rt_pitch,
                       u32 *save, int *warmup_draws);

static int canary_paints(CellGcmContextData *ctx, void *canary_container,
                         u32 rt_off, u32 rt_depth_off, u32 rt_pitch, u32 *save)
{
	u32 watermark = g_local_mem_heap;
	int warm = 0;
	int rc = render_side(ctx, canary_container, "0", 1, 1,
	                     rt_off, rt_depth_off, rt_pitch, save, &warm);
	g_local_mem_heap = watermark;
	if (rc != 0)
		return 0;
	return painted_pixels(save, rt_pitch) > 0;
}

/* ---- one side of a pair: bind, draw (with warm-up), read back ---- */

/* Returns 0 on success; -1 if a uniform in the pair's set has no
 * matching named parameter in this side's container; -2 if the
 * container declares a sampler the auto-binder cannot serve; -3 if it
 * declares samplers while the texture control is red (textures_ok ==
 * 0); -4 if it declares samplers and the manifest carries no texture
 * control at all (have_tex_control == 0); -5 if the set is `auto` and
 * the container declares a uniform kind the binder does not
 * synthesise; -6 if the side painted but no two consecutive readbacks
 * agreed within the warm-up budget (unstable).  The caller reports which side.  Sampler checks come
 * BEFORE any draw so a withheld verdict costs nothing. */
static int render_side(CellGcmContextData *ctx, void *container,
                       const char *uniform_set,
                       int textures_ok, int have_tex_control,
                       u32 rt_off, u32 rt_depth_off, u32 rt_pitch,
                       u32 *save, int *warmup_draws)
{
	CGprogram fpo = (CGprogram)container;
	cellGcmCgInitProgram(fpo);

	int nsamplers = bind_container_samplers(ctx, fpo);
	if (nsamplers < 0)
		return -2;
	if (nsamplers > 0 && !have_tex_control)
		return -4;
	if (nsamplers > 0 && !textures_ok)
		return -3;

	void *fp_blob_ucode; u32 fpsize = 0;
	cellGcmCgGetUCode(fpo, &fp_blob_ucode, &fpsize);
	void *fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_blob_ucode, fpsize);
	u32 fp_offset = 0;
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	/* Apply the pair's uniform set BEFORE the draw: the patch rides
	 * the FIFO's inline-transfer path against the copied ucode, so
	 * ordering relative to the draw commands is preserved. */
	if (strcmp(uniform_set, "auto") == 0) {
		if (apply_auto_uniforms(ctx, fpo, fp_offset) < 0)
			return -5;
	} else if (!uniformSetIsNone(uniform_set)) {
		for (int i = 0; i < g_nuniforms; i++) {
			if (strcmp(g_uniforms[i].set, uniform_set) != 0)
				continue;
			CGparameter prm =
				cellGcmCgGetNamedParameter(fpo, g_uniforms[i].name);
			if (!prm)
				return -1;
			cellGcmSetFragmentProgramParameter(ctx, fpo, prm,
			                                   g_uniforms[i].values,
			                                   fp_offset);
		}
	}

	/* Sentinel-fill the readback buffer so a transfer that never lands
	 * is attributable, then GPU-clear the RT to the mark the warm-up
	 * loop keys on. */
	fill_readback(rt_pitch, 0);

	set_rt_surface(ctx, rt_off, rt_depth_off, rt_pitch, RT_W, RT_H);
	set_draw_env(ctx, RT_W, RT_H);

	cellGcmSetClearColor(ctx, GPU_CLEAR_MARK);
	cellGcmSetClearSurface(ctx,
		GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	wait_rsx_idle(ctx);

	cellGcmSetVertexProgram(ctx, vpo, vp_ucode);
	cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, texcoord_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, uv));
	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);

	/* Warm-up retries against RPCS3's async shader compiler (see the
	 * readback rig).  A draw counts as landed only when two CONSECUTIVE
	 * readbacks are byte-identical AND painted: the interim program the
	 * emulator hands a never-seen shader on its first draw can paint
	 * nothing (the common case) or ALMOST everything (measured: 4042
	 * and 3968 of 4096 on two general-path containers, accepted by the
	 * previous any-pixel rule and reported as a compiler defect for an
	 * hour).  A blank interim never qualifies; a real frame costs one
	 * confirming draw.  A shader that legitimately paints nothing burns
	 * the budget and is judged from the final image anyway - slow,
	 * never wrong, and both sides of a pair face the identical policy. */
	int tries;
	int have_prev = 0;
	int stable = 0;
	int ever_painted = 0;   /* any draw painted, not just the last one */
	for (tries = 1; tries <= 10; tries++) {
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
		wait_rsx_idle(ctx);
		/* Refilled per readback, parity from the try count: before
		 * this the fill happened once per side, so a transfer that
		 * never landed on try N showed try N-1's frame, identical by
		 * construction, and passed the two-frames rule. */
		fill_readback(rt_pitch, tries);
		transfer_rt_to_main(ctx, rt_off, rt_pitch);
		int painted = painted_pixels(g_readback, rt_pitch);
		ever_painted |= painted > 0;
		if (painted > 0 && have_prev &&
		    memcmp(save, g_readback, (size_t)rt_pitch * RT_H) == 0) {
			stable = 1;
			break;
		}
		memcpy(save, g_readback, (size_t)rt_pitch * RT_H);
		have_prev = painted > 0;
		if (!have_prev)
			usleep(200000);
	}
	*warmup_draws = tries;
	/* Painted frames that never agreed twice in a row: the shader's
	 * output varies between draws, which is the signature of reading
	 * uninitialised state - a lane nobody wrote, a register holding two
	 * values.  Reported as its own status (unstable-a/b), never folded
	 * into vacuous or into a plain verdict: it is the most specific
	 * thing the rig can say about a shader (review question on the
	 * two-readback warm-up).  A shader that paints nothing burns the
	 * budget without ever being "painted" and stays on the vacuous path.
	 * Keyed on whether the side EVER painted, not on its last frame: a
	 * side that paints, never repeats, and ends the budget on a blank
	 * frame is unstable, not vacuous (review finding). */
	if (!stable && ever_painted)
		return -6;

	memcpy(save, g_readback, (size_t)rt_pitch * RT_H);
	return 0;
}

/* ---- artifacts ---- */

static void dump_artifact(const char *name, const char side, const u32 *img,
                          u32 bytes)
{
	char path[sizeof(SD_ROOT) + 96];
	snprintf(path, sizeof(path), SD_ROOT "artifacts/%s_%c.raw", name, side);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;  /* artifact loss is reported via the row's artifact col */
	fwrite(img, 1, bytes, f);
	fclose(f);
}

/* ---- manifest ---- */

static sd_pair g_pairs[MAX_PAIRS];
static int     g_npairs;

/* `tier|role|name|a_path|b_path|uniform_set`, '#' comments, and an
 * optional `@target <word>` directive.  Refuses malformed lines by
 * line number: a silently skipped row is a shader silently unjudged. */
static int load_manifest(void)
{
	FILE *f = fopen(SD_ROOT "manifest.txt", "r");
	if (!f) {
		printf("shader-differential: cannot open " SD_ROOT "manifest.txt\n");
		return 0;
	}
	char line[640];
	int lineno = 0;
	while (fgets(line, sizeof(line), f)) {
		lineno++;
		size_t len = strlen(line);
		while (len && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = 0;
		if (!len || line[0] == '#')
			continue;
		if (line[0] == '@') {
			if (sscanf(line, "@target %15s", g_target) == 1)
				continue;
			printf("shader-differential: manifest line %d: unknown directive '%s'\n",
			       lineno, line);
			fclose(f);
			return 0;
		}
		if (g_npairs >= MAX_PAIRS) {
			printf("shader-differential: manifest exceeds %d pairs\n", MAX_PAIRS);
			fclose(f);
			return 0;
		}
		sd_pair *p = &g_pairs[g_npairs];
		char *fields[6];
		int nf = 0;
		char *cur = line;
		fields[nf++] = cur;
		for (char *c = line; *c && nf < 6; c++) {
			if (*c == '|') {
				*c = 0;
				fields[nf++] = c + 1;
			}
		}
		if (nf != 6) {
			printf("shader-differential: manifest line %d: expected 6 |-fields, got %d\n",
			       lineno, nf);
			fclose(f);
			return 0;
		}
		snprintf(p->tier,        sizeof(p->tier),        "%s", fields[0]);
		snprintf(p->role,        sizeof(p->role),        "%s", fields[1]);
		snprintf(p->name,        sizeof(p->name),        "%s", fields[2]);
		snprintf(p->a_path,      sizeof(p->a_path),      "%s", fields[3]);
		snprintf(p->b_path,      sizeof(p->b_path),      "%s", fields[4]);
		snprintf(p->uniform_set, sizeof(p->uniform_set), "%s", fields[5]);
		g_npairs++;
	}
	fclose(f);
	if (g_npairs < 2) {
		printf("shader-differential: manifest has %d rows; the two standing controls are mandatory\n",
		       g_npairs);
		return 0;
	}
	if (strcmp(g_pairs[0].role, "control-identical") != 0 ||
	    strcmp(g_pairs[1].role, "control-mismatch") != 0) {
		printf("shader-differential: manifest rows 1..2 must be control-identical then control-mismatch (got '%s', '%s')\n",
		       g_pairs[0].role, g_pairs[1].role);
		return 0;
	}
	/* The uniform gate is MANDATORY whenever uniforms are in play,
	 * same move as the rows-1..2 check above: a manifest that uses
	 * uniform sets without the control-uniform row would judge every
	 * uniform-dependent pair against unapplied embedded defaults and
	 * report plausible verdicts — the exact silent-non-application
	 * failure the control exists to catch, with nothing to catch it
	 * (review finding on the increment-2b commit).  This also covers
	 * the absent-uniforms.txt case: rows referencing sets are what
	 * make the file load-bearing, not the file's own presence. */
	{
		int first_uniform_row = -1, control_index = -1;
		for (int i = 0; i < g_npairs; i++) {
			if (strcmp(g_pairs[i].role, "control-uniform") == 0) {
				if (control_index < 0)
					control_index = i;
			} else if (!uniformSetIsNone(g_pairs[i].uniform_set) &&
			           first_uniform_row < 0) {
				first_uniform_row = i;
			}
		}
		if (first_uniform_row >= 0 && control_index < 0) {
			printf("shader-differential: manifest declares uniform sets but no control-uniform row — uniform-dependent verdicts would be unvalidated; refusing\n");
			return 0;
		}
		/* Existence is not enough: the judging loop clears uniforms_ok
		 * only when the control row is REACHED, so a dependent row at
		 * a lower index would be judged against unapplied defaults
		 * with the gate still open.  The standing controls are immune
		 * to this by their POSITIONAL pin on rows 1..2; this is the
		 * same conversion of convention into property, one row later
		 * (review follow-on to the existence check). */
		if (first_uniform_row >= 0 && control_index > first_uniform_row) {
			/* Note the implication for rows 1..2: the standing
			 * controls are pinned before any possible control-uniform
			 * position, so a standing control carrying a uniform set
			 * always trips this check.  That is a RULE, not an
			 * ordering accident — the comparator's own validation
			 * cannot depend on uniform application — and the message
			 * says so, so nobody tries to reorder their way out of it. */
			printf("shader-differential: manifest line order puts a uniform-dependent row (index %d) before the control-uniform row (index %d) — it would be judged before the gate can close; refusing (note: the standing controls on rows 1..2 may never carry a uniform set — the comparator's own validation cannot depend on uniform application)\n",
			       first_uniform_row, control_index);
			return 0;
		}
	}
	/* Auto gate: same two properties as the uniform gate, keyed on the
	 * literal set name `auto`.  Existence CAN be required here (the
	 * set is manifest data), and the control must precede the first
	 * auto row.  An auto row is also a uniform-dependent row, so the
	 * uniform gate above already covers its relation to control-uniform. */
	{
		int first_auto_row = -1, auto_index = -1;
		for (int i = 0; i < g_npairs; i++) {
			if (strcmp(g_pairs[i].role, "control-auto") == 0) {
				if (auto_index < 0)
					auto_index = i;
			} else if (strcmp(g_pairs[i].uniform_set, "auto") == 0 &&
			           first_auto_row < 0) {
				first_auto_row = i;
			}
		}
		if (first_auto_row >= 0 && auto_index < 0) {
			printf("shader-differential: manifest uses uniform_set=auto but has no control-auto row - synthesised values would be unvalidated against the host's; refusing\n");
			return 0;
		}
		if (first_auto_row >= 0 && auto_index > first_auto_row) {
			printf("shader-differential: manifest line order puts an auto row (index %d) before the control-auto row (index %d) - it would be judged before the gate can close; refusing\n",
			       first_auto_row, auto_index);
			return 0;
		}
	}
	/* Texture gate ordering.  Sampler use is a property of the
	 * CONTAINERS, not the manifest, so existence cannot be required at
	 * load (a sampler-declaring pair with no control is refused at
	 * judge time, status samplers-unvalidated).  Ordering CAN be: if a
	 * control-texture row exists it must precede every row that is not
	 * a control, or a sampler-declaring row would be judged before the
	 * gate can close -- the same property the uniform gate enforces. */
	{
		int tex_index = -1, first_open_row = -1;
		for (int i = 0; i < g_npairs; i++) {
			if (strcmp(g_pairs[i].role, "control-texture") == 0) {
				if (tex_index < 0)
					tex_index = i;
			} else if (strncmp(g_pairs[i].role, "control-", 8) != 0 &&
			           first_open_row < 0) {
				first_open_row = i;
			}
		}
		if (tex_index >= 0 && first_open_row >= 0 && tex_index > first_open_row) {
			printf("shader-differential: manifest line order puts a non-control row (index %d) before the control-texture row (index %d) - a sampler-declaring pair there would be judged before the gate can close; refusing\n",
			       first_open_row, tex_index);
			return 0;
		}
	}
	return 1;
}

/* ---- pair judgment ---- */

/* Path-pair premise coupling: the row before <shader>@paths must be an
 * identical reference row whose side A is THE SAME CONTAINER as the
 * path-pair row's side A (compared by a_path, not by name: the default
 * container is the actual oracle premise, and a name stem can collide).
 * The stager always emits them that way, but that is a property of the
 * stager, and the gate lives here: a hand-edited manifest that put
 * shader X's oracle before shader Y's pair would otherwise judge Y on
 * X's premise with a confident verdict (review findings on the
 * path-pair role, t_5d8795e7 family). */

typedef struct {
	const char *status;      /* identical | mismatch | load-failed-a/b */
	int  max_delta;
	int  diff_pixels;
	int  total_pixels;
	long elapsed_ms;
	char diagnostic[192];
	char artifact[96];
} sd_result;

static long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long)(tv.tv_sec * 1000L + tv.tv_usec / 1000L);
}

static void judge_pair(CellGcmContextData *ctx, const sd_pair *p,
                       int textures_ok, int have_tex_control,
                       u32 rt_a_off, u32 rt_b_off, u32 rt_depth_off,
                       u32 rt_pitch, u32 *save_a, u32 *save_b,
                       sd_result *r)
{
	long t0 = now_ms();
	r->status = "identical";
	r->max_delta = 0;
	r->diff_pixels = 0;
	r->total_pixels = RT_W * RT_H;
	r->diagnostic[0] = 0;
	snprintf(r->diagnostic, sizeof(r->diagnostic), "-");
	snprintf(r->artifact, sizeof(r->artifact), "-");

	u32 sz_a = 0, sz_b = 0;
	void *cont_a = load_container(p->a_path, &sz_a);
	if (!cont_a) {
		r->status = "load-failed-a";
		snprintf(r->diagnostic, sizeof(r->diagnostic), "cannot read %s", p->a_path);
		r->elapsed_ms = now_ms() - t0;
		return;
	}
	void *cont_b = load_container(p->b_path, &sz_b);
	if (!cont_b) {
		free(cont_a);
		r->status = "load-failed-b";
		snprintf(r->diagnostic, sizeof(r->diagnostic), "cannot read %s", p->b_path);
		r->elapsed_ms = now_ms() - t0;
		return;
	}

	/* Local-memory watermark: each side bump-allocates ucode; restore
	 * after judging so a long corpus run cannot exhaust local memory.
	 * Safe because both draws are idle-waited before we return. */
	u32 watermark = g_local_mem_heap;

	/* The uniform control is asymmetric BY DESIGN: side A outputs the
	 * uniform, side B has the value baked and declares no parameter,
	 * so its set applies to side A only.  Every other role applies
	 * the set to both sides. */
	const char *set_b = (strcmp(p->role, "control-uniform") == 0 ||
	                     strcmp(p->role, "control-auto") == 0)
		? "0" : p->uniform_set;

	/* The texture control is the one row that may declare samplers
	 * while the gate is closed: it is the row that opens it. */
	int tex_ok_here = textures_ok ||
	                  strcmp(p->role, "control-texture") == 0;

	int warm_a = 0, warm_b = 0;
	int ua = render_side(ctx, cont_a, p->uniform_set, tex_ok_here,
	                     have_tex_control, rt_a_off,
	                     rt_depth_off, rt_pitch, save_a, &warm_a);
	int ub = ua == 0
		? render_side(ctx, cont_b, set_b, tex_ok_here,
		              have_tex_control, rt_b_off,
		              rt_depth_off, rt_pitch, save_b, &warm_b)
		: 0;

	g_local_mem_heap = watermark;
	free(cont_a);
	free(cont_b);

	if (ua != 0 || ub != 0) {
		int rc = ua != 0 ? ua : ub;
		char side = ua != 0 ? 'a' : 'b';
		switch (rc) {
		case -1:
			r->status = side == 'a' ? "uniform-missing-a" : "uniform-missing-b";
			snprintf(r->diagnostic, sizeof(r->diagnostic),
			         "set '%s' names a parameter the container lacks",
			         p->uniform_set);
			break;
		case -2:
			r->status = side == 'a' ? "sampler-unsupported-a" : "sampler-unsupported-b";
			snprintf(r->diagnostic, sizeof(r->diagnostic),
			         "container declares a sampler kind or unit the binder does not serve");
			break;
		case -3:
			r->status = "textures-invalid";
			snprintf(r->diagnostic, sizeof(r->diagnostic),
			         "skipped: control-texture failed");
			break;
		case -4:
			r->status = "samplers-unvalidated";
			snprintf(r->diagnostic, sizeof(r->diagnostic),
			         "container declares samplers but the manifest has no control-texture row");
			break;
		case -6:
			r->status = side == 'a' ? "unstable-a" : "unstable-b";
			snprintf(r->diagnostic, sizeof(r->diagnostic),
			         "10 draws, no two consecutive readbacks agreed: output varies between draws");
			break;
		case -5:
			r->status = side == 'a' ? "uniform-unsupported-a" : "uniform-unsupported-b";
			snprintf(r->diagnostic, sizeof(r->diagnostic),
			         "container declares a uniform kind the auto-binder does not synthesise");
			break;
		default:
			/* A code this switch does not know must not borrow a
			 * confident status it does not deserve (review finding on
			 * the 3a commit, the t_5d8795e7 family). */
			r->status = "internal-error";
			snprintf(r->diagnostic, sizeof(r->diagnostic),
			         "render_side returned unmapped code %d on side %c", rc, side);
			break;
		}
		r->elapsed_ms = now_ms() - t0;
		return;
	}

	/* Vacuity guard, the differential harness's dumbest failure at row
	 * level: two sides that both painted NOTHING agree byte-for-byte
	 * and would read identical.  Such a pair carries no information
	 * about the compiler and is reported as vacuous, never identical.
	 * (First real corpus sweep: 62 of 63 pairs would have passed this
	 * way while RPCS3 was rejecting programs for invalid registers.) */
	int painted_a = painted_pixels(save_a, rt_pitch);
	int painted_b = painted_pixels(save_b, rt_pitch);
	if (painted_a == 0 && painted_b == 0) {
		r->status = "vacuous";
		snprintf(r->diagnostic, sizeof(r->diagnostic),
		         "no pixel painted on either side");
		r->elapsed_ms = now_ms() - t0;
		return;
	}

	/* Raw-byte channel compare.  A8R8G8B8 both sides, same pitch, so a
	 * per-u32 walk with per-channel deltas gives max_delta in 8-bit
	 * steps and diff_pixels as "any channel differs". */
	for (int i = 0; i < RT_W * RT_H; i++) {
		u32 a = save_a[i], b = save_b[i];
		if (a == b)
			continue;
		r->diff_pixels++;
		for (int sh = 0; sh < 32; sh += 8) {
			int da = (int)((a >> sh) & 0xff) - (int)((b >> sh) & 0xff);
			if (da < 0) da = -da;
			if (da > r->max_delta) r->max_delta = da;
		}
	}
	if (r->diff_pixels > 0) {
		r->status = "mismatch";
		dump_artifact(p->name, 'a', save_a, rt_pitch * RT_H);
		dump_artifact(p->name, 'b', save_b, rt_pitch * RT_H);
		snprintf(r->artifact, sizeof(r->artifact), "artifacts/%s_{a,b}.raw", p->name);
	}
	if (painted_a < RT_W * RT_H || painted_b < RT_W * RT_H) {
		size_t len = strlen(r->diagnostic);
		if (strcmp(r->diagnostic, "-") == 0) len = 0;
		snprintf(r->diagnostic + len, sizeof(r->diagnostic) - len,
		         "%spainted a=%d b=%d", len ? " " : "", painted_a, painted_b);
	}
	{
		sd_sensitivity sa, sb;
		measure_sensitivity(save_a, rt_pitch, &sa);
		measure_sensitivity(save_b, rt_pitch, &sb);
		size_t len = strlen(r->diagnostic);
		if (strcmp(r->diagnostic, "-") == 0) len = 0;
		snprintf(r->diagnostic + len, sizeof(r->diagnostic) - len,
		         "%slevels a=%d/%d/%d/%d b=%d/%d/%d/%d sat a=%d%% b=%d%%",
		         len ? " " : "",
		         sa.levels[0], sa.levels[1], sa.levels[2], sa.levels[3],
		         sb.levels[0], sb.levels[1], sb.levels[2], sb.levels[3],
		         (sa.saturated * 100) / (RT_W * RT_H),
		         (sb.saturated * 100) / (RT_W * RT_H));
	}
	if (warm_a > 1 || warm_b > 1) {
		size_t len = strlen(r->diagnostic);
		if (strcmp(r->diagnostic, "-") == 0) len = 0;
		snprintf(r->diagnostic + len, sizeof(r->diagnostic) - len,
		         "%swarmup a=%d b=%d", len ? " " : "", warm_a, warm_b);
	}
	{
		/* Cost of each side, so a green can be priced: container
		 * bytes, fragment instructions (the ucode blob is 16 bytes
		 * per instruction on NV40), and container parameters (a
		 * promoted file-scope const shows up here as +1, t_3bf3ce95).
		 * Both containers were initialised by render_side above. */
		void *ua = NULL, *ub = NULL;
		u32 usz_a = 0, usz_b = 0;
		cellGcmCgGetUCode((CGprogram)cont_a, &ua, &usz_a);
		cellGcmCgGetUCode((CGprogram)cont_b, &ub, &usz_b);
		size_t len = strlen(r->diagnostic);
		if (strcmp(r->diagnostic, "-") == 0) len = 0;
		snprintf(r->diagnostic + len, sizeof(r->diagnostic) - len,
		         "%ssize a=%u b=%u insn a=%u b=%u params a=%u b=%u",
		         len ? " " : "", (unsigned)sz_a, (unsigned)sz_b,
		         (unsigned)(usz_a / 16), (unsigned)(usz_b / 16),
		         (unsigned)cellGcmCgGetCountParameter((CGprogram)cont_a),
		         (unsigned)cellGcmCgGetCountParameter((CGprogram)cont_b));
	}
	r->elapsed_ms = now_ms() - t0;
}

static void print_row(const sd_pair *p, const sd_result *r)
{
	printf("SDIFF|tier=%s|role=%s|shader=%s|compiler=ab|uniform_set=%s|target=%s|status=%s|max_delta=%d|diff_pixels=%d|total_pixels=%d|diagnostic=%s|elapsed_ms=%ld|artifact=%s\n",
	       p->tier, p->role, p->name, p->uniform_set, g_target,
	       r->status, r->max_delta, r->diff_pixels, r->total_pixels,
	       r->diagnostic, r->elapsed_ms, r->artifact);
}

int main(int argc, const char **argv)
{
	(void)argc; (void)argv;
	printf("shader-differential: A/B pixel-differential rig, increment 1\n");

	if (!load_manifest()) {
		printf("SHADER_DIFF_INVALID\n");
		return 2;
	}
	if (!load_uniforms()) {
		printf("SHADER_DIFF_INVALID\n");
		return 2;
	}
	printf("shader-differential: %d pairs, %d uniform entries, target=%s\n",
	       g_npairs, g_nuniforms, g_target);

	void *host_addr = memalign(1024 * 1024, HOST_SIZE);
	if (!init_screen(host_addr, HOST_SIZE)) {
		printf("shader-differential: init failed\nSHADER_DIFF_INVALID\n");
		free(host_addr);
		return 2;
	}
	CellGcmContextData *ctx = (CellGcmContextData *)gCellGcmCurrentContext;

	display_buffer buffers[MAX_BUFFERS];
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!make_buffer(&buffers[i], i)) {
			printf("shader-differential: buffer init failed\nSHADER_DIFF_INVALID\n");
			cellGcmFinish(ctx, 0);
			free(host_addr);
			return 2;
		}
	}
	if (cellGcmSetFlip(ctx, (uint8_t)(MAX_BUFFERS - 1)) == 0) {
		cellGcmFlush(ctx);
		cellGcmSetWaitFlip(ctx);
	}

	/* Bind the backbuffer once and idle before any RT switch (proven
	 * boot sequence — see the readback rig). */
	set_rt_surface(ctx, buffers[0].offset, disp_depth_offset,
	               display_pitch, (u16)display_w, (u16)display_h);
	wait_rsx_idle(ctx);

	/* ---- shared VP + geometry ---- */
	vpo = (CGprogram)sd_pos_allch_vpo;
	cellGcmCgInitProgram(vpo);
	u32 vpsize = 0;
	cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);

	CGparameter pos = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter tc  = cellGcmCgGetNamedParameter(vpo, "in_texcoord");
	position_index  = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	texcoord_index  = tc  ? (int)cellGcmCgGetParameterResource(vpo, tc)  - CG_ATTR0 : 8;

	vertex_buffer = (vertex_t *)local_align(128, 4 * sizeof(vertex_t));
	vertex_buffer[0] = (vertex_t){{-1.0f,  1.0f}, {0.0f, 0.0f}};
	vertex_buffer[1] = (vertex_t){{ 1.0f,  1.0f}, {1.0f, 0.0f}};
	vertex_buffer[2] = (vertex_t){{-1.0f, -1.0f}, {0.0f, 1.0f}};
	vertex_buffer[3] = (vertex_t){{ 1.0f, -1.0f}, {1.0f, 1.0f}};
	cellGcmAddressToOffset(vertex_buffer, &vertex_buffer_offset);

	cellGcmCgUploadInternalConsts(ctx, vpo);

	if (!init_procedural_texture()) {
		printf("shader-differential: texture alloc failed\nSHADER_DIFF_INVALID\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 2;
	}

	/* ---- readback buffer at the host region's tail ---- */
	g_readback = (u32 *)((char *)host_addr + HOST_SIZE - (64 * 1024));
	if (cellGcmAddressToOffset(g_readback, &g_readback_off) != 0) {
		printf("shader-differential: readback offset failed\nSHADER_DIFF_INVALID\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 2;
	}

	/* ---- two RTs (A and B), one shared depth (test disabled) ---- */
	u32 rt_pitch = RT_W * 4;
	u32 rt_sz    = rt_pitch * RT_H;
	u32 *rt_a = (u32 *)local_align(64, rt_sz);
	u32 *rt_b = (u32 *)local_align(64, rt_sz);
	void *rt_depth = local_align(64, rt_sz);
	u32 rt_a_off = 0, rt_b_off = 0, rt_depth_off = 0;
	if (cellGcmAddressToOffset(rt_a, &rt_a_off) != 0 ||
	    cellGcmAddressToOffset(rt_b, &rt_b_off) != 0 ||
	    cellGcmAddressToOffset(rt_depth, &rt_depth_off) != 0) {
		printf("shader-differential: RT alloc failed\nSHADER_DIFF_INVALID\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 2;
	}

	u32 *save_a = (u32 *)malloc(rt_sz);
	u32 *save_b = (u32 *)malloc(rt_sz);
	if (!save_a || !save_b) {
		printf("shader-differential: save alloc failed\nSHADER_DIFF_INVALID\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 2;
	}

	/* ---- controls first, judged in manifest order ---- */
	int controls_ok = 1;
	int uniforms_ok = 1;   /* flipped by a failed control-uniform row */
	int uniforms_skipped = 0;
	int textures_ok = 1;   /* flipped by a failed control-texture row */
	int textures_skipped = 0;
	int autos_ok = 1;      /* flipped by a failed control-auto row */
	int unsupported = 0;   /* gated rows the binder could not serve */
	int vacuous = 0;       /* gated rows where neither side painted */
	int unstable = 0;      /* gated rows where a side never repeated a frame */
	int unoracled = 0;     /* path-pair rows whose premise row was not identical */
	const char *prev_role = "";       /* the row before this one, for path-pair */
	const char *prev_status = "";
	const char *prev_name = "";
	const char *prev_a_path = "";
	u32 canary_sz = 0;
	void *canary = load_container(g_pairs[0].a_path, &canary_sz);
	if (!canary) {
		printf("shader-differential: cannot load the identity control's container for the poison canary\nSHADER_DIFF_INVALID\n");
		cellGcmFinish(ctx, 0);
		free(host_addr);
		return 2;
	}
	int have_tex_control = 0;
	for (int i = 0; i < g_npairs; i++)
		if (strcmp(g_pairs[i].role, "control-texture") == 0)
			have_tex_control = 1;
	int failures = 0;
	for (int i = 0; i < g_npairs; i++) {
		const sd_pair *p = &g_pairs[i];
		sd_result r;

		/* A failed uniform control invalidates uniform-DEPENDENT rows
		 * only: judging them would compare unapplied embedded defaults
		 * and report plausible verdicts about values nobody set. */
		if (!uniforms_ok && i > 1 && !uniformSetIsNone(p->uniform_set) &&
		    strcmp(p->role, "control-uniform") != 0) {
			r.status = "uniforms-invalid";
			r.max_delta = 0;
			r.diff_pixels = 0;
			r.total_pixels = 0;
			r.elapsed_ms = 0;
			snprintf(r.diagnostic, sizeof(r.diagnostic),
			         "skipped: control-uniform failed");
			snprintf(r.artifact, sizeof(r.artifact), "-");
			print_row(p, &r);
			uniforms_skipped++;
			prev_role = p->role;
			prev_status = r.status;
			prev_name = p->name;
			prev_a_path = p->a_path;
			continue;
		}

		/* A path-pair row is an oracle comparison only while its
		 * premise holds: the reference row immediately before it (the
		 * same shader's default-path container against the reference)
		 * must have judged identical.  "Default equals general" is
		 * satisfiable by both being wrong the same way, so the row
		 * checks the premise in the run rather than assuming it, and a
		 * failed premise withholds the verdict as unoracled - the
		 * premise row itself already failed the run as a reference
		 * row (review condition on the path-pair role). */
		if (strcmp(p->role, "path-pair") == 0 &&
		    !(strcmp(prev_role, "reference") == 0 &&
		      strcmp(prev_status, "identical") == 0 &&
		      strcmp(prev_a_path, p->a_path) == 0)) {
			r.status = "path-pair-unoracled";
			r.max_delta = 0;
			r.diff_pixels = 0;
			r.total_pixels = 0;
			r.elapsed_ms = 0;
			snprintf(r.diagnostic, sizeof(r.diagnostic),
			         "premise row before it (%s %s, %s) is not an identical reference row for this default container",
			         prev_name[0] ? prev_name : "none",
			         prev_role[0] ? prev_role : "none",
			         prev_status[0] ? prev_status : "none");
			snprintf(r.artifact, sizeof(r.artifact), "-");
			print_row(p, &r);
			unoracled++;
			prev_role = p->role;
			prev_status = r.status;
			prev_name = p->name;
			prev_a_path = p->a_path;
			continue;
		}

		/* A failed auto control invalidates auto rows only, counted
		 * with the uniform skips: they are uniform-dependent rows. */
		if (!autos_ok && strcmp(p->uniform_set, "auto") == 0 &&
		    strcmp(p->role, "control-auto") != 0) {
			r.status = "auto-invalid";
			r.max_delta = 0;
			r.diff_pixels = 0;
			r.total_pixels = 0;
			r.elapsed_ms = 0;
			snprintf(r.diagnostic, sizeof(r.diagnostic),
			         "skipped: control-auto failed");
			snprintf(r.artifact, sizeof(r.artifact), "-");
			print_row(p, &r);
			uniforms_skipped++;
			prev_role = p->role;
			prev_status = r.status;
			prev_name = p->name;
			prev_a_path = p->a_path;
			continue;
		}

		judge_pair(ctx, p, textures_ok, have_tex_control,
		           rt_a_off, rt_b_off, rt_depth_off, rt_pitch,
		           save_a, save_b, &r);
		print_row(p, &r);
		prev_role = p->role;
		prev_status = r.status;
		prev_name = p->name;
		prev_a_path = p->a_path;

		if (strcmp(r.status, "textures-invalid") == 0) {
			/* Withheld, not failed: the red texture control already
			 * counted, the same accounting as the uniform skip. */
			textures_skipped++;
			continue;
		}

		if (strcmp(p->role, "control-texture") == 0) {
			/* Sampled-vs-arithmetic twin: identical iff the procedural
			 * texture is bound where the container's sampler says and
			 * sampled nearest, texel-aligned.  Gates sampler-declaring
			 * rows only; its failure counts even with none present. */
			if (strcmp(r.status, "identical") != 0) {
				printf("shader-differential: control-texture judged '%s' - texture binding is not working; sampler-declaring pairs will not be judged\n",
				       r.status);
				textures_ok = 0;
				failures++;
			}
			goto post_row;
		}

		if (strcmp(p->role, "control-auto") == 0) {
			/* Synthesised-vs-baked twin: identical iff the guest's
			 * auto_value and the stager's Auto-Value agree AND the
			 * patch path applies them.  Gates auto rows only; its
			 * failure counts even with none present. */
			if (strcmp(r.status, "identical") != 0) {
				printf("shader-differential: control-auto judged '%s' - synthesised uniforms disagree with the host's; auto rows will not be judged\n",
				       r.status);
				autos_ok = 0;
				failures++;
			}
			goto post_row;
		}

		if (strcmp(p->role, "control-uniform") == 0) {
			/* Uniform-vs-baked twin: identical iff application works.
			 * Not a probe (it gates) and not a standing control (it
			 * invalidates only uniform-dependent rows).  Its failure
			 * COUNTS AS A FAILURE even when no dependent rows exist:
			 * a run with a red control must not read green. */
			if (strcmp(r.status, "identical") != 0) {
				printf("shader-differential: control-uniform judged '%s' — uniform application is not working; uniform-dependent pairs will not be judged\n",
				       r.status);
				uniforms_ok = 0;
				failures++;
			}
			goto post_row;
		}

		if (i == 0) {
			/* control-identical must judge identical. */
			if (strcmp(r.status, "identical") != 0) {
				printf("shader-differential: control-identical judged '%s' — a byte-copied pair must be identical; the rig is not valid\n",
				       r.status);
				controls_ok = 0;
			}
		} else if (i == 1) {
			/* control-mismatch must judge mismatch: the comparator
			 * must SEE the deliberate difference before any real
			 * pair is trusted. */
			if (strcmp(r.status, "mismatch") != 0) {
				printf("shader-differential: control-mismatch judged '%s' — the comparator did not see a deliberate difference; the rig is not valid\n",
				       r.status);
				controls_ok = 0;
			}
			if (!controls_ok) {
				printf("shader-differential: %d remaining pairs NOT judged\nSHADER_DIFF_INVALID\n",
				       g_npairs - 2);
				cellGcmSetWaitFlip(ctx);
				cellGcmFinish(ctx, 1);
				free(host_addr);
				return 2;
			}
		} else if (strcmp(p->role, "probe") == 0) {
			/* Probe rows measure; they never gate. */
		} else if (strncmp(r.status, "unstable", 8) == 0) {
			/* Output varied between draws on one side: a statement
			 * about the shader (uninitialised state), counted on its
			 * own line so it is never mistaken for an emulator stall
			 * or a plain mismatch. */
			unstable++;
		} else if (strcmp(r.status, "vacuous") == 0) {
			/* Neither side painted: no verdict about the compiler was
			 * reached.  Counted on its own line, loudly, and never as
			 * identical.  A control that comes out vacuous has already
			 * failed its own check above (it is not identical). */
			vacuous++;
		} else if (strncmp(r.status, "sampler-unsupported", 19) == 0 ||
		           strncmp(r.status, "uniform-unsupported", 19) == 0) {
			/* A pair the RIG cannot bind is a rig limit, not a
			 * compiler finding: counted on its own line so a corpus
			 * sweep reports "not judged" rather than "failed" for it.
			 * Every other non-identical status on a gated row - a
			 * mismatch, a load failure, an unvalidated sampler - still
			 * fails the run. */
			unsupported++;
		} else {
			if (strcmp(r.status, "identical") != 0)
				failures++;
		}

	post_row:
		/* Poison check after EVERY row that drew, the three non-standing
		 * controls included (they reach here by goto; review finding: a
		 * control that poisons must be blamed itself, not the next row).
		 * Rows withheld without drawing - uniforms-invalid, auto-invalid,
		 * textures-invalid - cannot have poisoned anything and skip it.
		 * Rows 0..1 are validated by their own verdicts. */
		if (i > 1 && !canary_paints(ctx, canary, rt_a_off, rt_depth_off,
		                            rt_pitch, save_a)) {
			printf("shader-differential: RSX state poisoned after row %d (%s, role %s): a constant-colour canary draw painted nothing, so no later verdict would mean anything; %d rows not judged\nSHADER_DIFF_INVALID\n",
			       i, p->name, p->role, g_npairs - 1 - i);
			free(canary);
			cellGcmSetWaitFlip(ctx);
			cellGcmFinish(ctx, 1);
			free(host_addr);
			return 2;
		}
	}
	free(canary);

	int corpus = g_npairs - 2;
	printf("shader-differential: controls valid, %d judged pairs, %d gate failures, %d uniform-dependent pairs skipped, %d sampler-dependent pairs skipped, %d pairs the binder could not serve, %d vacuous pairs (neither side painted), %d path-pair rows unoracled, %d unstable pairs (a side never repeated a frame)\n",
	       corpus - uniforms_skipped - textures_skipped - unsupported - vacuous - unoracled - unstable,
	       failures, uniforms_skipped, textures_skipped, unsupported, vacuous, unoracled, unstable);
	/* What makes a red uniform control fail the run is the control's
	 * own failures++ in its branch above — by the time rows are
	 * skipped, failures is already nonzero.  No second guard here:
	 * a line that re-failed on uniforms_skipped would be a no-op
	 * that READS as the load-bearing check (review finding). */
	printf("%s\n", failures == 0 ? "SHADER_DIFF_OK" : "SHADER_DIFF_FAIL");

	cellGcmSetWaitFlip(ctx);
	cellGcmFinish(ctx, 1);
	free(host_addr);
	return failures == 0 ? 0 : 1;
}
