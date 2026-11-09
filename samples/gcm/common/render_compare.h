/* Shared display path for the three focused rendering examples. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include "gcm_host_buffer.h"
#include "render_pixels.h"
#include <cell/gcm/gcm_swizzle_func.h>
#include <sys/sys_time.h>

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <sys/process.h>
#include <sysutil/video.h>
#include <io/pad.h>
#include <cell/gcm.h>
#include <cell/sysutil.h>
#include <rsx/rsx.h>

#include "vpshader_vpo.h"
#include "fpshader_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

/* Static sample resources; no per-frame allocation. */
#define CB_SIZE              0x100000
#define HOST_SIZE            (16 * 1024 * 1024)
#define COLOR_BUFFER_NUM     4
#define MAX_QUEUE_FRAMES     1

#define LABEL_PREPARED_BUFFER_OFFSET   0x41
#define LABEL_BUFFER_STATUS_OFFSET     0x42
#define BUFFER_IDLE                    0
#define BUFFER_BUSY                    1

#if SAMPLE_KIND == 3
#define TEX_W 8
#define PANEL_COUNT 6
#define SAMPLE_NAME "gcm-dxt"
#else
#define TEX_W 32
#define PANEL_COUNT 3
#if SAMPLE_KIND == 1
#define SAMPLE_NAME "gcm-fp-control"
#else
#define SAMPLE_NAME "gcm-swizzle"
#endif
#endif
#define TEX_H TEX_W
#define CHECK_LABEL 240
#define BACKGROUND 0xff202830u
static unsigned panel_size, panel_x[PANEL_COUNT], panel_y[PANEL_COUNT];
static unsigned pixel_checks, pixel_failures;
static uint32_t source_argb[TEX_W*TEX_H];
static uint32_t texture_offsets[PANEL_COUNT];
static uint8_t texture_formats[PANEL_COUNT];
static uint32_t texture_pitches[PANEL_COUNT];
static volatile uint32_t *readback;
static uint32_t readback_offset;
static char image_path[128], receipt_path[128];

typedef struct {
	float pos[2];
	float uv[2];
} vertex_t;

static u32 display_width, display_height;
static u32 color_pitch, depth_pitch;
static u32 color_offset[COLOR_BUFFER_NUM];
static u32 depth_offset;

static u32 g_local_mem_heap;
static void *local_alloc(u32 size) {
	u32 sz = (size + 1023) & ~1023u;
	u32 base = g_local_mem_heap;
	g_local_mem_heap += sz;
	return (void *)(uintptr_t)base;
}
static void *local_align(u32 alignment, u32 size) {
	g_local_mem_heap = (g_local_mem_heap + alignment - 1) & ~(alignment - 1);
	return local_alloc(size);
}

/* CgBinaryProgram blobs produced by rsx-cg-compiler.  We consume them
 * via the cellGcmCg* / cellGcmSet* API so the runtime parses the
 * CgBinary header layout instead of mis-reading our blob through the
 * legacy cgcomp `rsxVertexProgram` struct (the layouts are different
 * — feeding a CgBinaryProgram to rsxLoadVertexProgram crashes the
 * GPU). */
static CGprogram vpo;
static CGprogram fpo;
static void     *vp_ucode, *fp_ucode;
static u32       fp_offset;
static int       position_index;
static int       texcoord_index;

static vertex_t *vertex_buffer;
static u32       vertex_buffer_offset;


static volatile u32 g_buffer_on_display = 0;
static volatile u32 g_buffer_flipped    = 0;
static volatile u32 g_on_flip           = 0;
static u32          g_frame_index       = 0;

static volatile int g_exit_request = 0;
static volatile int g_drawing_paused = 0;

static void on_sysutil_event(uint64_t status, uint64_t param, void *userdata) {
	(void)param; (void)userdata;
	if (status == CELL_SYSUTIL_REQUEST_EXITGAME) g_exit_request = 1;
    if (status == CELL_SYSUTIL_DRAWING_BEGIN || status == CELL_SYSUTIL_SYSTEM_MENU_OPEN) g_drawing_paused=1;
    if (status == CELL_SYSUTIL_DRAWING_END || status == CELL_SYSUTIL_SYSTEM_MENU_CLOSE) g_drawing_paused=0;
}

