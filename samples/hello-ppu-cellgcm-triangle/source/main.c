/*
 * hello-ppu-cellgcm-triangle — Phase 7 step 1.5 / parallel-track demo.
 *
 * First sample to put **Sony's actual .cg shaders** (verbatim from
 * SDK 475.001 samples/sdk/graphics/gcm/flip_immediate/) through our
 * toolchain end-to-end:
 *
 *   .cg source (Sony)
 *     → cgcomp (PSL1GHT cgcomp + libCg, Path-3 pipeline)
 *     → .vpo / .fpo binary (NV40 + container)
 *     → bin2s + as → .o
 *     → linked into PPU ELF
 *     → loaded at runtime via rsxLoadVertexProgram /
 *       rsxLoadFragmentProgramLocation
 *     → RSX renders a triangle on screen
 *
 * Validates:
 *   - PSL1GHT cgcomp produces a usable NV40 ucode for Sony shaders
 *     (we already byte-diff'd the demosaic shader; this is the
 *      first end-to-end runtime test)
 *   - Sony's shader source compiles unchanged (no edits needed)
 *   - The PSL1GHT runtime accepts our compiled programs
 *
 * Caveats / temporary mismatches:
 *   - cellGcmSetVertexProgram(CGprogram, ...) and
 *     cellGcmSetFragmentProgram(CGprogram, offset) are deferred to a
 *     later Phase 7 step (need a Cg-runtime bridge layer).  This
 *     sample uses PSL1GHT-runtime program loaders directly:
 *       rsxLoadVertexProgram / rsxLoadFragmentProgramLocation
 *     for shader binding, and rsxBindVertexArrayAttrib for vertex
 *     data.  Once the Cg-handle forwarders ship, this sample can be
 *     re-spelled with cellGcmSetVertexProgram etc.
 *
 * Geometry: a single colored triangle in NDC space:
 *     V0 = ( 0.0,  0.6, 0.0)  red
 *     V1 = (-0.6, -0.6, 0.0)  green
 *     V2 = ( 0.6, -0.6, 0.0)  blue
 *   transformed by an identity modelViewProj.  Press START to exit.
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

#include "vpshader_vpo.h"
#include "fpshader_fpo.h"

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

typedef struct {
	float pos[3];
	float color[4];
} vertex_t;

static u32  depth_pitch;
static u32  depth_offset;
static u32 *depth_buffer;

/* Shader objects — compiled by PSL1GHT cgcomp from Sony's .vcg/.fcg. */
static rsxVertexProgram   *vpo;
static rsxFragmentProgram *fpo;
static void               *vp_ucode;
static void               *fp_ucode;
static u32                 fp_offset;
static rsxProgramConst    *modelViewProjConst;

/* Vertex buffer in RSX local memory. */
static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;

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
	if (!b->ptr) return 0;
	if (cellGcmAddressToOffset(b->ptr, &b->offset) != 0) return 0;
	if (cellGcmSetDisplayBuffer((uint8_t)id, b->offset, pitch, width, height) != 0) return 0;

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
	sf.x                = 0;
	sf.y                = 0;
	cellGcmSetSurface(ctx, &sf);
}

static void init_shaders(void)
{
	u32 vpsize = 0, fpsize = 0;

	/* Map the embedded blobs into rsx{Vertex,Fragment}Program *. */
	vpo = (rsxVertexProgram   *)vpshader_vpo;
	fpo = (rsxFragmentProgram *)fpshader_fpo;

	rsxVertexProgramGetUCode(vpo, &vp_ucode, &vpsize);
	modelViewProjConst = rsxVertexProgramGetConst(vpo, "modelViewProj");

	/* Fragment ucode lives in RSX local memory (the GPU samples it
	 * during fragment-shader execution; PSL1GHT's pattern is to
	 * memcpy out of the linker-loaded blob). */
	void *fp_ucode_blob;
	rsxFragmentProgramGetUCode(fpo, &fp_ucode_blob, &fpsize);
	fp_ucode = rsxMemalign(64, fpsize);
	memcpy(fp_ucode, fp_ucode_blob, fpsize);
	rsxAddressToOffset(fp_ucode, &fp_offset);

	printf("  vp ucode: %u bytes; mvp uniform: %p\n", vpsize, (void *)modelViewProjConst);
	printf("  fp ucode: %u bytes (rsx-local at offset 0x%08x)\n", fpsize, fp_offset);
}

static void init_geometry(void)
{
	vertex_buffer = (vertex_t *)rsxMemalign(128, 3 * sizeof(vertex_t));

	/* Triangle in NDC space, RGB per-vertex colors. */
	vertex_buffer[0] = (vertex_t){{ 0.0f,  0.6f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}};
	vertex_buffer[1] = (vertex_t){{-0.6f, -0.6f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
	vertex_buffer[2] = (vertex_t){{ 0.6f, -0.6f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};

	rsxAddressToOffset(vertex_buffer, &vertex_buffer_offset);
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

	printf("hello-ppu-cellgcm-triangle: Sony shaders + PSL1GHT cgcomp\n");

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

	init_shaders();
	init_geometry();

	/* Identity 4x4 modelViewProj — column-major float[16]. */
	float mvp[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1,
	};

	do_flip(ctx, MAX_BUFFERS - 1);

	const int total = 360;  /* ~6 s @ 60Hz */
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

		/* Dim grey background — triangle should pop. */
		cellGcmSetClearColor(ctx, 0xff404040);
		cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

		/* Bind shader programs.  Sony-name forwarders for these are
		 * deferred (need Cg-handle bridge); use rsx* directly. */
		rsxLoadVertexProgram(ctx, vpo, vp_ucode);
		rsxSetVertexProgramParameter(ctx, vpo, modelViewProjConst, mvp);
		rsxLoadFragmentProgramLocation(ctx, fpo, fp_offset, GCM_LOCATION_RSX);

		/* Bind vertex attributes: position to slot 0, color to slot 3. */
		rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0,
		                         vertex_buffer_offset + offsetof(vertex_t, pos),
		                         sizeof(vertex_t), 3,
		                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
		rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0,
		                         vertex_buffer_offset + offsetof(vertex_t, color),
		                         sizeof(vertex_t), 4,
		                         GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, 0, 3);

		wait_flip();
		do_flip(ctx, (uint8_t)buffers[cur].id);

		cur = (cur + 1) % MAX_BUFFERS;
		frame++;
	}

	printf("  drew %d frames\n", frame);
	if (exit_request) printf("  early exit on START button\n");

	cellGcmSetWaitFlip(ctx);
	rsxFree(vertex_buffer);
	rsxFree(fp_ucode);
	for (int i = 0; i < MAX_BUFFERS; i++) rsxFree(buffers[i].ptr);
	cellGcmFinish(ctx, 1);
	free(host_addr);
	ioPadEnd();

	printf("hello-ppu-cellgcm-triangle: done\n");
	return 0;
}
