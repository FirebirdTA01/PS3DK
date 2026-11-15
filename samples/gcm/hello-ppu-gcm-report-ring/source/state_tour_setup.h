/* Setup functions copied unchanged from hello-ppu-cellgcm-state-tour.
 * The host test compares each function and the reused shader paths. */
#ifndef RRP_STATE_TOUR_SETUP_H
#define RRP_STATE_TOUR_SETUP_H
#define TILE_W 64
#define TILE_H 64
#define TILE_PITCH (TILE_W * 4u)
#define CB_SIZE 0x40000
#define HOST_HEAP_SIZE (8 * 1024 * 1024)
static uint32_t *g_tile, *g_depth, *g_texel;
static uint32_t g_tile_offset, g_depth_offset, g_texel_offset, g_local_heap;
static CGprogram g_vp, g_fp;
static void *g_vp_ucode;
static uint32_t g_fp_offset;
static int g_position_attr, g_color_attr, g_uv_attr;
typedef struct { float pos[2]; float color[4]; float uv[2]; } vertex_t;
static vertex_t *g_vb;
static uint32_t g_vb_offset;
static void *local_align(uint32_t align, uint32_t size)
{
	uint32_t sz = (size + 0x3ff) & ~0x3ffu;
	g_local_heap = (g_local_heap + align - 1) & ~(align - 1u);
	void *p = (void *)(uintptr_t)g_local_heap;
	g_local_heap += sz;
	return p;
}

static int init_rsx(void *host_addr)
{
	CellGcmConfig cfg;

	if (cellGcmInit(CB_SIZE, HOST_HEAP_SIZE, host_addr) != 0) {
		printf("  cellGcmInit failed\n");
		return 0;
	}
	cellGcmGetConfiguration(&cfg);
	g_local_heap = (uint32_t)(uintptr_t)cfg.localAddress;

	uint32_t sz = TILE_PITCH * TILE_H;

	g_tile    = (uint32_t *)local_align(64, sz);
	g_depth   = (uint32_t *)local_align(64, sz);
	if (cellGcmAddressToOffset(g_tile,  &g_tile_offset)  != 0) return 0;
	if (cellGcmAddressToOffset(g_depth, &g_depth_offset) != 0) return 0;

	/* 1x1 white texture (RGBA8) — identity for the frag multiply. */
	g_texel = (uint32_t *)local_align(64, 4);
	cellGcmAddressToOffset(g_texel, &g_texel_offset);
	g_texel[0] = 0xFFFFFFFFu;

	return 1;
}

static void init_shaders(void)
{
	uint32_t vpsize = 0, fpsize = 0;

	g_vp = (CGprogram)vpshader_vpo;
	g_fp = (CGprogram)fpshader_fpo;

	cellGcmCgInitProgram(g_vp);
	cellGcmCgInitProgram(g_fp);

	cellGcmCgGetUCode(g_vp, &g_vp_ucode, &vpsize);

	void *fp_blob;
	cellGcmCgGetUCode(g_fp, &fp_blob, &fpsize);
	uint32_t fp_copy = (uint32_t)fpsize;
	uint32_t *dst = (uint32_t *)local_align(64, fp_copy ? fp_copy : 4);
	memcpy(dst, fp_blob, fp_copy);
	cellGcmAddressToOffset(dst, &g_fp_offset);

	/* cellGcmCgGetParameterResource returns the CGresource enumerator,
	 * not a 0-based array index: in_position -> CG_ATTR0, in_color ->
	 * CG_ATTR1/CG_ATTR2/..., in_texcoord -> CG_TEX0, etc.  Subtract the
	 * per-class base to get the 0-based index that
	 * cellGcmSetVertexDataArray / cellGcmSetVertexAttribInputMask
	 * expect (matches hello-ppu-cellgcm-alpha-mask main.c:387-389). */
	CGparameter pos = cellGcmCgGetNamedParameter(g_vp, "in_position");
	CGparameter col = cellGcmCgGetNamedParameter(g_vp, "in_color");
	CGparameter uv  = cellGcmCgGetNamedParameter(g_vp, "in_uv");
	g_position_attr = pos ? (int)cellGcmCgGetParameterResource(g_vp, pos) - CG_ATTR0 : 0;
	g_color_attr    = col ? (int)cellGcmCgGetParameterResource(g_vp, col) - CG_ATTR0 : 1;
	g_uv_attr       = uv  ? (int)cellGcmCgGetParameterResource(g_vp, uv)  - CG_ATTR0 : 2;
}

static void init_geometry(void)
{
	g_vb = (vertex_t *)local_align(64, 4 * sizeof(vertex_t));
	/* CCW quad: bottom-left, bottom-right, top-right, top-left */
	g_vb[0].pos[0] = -1.0f; g_vb[0].pos[1] = -1.0f;
	g_vb[1].pos[0] =  1.0f; g_vb[1].pos[1] = -1.0f;
	g_vb[2].pos[0] =  1.0f; g_vb[2].pos[1] =  1.0f;
	g_vb[3].pos[0] = -1.0f; g_vb[3].pos[1] =  1.0f;
	for (int i = 0; i < 4; i++) {
		g_vb[i].color[0] = 1.0f; g_vb[i].color[1] = 1.0f;
		g_vb[i].color[2] = 1.0f; g_vb[i].color[3] = 1.0f;
		g_vb[i].uv[0] = 0.5f;   g_vb[i].uv[1] = 0.5f;
	}
	cellGcmAddressToOffset(g_vb, &g_vb_offset);
}