static void on_flip(const u32 head) {
	(void)head;
	u32 v = g_buffer_flipped;
	for (u32 i = g_buffer_on_display; i != v; i = (i + 1) % COLOR_BUFFER_NUM)
		*(volatile u32 *)cellGcmGetLabelAddress(LABEL_BUFFER_STATUS_OFFSET + i) = BUFFER_IDLE;
	g_buffer_on_display = v;
	g_on_flip = 0;
}

static void on_vblank(const u32 head) {
	(void)head;
	u32 data = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET);
	u32 buffer_to_flip = data >> 8;
	u32 index_to_flip  = data & 0x7;
	if (g_on_flip == 0 && buffer_to_flip != g_buffer_on_display) {
		g_on_flip = 1;
		if (cellGcmSetFlipImmediate((u8)index_to_flip) != 0) { g_on_flip = 0; return; }
		g_buffer_flipped = buffer_to_flip;
	}
}

static int ppu_too_fast(void) {
	u32 gpu = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) >> 8;
	return (((g_frame_index + COLOR_BUFFER_NUM - gpu) % COLOR_BUFFER_NUM) > MAX_QUEUE_FRAMES);
}

static void set_render_target(CellGcmContextData *ctx, u32 idx) {
	CellGcmSurface sf = {0};
	sf.colorFormat      = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = color_offset[idx];
	sf.colorPitch[0]    = color_pitch;
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
	sf.width            = display_width;
	sf.height           = display_height;
	cellGcmSetSurface(ctx, &sf);
}

static void set_draw_env(CellGcmContextData *ctx) {
	cellGcmSetColorMask(ctx,
		GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	cellGcmSetColorMaskMrt(ctx, 0);

	float min = 0.0f, max = 1.0f;
	float scale[4]  = { display_width * 0.5f, display_height * -0.5f, (max - min) * 0.5f, 0.0f };
	float offset[4] = { display_width * 0.5f, display_height *  0.5f, (max + min) * 0.5f, 0.0f };
	cellGcmSetViewport(ctx, 0, 0, display_width, display_height, min, max, scale, offset);
	rsxSetScissor(ctx, 0, 0, display_width, display_height);

	cellGcmSetDepthTestEnable(ctx, GCM_FALSE);
	cellGcmSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	cellGcmSetCullFaceEnable(ctx, GCM_FALSE);
	cellGcmSetBlendEnable(ctx, GCM_FALSE);
}

static void bind_texture(CellGcmContextData *ctx, unsigned panel) {
	CellGcmTexture t = {0};
	t.format = texture_formats[panel];
	t.mipmap    = 1;
	t.dimension = CELL_GCM_TEXTURE_DIMENSION_2;
	t.cubemap   = CELL_GCM_FALSE;
	t.remap     = (CELL_GCM_TEXTURE_REMAP_REMAP <<  8) /* A */
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 10) /* R */
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 12) /* G */
	            | (CELL_GCM_TEXTURE_REMAP_REMAP << 14) /* B */
	            | (CELL_GCM_TEXTURE_REMAP_FROM_A << 0)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_R << 2)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_G << 4)
	            | (CELL_GCM_TEXTURE_REMAP_FROM_B << 6);
	t.width     = TEX_W;
	t.height    = TEX_H;
	t.depth     = 1;
	t.location  = CELL_GCM_LOCATION_LOCAL;
	t.pitch = texture_pitches[panel];
	t.offset = texture_offsets[panel];

	cellGcmSetTexture(ctx, 0, &t);
	cellGcmSetTextureControl(ctx, 0, CELL_GCM_TRUE,
	                         0, 0, CELL_GCM_TEXTURE_MAX_ANISO_1);
	cellGcmSetTextureFilter(ctx, 0,
	                        0,
	                        CELL_GCM_TEXTURE_NEAREST,
	                        CELL_GCM_TEXTURE_NEAREST,
	                        CELL_GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	cellGcmSetTextureAddress(ctx, 0,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_CLAMP_TO_EDGE,
	                         CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
	                         CELL_GCM_TEXTURE_ZFUNC_LESS,
	                         0);
}

