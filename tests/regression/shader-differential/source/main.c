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
 *         corpus | probe | reference (ours vs a reference-compiled
 *         container; gated like corpus)
 * status: identical | mismatch | load-failed-a | load-failed-b |
 *         uniform-missing-a | uniform-missing-b | uniforms-invalid
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

#include "sd_pos_uv_vpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)
#define MAX_BUFFERS 2

#define GCM_LABEL_INDEX 255

#define RT_W 64
#define RT_H 64
#define CLEAR_SENTINEL 0xFF000000u  /* readback fill: transfer-never-landed marker */
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

/* ---- one side of a pair: bind, draw (with warm-up), read back ---- */

/* Returns 0 on success, -1 if a uniform in the pair's set has no
 * matching named parameter in this side's container (the caller
 * reports which side). */
static int render_side(CellGcmContextData *ctx, void *container,
                       const char *uniform_set,
                       u32 rt_off, u32 rt_depth_off, u32 rt_pitch,
                       u32 *save, int *warmup_draws)
{
	CGprogram fpo = (CGprogram)container;
	cellGcmCgInitProgram(fpo);

	void *fp_blob_ucode; u32 fpsize = 0;
	cellGcmCgGetUCode(fpo, &fp_blob_ucode, &fpsize);
	void *fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_blob_ucode, fpsize);
	u32 fp_offset = 0;
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	/* Apply the pair's uniform set BEFORE the draw: the patch rides
	 * the FIFO's inline-transfer path against the copied ucode, so
	 * ordering relative to the draw commands is preserved. */
	if (!uniformSetIsNone(uniform_set)) {
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
	for (u32 i = 0; i < (rt_pitch / 4u) * RT_H; i++)
		g_readback[i] = CLEAR_SENTINEL;

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
	 * readback rig).  Probe = RT center.  A shader whose CORRECT
	 * center pixel equals GPU_CLEAR_MARK burns the whole budget and is
	 * then judged from the final image anyway — slow, never wrong,
	 * and both sides of a pair face the identical policy. */
	const u32 probe = (RT_H / 2) * (rt_pitch / 4u) + (RT_W / 2);
	int tries;
	for (tries = 1; tries <= 10; tries++) {
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
		wait_rsx_idle(ctx);
		transfer_rt_to_main(ctx, rt_off, rt_pitch);
		if (g_readback[probe] != GPU_CLEAR_MARK)
			break;
		usleep(200000);
	}
	*warmup_draws = tries;

	memcpy(save, g_readback, (size_t)rt_pitch * RT_H);
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
	return 1;
}

/* ---- pair judgment ---- */

typedef struct {
	const char *status;      /* identical | mismatch | load-failed-a/b */
	int  max_delta;
	int  diff_pixels;
	int  total_pixels;
	long elapsed_ms;
	char diagnostic[96];
	char artifact[96];
} sd_result;

static long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long)(tv.tv_sec * 1000L + tv.tv_usec / 1000L);
}

static void judge_pair(CellGcmContextData *ctx, const sd_pair *p,
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
	const char *set_b = strcmp(p->role, "control-uniform") == 0
		? "0" : p->uniform_set;

	int warm_a = 0, warm_b = 0;
	int ua = render_side(ctx, cont_a, p->uniform_set, rt_a_off,
	                     rt_depth_off, rt_pitch, save_a, &warm_a);
	int ub = render_side(ctx, cont_b, set_b, rt_b_off,
	                     rt_depth_off, rt_pitch, save_b, &warm_b);

	g_local_mem_heap = watermark;
	free(cont_a);
	free(cont_b);

	if (ua != 0 || ub != 0) {
		r->status = ua != 0 ? "uniform-missing-a" : "uniform-missing-b";
		snprintf(r->diagnostic, sizeof(r->diagnostic),
		         "set '%s' names a parameter the container lacks",
		         p->uniform_set);
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
	if (warm_a > 1 || warm_b > 1)
		snprintf(r->diagnostic, sizeof(r->diagnostic),
		         "warmup a=%d b=%d", warm_a, warm_b);
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
	vpo = (CGprogram)sd_pos_uv_vpo;
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
			continue;
		}

		judge_pair(ctx, p, rt_a_off, rt_b_off, rt_depth_off, rt_pitch,
		           save_a, save_b, &r);
		print_row(p, &r);

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
			continue;
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
		} else {
			if (strcmp(r.status, "identical") != 0)
				failures++;
		}
	}

	int corpus = g_npairs - 2;
	printf("shader-differential: controls valid, %d judged pairs, %d gate failures, %d uniform-dependent pairs skipped\n",
	       corpus - uniforms_skipped, failures, uniforms_skipped);
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