static void bind_surface(CellGcmContextData *ctx)
{
	CellGcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat      = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = g_tile_offset;
	sf.colorPitch[0]    = TILE_PITCH;
	sf.colorLocation[1] = GCM_LOCATION_RSX;
	sf.colorLocation[2] = GCM_LOCATION_RSX;
	sf.colorLocation[3] = GCM_LOCATION_RSX;
	sf.colorPitch[1]    = 64; sf.colorPitch[2] = 64; sf.colorPitch[3] = 64;
	sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation    = GCM_LOCATION_RSX;
	sf.depthOffset      = g_depth_offset;
	sf.depthPitch       = TILE_PITCH;
	sf.type             = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias        = GCM_SURFACE_CENTER_1;
	sf.width            = TILE_W;
	sf.height           = TILE_H;
	sf.x                = 0;
	sf.y                = 0;
	cellGcmSetSurface(ctx, &sf);
}

static void env_reset(CellGcmContextData *ctx)
{
	bind_surface(ctx);

	float minz = 0.0f, maxz = 1.0f;
	float scale[4]  = { TILE_W * 0.5f, TILE_H * -0.5f, 0.5f, 0.0f };
	float offset[4] = { TILE_W * 0.5f, TILE_H *  0.5f, 0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, TILE_W, TILE_H, minz, maxz, scale, offset);
	cellGcmSetScissor(ctx, 0, 0, TILE_W, TILE_H);

	cellGcmSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	cellGcmSetColorMaskMrt(ctx, 0);

	cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
	cellGcmSetDepthFunc(ctx, GCM_LESS);
	cellGcmSetDepthMask(ctx, GCM_FALSE);

	cellGcmSetStencilTestEnable(ctx, GCM_FALSE);
	cellGcmSetTwoSidedStencilTestEnable(ctx, GCM_FALSE);

	cellGcmSetBlendEnable(ctx, GCM_FALSE);
	cellGcmSetLogicOpEnable(ctx, GCM_FALSE);

	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);

	cellGcmSetFogMode(ctx, GCM_FOG_MODE_LINEAR);
	cellGcmSetFogParams(ctx, 0.0f, 0.0f);

	/* Re-bind the identity texture on unit 0 so the frag multiply is
	 * a no-op by default. */
	CellGcmTexture t;
	memset(&t, 0, sizeof(t));
	t.format    = CELL_GCM_TEXTURE_A8R8G8B8 | CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_NR;
	t.mipmap    = 1;
	t.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
	t.cubemap   = CELL_GCM_FALSE;
	/* identity remap: each output channel comes from the same texel channel */
	t.remap     = (CELL_GCM_TEXTURE_REMAP_REMAP <<  8) | (CELL_GCM_TEXTURE_REMAP_REMAP << 10)
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 12) | (CELL_GCM_TEXTURE_REMAP_REMAP << 14)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_A << 0) | (CELL_GCM_TEXTURE_REMAP_FROM_R << 2)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_G << 4) | (CELL_GCM_TEXTURE_REMAP_FROM_B << 6);
	t.width     = 1;
	t.height    = 1;
	t.depth     = 1;
	t.location  = CELL_GCM_LOCATION_LOCAL;
	t.pitch     = 4;
	t.offset    = g_texel_offset;
	cellGcmSetTexture(ctx, 0, &t);
	cellGcmSetTextureControl(ctx, 0, CELL_GCM_TRUE, 0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
	cellGcmSetTextureFilter(ctx, 0, 0, CELL_GCM_TEXTURE_NEAREST, CELL_GCM_TEXTURE_NEAREST,
	                        CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	cellGcmSetTextureAddress(ctx, 0,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE, CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL, CELL_GCM_TEXTURE_ZFUNC_LESS, 0);
	cellGcmSetTextureBorderColor(ctx, 0, 0xFFFFFFFFu);

	/* Programs + vertex data arrays.  The fragment program needs both
	 * the per-vertex colour (COLOR) and the per-vertex UV (TEXCOORD0)
	 * routed to it, so both must be bound as arrays and present in the
	 * input mask (the input mask here is source-compat; the binding is
	 * what selects the hardware input slot). */
	cellGcmSetVertexProgram(ctx, g_vp, g_vp_ucode);
	cellGcmSetFragmentProgram(ctx, g_fp, g_fp_offset);

	cellGcmSetVertexDataArray(ctx, g_position_attr, 0, (uint8_t)sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          g_vb_offset + (uint32_t)offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, g_color_attr, 0, (uint8_t)sizeof(vertex_t), 4,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          g_vb_offset + (uint32_t)offsetof(vertex_t, color));
	cellGcmSetVertexDataArray(ctx, g_uv_attr, 0, (uint8_t)sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          g_vb_offset + (uint32_t)offsetof(vertex_t, uv));
	cellGcmSetVertexAttribInputMask(ctx, (uint16_t)((1u << g_position_attr) |
	                                                  (1u << g_color_attr) |
	                                                  (1u << g_uv_attr)));
	cellGcmSetVertexAttribOutputMask(ctx,
	    GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE | GCM_ATTRIB_OUTPUT_MASK_TEX0);
}
#endif