static void set_render_state(CellGcmContextData *ctx) {
	cellGcmSetVertexProgram(ctx, vpo, vp_ucode);

	cellGcmSetVertexDataArray(ctx, position_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, pos));
	cellGcmSetVertexDataArray(ctx, texcoord_index, 0, sizeof(vertex_t), 2,
	                          CELL_GCM_VERTEX_F, CELL_GCM_LOCATION_LOCAL,
	                          vertex_buffer_offset + offsetof(vertex_t, uv));

	cellGcmSetFragmentProgram(ctx, fpo, fp_offset);

}

static void flip_and_setup_next(CellGcmContextData *ctx) {
	s32 qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index);
	while (qid < 0) { usleep(1000); qid = cellGcmSetPrepareFlip(ctx, (u8)g_frame_index); }
	cellGcmSetWriteBackEndLabel(ctx, LABEL_PREPARED_BUFFER_OFFSET,
	                            (g_frame_index << 8) | (u32)qid);
	cellGcmFlush(ctx);
	while (ppu_too_fast()) usleep(3000);

	set_draw_env(ctx);
	set_render_state(ctx);

	g_frame_index = (g_frame_index + 1) % COLOR_BUFFER_NUM;
	cellGcmSetWaitLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_IDLE);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);
	set_render_target(ctx, g_frame_index);
}

static int init_display(void) {
	videoState         state;
	videoConfiguration vcfg;
	videoResolution    res;
	if (videoGetState(0, 0, &state) != 0 || state.state != 0) return 0;
	if (videoGetResolution(state.displayMode.resolution, &res) != 0) return 0;

	display_width  = res.width;
	display_height = res.height;
	color_pitch    = display_width * sizeof(uint32_t);
	depth_pitch    = display_width * sizeof(uint16_t);

	memset(&vcfg, 0, sizeof(vcfg));
	vcfg.resolution = state.displayMode.resolution;
	vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
	vcfg.pitch      = color_pitch;
	vcfg.aspect     = state.displayMode.aspect;
	if (videoConfigure(0, &vcfg, NULL, 0) != 0) return 0;

	cellGcmSetFlipMode(GCM_FLIP_VSYNC);

	CellGcmConfig cfg;
	cellGcmGetConfiguration(&cfg);
	g_local_mem_heap = (u32)(uintptr_t)cfg.localAddress;

	u32 color_size = color_pitch * display_height;
	u32 depth_size = depth_pitch * display_height;
	void *color_base = local_align(64, COLOR_BUFFER_NUM * color_size);
	for (int i = 0; i < COLOR_BUFFER_NUM; i++) {
		void *addr = (void *)((uintptr_t)color_base + (u32)i * color_size);
		cellGcmAddressToOffset(addr, &color_offset[i]);
		cellGcmSetDisplayBuffer((u8)i, color_offset[i],
		                        color_pitch, display_width, display_height);
	}
	void *depth_addr = local_align(64, depth_size);
	cellGcmAddressToOffset(depth_addr, &depth_offset);

	for (int i = 0; i < COLOR_BUFFER_NUM; i++)
		*(volatile u32 *)cellGcmGetLabelAddress(LABEL_BUFFER_STATUS_OFFSET + i) = BUFFER_IDLE;
	*(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) = (g_buffer_on_display << 8);
	*(volatile u32 *)cellGcmGetLabelAddress(LABEL_BUFFER_STATUS_OFFSET + g_buffer_on_display) = BUFFER_BUSY;
	g_frame_index = (g_buffer_on_display + 1) % COLOR_BUFFER_NUM;

	cellGcmSetVBlankHandler(on_vblank);
	cellGcmSetFlipHandler(on_flip);
	return 1;
}

static void init_shaders(void) {
	u32 vpsize = 0, fpsize = 0;
	vpo = (CGprogram)vpshader_vpo;
	fpo = (CGprogram)fpshader_fpo;

	cellGcmCgInitProgram(vpo);
	cellGcmCgInitProgram(fpo);

	cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);

	void *fp_ucode_blob;
	cellGcmCgGetUCode(fpo, &fp_ucode_blob, &fpsize);
	fp_ucode = local_align(64, fpsize);
	memcpy(fp_ucode, fp_ucode_blob, fpsize);
	cellGcmAddressToOffset(fp_ucode, &fp_offset);

	/* Resolve vertex-attribute indices from the CgBinary parameter
	 * table.  cellGcmCgGetParameterResource returns CG_ATTR0..15 for
	 * vertex inputs; subtract CG_ATTR0 to get the attribute index
	 * the cellGcmSetVertexDataArray binding takes. */
	CGparameter pos = cellGcmCgGetNamedParameter(vpo, "in_position");
	CGparameter tc  = cellGcmCgGetNamedParameter(vpo, "in_texcoord");
	position_index  = pos ? (int)cellGcmCgGetParameterResource(vpo, pos) - CG_ATTR0 : 0;
	texcoord_index  = tc  ? (int)cellGcmCgGetParameterResource(vpo, tc)  - CG_ATTR0 : 8;
}

#if SAMPLE_KIND == 2
static void copy_texel(void *dst,const void *src) { memcpy(dst,src,4); }
#endif

static int init_texture(void)
{
    for (unsigned y=0; y<TEX_H; ++y) for (unsigned x=0; x<TEX_W; ++x) {
#if SAMPLE_KIND == 3
        const uint8_t *p=sample_rgba+4*(y*TEX_W+x);
        source_argb[y*TEX_W+x]=((u32)p[3]<<24)|((u32)p[0]<<16)|((u32)p[1]<<8)|p[2];
#else
        /* Asymmetric colors and a one-texel diagonal detect orientation and stride. */
        u32 color=0xff000000u | ((x*255/(TEX_W-1))<<16) |
                  ((y*255/(TEX_H-1))<<8) | (((x/4)^(y/4))&1 ? 0xe0u : 0x20u);
        if (x==y) color=0xffffffffu;
        source_argb[y*TEX_W+x]=color;
#endif
    }
    for (unsigned panel=0; panel<PANEL_COUNT; ++panel) {
        void *data=local_align(128,16384);
        memset(data,0,16384);
        texture_formats[panel]=CELL_GCM_TEXTURE_A8R8G8B8|CELL_GCM_TEXTURE_LN|CELL_GCM_TEXTURE_NR;
        texture_pitches[panel]=TEX_W*4;
#if SAMPLE_KIND == 2
        if (panel) {
            cellGcmConvertSwizzleFormat(data,source_argb,0,0,0,32,32,1,
                0,0,0,TEX_W,TEX_H,1,TEX_W,TEX_H,1,2,4,0,2,panel==2?copy_texel:NULL);
            texture_formats[panel]=CELL_GCM_TEXTURE_A8R8G8B8|CELL_GCM_TEXTURE_SZ|CELL_GCM_TEXTURE_NR;
            texture_pitches[panel]=0;
        } else memcpy(data,source_argb,sizeof source_argb);
#elif SAMPLE_KIND == 3
        if (panel&1) {
            const uint8_t *bytes=panel==1?sample_dxt1:panel==3?sample_dxt3:sample_dxt5;
            memcpy(data,bytes,panel==1?sizeof sample_dxt1:sizeof sample_dxt3);
            texture_formats[panel]=(panel==1?CELL_GCM_TEXTURE_COMPRESSED_DXT1:
                                   panel==3?CELL_GCM_TEXTURE_COMPRESSED_DXT23:
                                            CELL_GCM_TEXTURE_COMPRESSED_DXT45)|CELL_GCM_TEXTURE_NR;
            texture_pitches[panel]=0;
        } else memcpy(data,source_argb,sizeof source_argb);
#else
        memcpy(data,source_argb,sizeof source_argb);
#endif
        if (cellGcmAddressToOffset(data,&texture_offsets[panel])!=0) return 0;
    }
    return 1;
}

static void init_geometry(void)
{
    unsigned columns=SAMPLE_KIND==3?2:3;
    unsigned rows=SAMPLE_KIND==3?3:1;
    panel_size=display_width/(columns+1);
    if (panel_size>display_height/(rows+1)) panel_size=display_height/(rows+1);
    panel_size=(panel_size/TEX_W)*TEX_W;
    if (!panel_size) { puts("display too small"); exit(1); }
    unsigned gap_x=(display_width-columns*panel_size)/(columns+1);
    unsigned gap_y=(display_height-rows*panel_size)/(rows+1);
    vertex_buffer=(vertex_t *)local_align(128,4*PANEL_COUNT*sizeof(vertex_t));
    for (unsigned p=0;p<PANEL_COUNT;++p) {
        unsigned x=gap_x+(p%columns)*(panel_size+gap_x);
        unsigned y=gap_y+(p/columns)*(panel_size+gap_y);
        panel_x[p]=x; panel_y[p]=y;
        float l=2.0f*x/display_width-1, r=2.0f*(x+panel_size)/display_width-1;
        float t=1-2.0f*y/display_height, b=1-2.0f*(y+panel_size)/display_height;
        vertex_buffer[4*p+0]=(vertex_t){{l,t},{0,0}};
        vertex_buffer[4*p+1]=(vertex_t){{r,t},{1,0}};
        vertex_buffer[4*p+2]=(vertex_t){{l,b},{0,1}};
        vertex_buffer[4*p+3]=(vertex_t){{r,b},{1,1}};
        printf("panel %u at %u,%u size %u format %02x pitch %u\n",p,x,y,panel_size,texture_formats[p],texture_pitches[p]);
    }
    cellGcmAddressToOffset(vertex_buffer,&vertex_buffer_offset);
}

static int wait_readback_label(CellGcmContextData *ctx,u32 value)
{
    volatile u32 *label=(volatile u32 *)cellGcmGetLabelAddress(CHECK_LABEL);
    cellGcmSetWriteBackEndLabel(ctx,CHECK_LABEL,value);
    cellGcmSetWaitLabel(ctx,CHECK_LABEL,value);
    cellGcmSetWriteBackEndLabel(ctx,CHECK_LABEL,value+1);
    cellGcmFlush(ctx);
    uint64_t start=(uint64_t)sys_time_get_system_time();
    for (;;) {
        u32 seen=*label;
        uint64_t now=(uint64_t)sys_time_get_system_time();
        if (now<start || now-start>=10000000) { puts("readback fence timeout"); return 0; }
        if (seen==value+1) break;
        usleep(100);
    }
    __asm__ __volatile__("sync" ::: "memory");
    return 1;
}

static int readback_ready(CellGcmContextData *ctx)
{
    *(volatile u32 *)cellGcmGetLabelAddress(CHECK_LABEL)=0;
    __asm__ __volatile__("sync" ::: "memory");
    /* Finish rendering before starting the separate image-transfer engine. */
    if (!wait_readback_label(ctx,1)) return 0;
    cellGcmSetTransferImage(ctx,CELL_GCM_TRANSFER_LOCAL_TO_MAIN,
        readback_offset,color_pitch,0,0,color_offset[g_frame_index],color_pitch,
        0,0,display_width,display_height,4);
    return wait_readback_label(ctx,3);
}

static uint32_t expected_pixel(unsigned panel,unsigned x,unsigned y)
{
    u32 c=source_argb[y*TEX_W+x];
#if SAMPLE_KIND == 3
    if (panel>=2) {
        return sample_blend_rgb(c,BACKGROUND);
    }
#else
    (void)panel;
#endif
    return c;
}

static void inspect_pixels(volatile u32 *pixels)
{
    unsigned scale=panel_size/TEX_W;
    for (unsigned p=0;p<PANEL_COUNT;++p) {
        uint32_t expected[TEX_W*TEX_H];
        unsigned tolerance=(SAMPLE_KIND==3 && p==5)?20:2;
        for (unsigned y=0;y<TEX_H;++y) for (unsigned x=0;x<TEX_W;++x) {
            expected[y*TEX_W+x]=expected_pixel(p,x,y);
        }
        int errors=sample_check_panel(pixels,display_width,display_height,
            color_pitch/4,panel_x[p],panel_y[p],scale,TEX_W,TEX_H,expected,tolerance);
        if (errors<0) ++pixel_failures;
        else { pixel_checks+=TEX_W*TEX_H; pixel_failures+=(unsigned)errors; }
        printf("panel %u: %u pixels, %d mismatches, tolerance %u/255\n",p,TEX_W*TEX_H,errors,tolerance);
    }
}

static int close_file(FILE *f,int ok)
{
    if (fflush(f)!=0) ok=0;
    if (fsync(fileno(f))!=0) ok=0;
    if (fclose(f)!=0) ok=0;
    return ok;
}

static int save_image(volatile u32 *pixels)
{
    char tmp[144]; snprintf(tmp,sizeof tmp,"%s.tmp",image_path);
    FILE *f=fopen(tmp,"wb"); if (!f) return 0;
    int ok=fprintf(f,"P6\n%u %u\n255\n",display_width,display_height)>0;
    uint8_t *row=malloc(display_width*3);
    if (!row) ok=0;
    for (unsigned y=0;ok && y<display_height;++y) {
        for (unsigned x=0;x<display_width;++x) {
            u32 c=pixels[y*(color_pitch/4)+x];
            row[3*x]=(uint8_t)(c>>16); row[3*x+1]=(uint8_t)(c>>8); row[3*x+2]=(uint8_t)c;
        }
        if (fwrite(row,3,display_width,f)!=display_width) ok=0;
    }
    free(row);
    ok=close_file(f,ok);
    if (ok && rename(tmp,image_path)!=0) ok=0;
    if (!ok) unlink(tmp);
    return ok;
}

static int write_receipt(uint64_t run_id,int frames,int pass)
{
    char tmp[144]; snprintf(tmp,sizeof tmp,"%s.tmp",receipt_path);
    FILE *f=fopen(tmp,"wb"); if (!f) return 0;
    int ok=fprintf(f,"sample=%s run=%016llx frames=%d checks=%u mismatches=%u image=%s result=%s receipt=complete\n",
        SAMPLE_NAME,(unsigned long long)run_id,frames,pixel_checks,pixel_failures,image_path,pass?"PASS":"FAIL")>0;
    ok=close_file(f,ok);
    if (ok && rename(tmp,receipt_path)!=0) ok=0;
    if (!ok) unlink(tmp);
    return ok;
}

static void program_exit(CellGcmContextData *ctx) {
	cellGcmFinish(ctx, 1);
	u32 last = *(volatile u32 *)cellGcmGetLabelAddress(LABEL_PREPARED_BUFFER_OFFSET) >> 8;
	while (g_buffer_on_display != last) usleep(1000);
}

int main(int argc, const char **argv) {
	(void)argc; (void)argv;
	printf(SAMPLE_NAME ": rendering comparison panels\n");
    if (sample_checks() != 0) return 1;
    uint64_t run_id=(uint64_t)sys_time_get_system_time();
    snprintf(image_path,sizeof image_path,"/dev_hdd0/tmp/%s-%016llx.ppm",SAMPLE_NAME,(unsigned long long)run_id);
    snprintf(receipt_path,sizeof receipt_path,"/dev_hdd0/tmp/%s-%016llx.txt",SAMPLE_NAME,(unsigned long long)run_id);

	sys_addr_t           host_addr = 0;
	CellGcmContextData *ctx;
	/* LV2 pages, not the sbrk heap (see gcm_host_buffer.h).  Never freed after
	 * GCM init — the RSX may still fetch commands from it; process exit
	 * reclaims it. */
	if (gcm_host_buffer_alloc(HOST_SIZE, &host_addr) != 0) {
		printf("  gcm_host_buffer_alloc(%d MiB) failed\n", HOST_SIZE / (1024 * 1024));
		return 1;
	}
	if (cellGcmInit(CB_SIZE, HOST_SIZE, (void *)(uintptr_t)host_addr) != 0) { return 1; }
	if (!init_display())                                                  { return 1; }
	ctx = CELL_GCM_CURRENT;
	printf("  RSX up at %ux%u\n", display_width, display_height);
    if ((uint64_t)color_pitch*display_height>HOST_SIZE-CB_SIZE-128) return 1;
    readback=(volatile u32 *)((uintptr_t)host_addr+CB_SIZE+128);
    if (cellGcmAddressToOffset((void *)readback,&readback_offset)!=0) return 1;
    for (unsigned i=0;i<(color_pitch/4)*display_height;++i) readback[i]=0xfff100f1;

	ioPadInit(7);
	cellSysutilRegisterCallback(0, on_sysutil_event, NULL);

	init_shaders();
	if (!init_texture()) return 1;
	init_geometry();

	/* Push the VP literal-pool slots (e.g. c[467] = [0,1,0,0] for the
	 * `float4(x, y, 0, 1)` position output) into the const bank exactly
	 * once, before the first draw.  These are immutable shader literals;
	 * binding the program every frame does not need to re-upload them. */
	cellGcmCgUploadInternalConsts(ctx, vpo);

	set_draw_env(ctx);
	set_render_state(ctx);
	set_render_target(ctx, g_frame_index);
	cellGcmSetWriteCommandLabel(ctx, LABEL_BUFFER_STATUS_OFFSET + g_frame_index, BUFFER_BUSY);

	const int total = 60 * 10;
    int saved=0;
	int       frame = 0;

	while (frame < total && !g_exit_request) {
		cellSysutilCheckCallback();

		padInfo padinfo;
		ioPadGetInfo(&padinfo);
		for (int i = 0; i < MAX_PADS; i++) {
			if (padinfo.status[i]) {
				padData paddata = {0};
				ioPadGetData(i, &paddata);
				/* Detect rising edge — only exit on press, not hold */
				static uint16_t prev_start[MAX_PADS];
				uint16_t cur = paddata.BTN_START;
				if (cur && !prev_start[i]) {
					g_exit_request = 1; break;
				}
				prev_start[i] = cur;
			}
		}

        if (g_drawing_paused) { usleep(16000); continue; }
		cellGcmSetClearColor(ctx, BACKGROUND);
		cellGcmSetClearSurface(ctx,
			GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

        for (unsigned panel=0; panel<PANEL_COUNT; ++panel) {
            bind_texture(ctx,panel);
            cellGcmSetFragmentProgram(ctx,fpo,fp_offset);
#if SAMPLE_KIND == 1
            if (panel==1) cellGcmSetFragmentProgramControl(ctx,fpo,0,0,0);
            if (panel==2) {
                cellGcmReserveMethodSize(ctx,2);
                if (!ctx->current || !ctx->end || ctx->current>ctx->end ||
                    (size_t)(ctx->end-ctx->current)<2) return 1;
                cellGcmSetFragmentProgramControlUnsafe(ctx,fpo,0,0,0);
            }
#endif
#if SAMPLE_KIND == 3
            cellGcmSetBlendEnable(ctx,panel>=2 ? GCM_TRUE : GCM_FALSE);
            cellGcmSetBlendFunc(ctx,GCM_SRC_ALPHA,GCM_ONE_MINUS_SRC_ALPHA,GCM_ONE,GCM_ZERO);
#endif
            rsxDrawVertexArray(ctx,GCM_TYPE_TRIANGLE_STRIP,4*panel,4);
        }
        if (frame==0) {
            if (!readback_ready(ctx)) return 1;
            inspect_pixels(readback);
            saved=save_image(readback);
        }

		flip_and_setup_next(ctx);
		frame++;
	}

	printf("  drew %d frames\n", frame);
	program_exit(ctx);
	ioPadEnd();
	int ok=frame==total && saved && pixel_checks && !pixel_failures;
    if (!write_receipt(run_id,frame,ok)) ok=0;
    printf(SAMPLE_NAME ": %s frames=%d pixel_checks=%u pixel_failures=%u image=%s\n",ok?"PASS":"FAIL",frame,pixel_checks,pixel_failures,image_path);
	return ok ? 0 : 1;
}
