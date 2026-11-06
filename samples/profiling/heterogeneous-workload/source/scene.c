#include "scene.h"
#include <cell/gcm.h>
#include <cell/dbgfont.h>
#include <cell/sysutil.h>
#include <cell/pad.h>
#include <sysutil/video.h>
#include <sys/sys_time.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <Cg/cg.h>
#include "gcm_host_buffer.h"
#include "scene_vpo.h"
#include "light_fpo.h"
#include "heavy_fpo.h"
#include <ps3dk/prof.h>
#if SAMPLE_PROF_EXT
#include "prof_ext_rt.h"
#include "prof_gpu.h"
/* An RSX zone around a block of commands; a failed begin leaves the zone INERT (ended never), logged once.  Used
 * only inside #if SAMPLE_PROF_EXT blocks, so an OFF build preprocesses to today's tokens. */
static int gpu_zone_failed;
#define SCENE_GPU_BEGIN(z,name) do{uint32_t g_=pe_rt_gpu();if(g_ && PS3DK_PROF_GPU_ZONE_BEGIN(g_,ctx,name,z) && !gpu_zone_failed){gpu_zone_failed=1;puts("PROF_EXT gpu zone begin failed");}}while(0)
#define SCENE_GPU_END(z) do{uint32_t g_=pe_rt_gpu();if(g_ && (z).gpu && PS3DK_PROF_GPU_ZONE_END(g_,ctx,z) && !gpu_zone_failed){gpu_zone_failed=1;puts("PROF_EXT gpu zone end failed");}}while(0)
#endif
static uint16_t width,height;
static uint32_t pitch,depth,colors[2],vertex_offset,fragment_offset[2];
static unsigned current,submitted;
static atomic_uint flips,quit;
static int callback_owned,pad_owned,font_owned,flip_owned,failed;
static CGprogram vertex,fragment[2];
static void *vertex_code;
static CGparameter motion;
static unsigned position_index;
static volatile uint32_t *complete_label;
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"callback publication must be lock-free");
uint64_t sample_now_us(void){return (uint64_t)sys_time_get_system_time();}
static void on_flip(uint32_t head){(void)head;atomic_fetch_add_explicit(&flips,1,memory_order_release);}
static void on_sysutil(uint64_t status,uint64_t param,void *arg)
{(void)param;(void)arg;if(status==CELL_SYSUTIL_REQUEST_EXITGAME)atomic_store(&quit,1);}
int scene_poll(void)
{
    if(cellSysutilCheckCallback()!=0)return -1;
    if(pad_owned)for(unsigned p=0;p<7;++p){
        CellPadData data;
        if(cellPadGetData(p,&data)==CELL_OK && data.len>0 &&
           (data.button[CELL_PAD_BTN_OFFSET_DIGITAL1]&CELL_PAD_CTRL_START))atomic_store(&quit,1);
    }
    return atomic_load(&quit)?1:0;
}
int scene_init(void)
{
    sys_addr_t host=0;videoState state;videoResolution res;videoConfiguration cfg={0};
    if(gcm_host_buffer_alloc(32u<<20,&host) || cellGcmInit(1u<<20,32u<<20,(void *)(uintptr_t)host))return -1;
    if(videoGetState(0,0,&state) || state.state || videoGetResolution(state.displayMode.resolution,&res))return -1;
    width=res.width;height=res.height;
    if(width<320 || height<240 || width>4096 || height>4096)return -1;
    pitch=(uint32_t)width*4;
    cfg.resolution=state.displayMode.resolution;cfg.format=VIDEO_BUFFER_FORMAT_XRGB;
    cfg.pitch=pitch;cfg.aspect=state.displayMode.aspect;
    if(videoConfigure(0,&cfg,NULL,0))return -1;
    void *d=rsxMemalign(64,(size_t)height*pitch);
    if(!d || cellGcmAddressToOffset(d,&depth))return -1;
    for(unsigned i=0;i<2;++i){
        void *b=rsxMemalign(64,(size_t)height*pitch);
        if(!b || cellGcmAddressToOffset(b,&colors[i]) || cellGcmSetDisplayBuffer(i,colors[i],pitch,width,height))return -1;
    }
    size_t bytes=CELL_DBGFONT_FRAGMENT_SIZE+CELL_DBGFONT_TEXTURE_SIZE+12000*CELL_DBGFONT_VERTEX_SIZE;
    void *font=rsxMemalign(128,bytes);if(!font)return -1;
    CellDbgFontConfigGcm fc={0};fc.localBufAddr=(sys_addr_t)(uintptr_t)font;fc.localBufSize=(uint32_t)bytes;
    fc.option=CELL_DBGFONT_VERTEX_LOCAL|CELL_DBGFONT_TEXTURE_LOCAL;
    if(cellDbgFontInitGcm(&fc))return -1;
    font_owned=1;
    vertex=(CGprogram)scene_vpo;fragment[0]=(CGprogram)light_fpo;fragment[1]=(CGprogram)heavy_fpo;
    cellGcmCgInitProgram(vertex);
    uint32_t size=0;cellGcmCgGetUCode(vertex,&vertex_code,&size);
    if(!vertex_code || !size)return -1;
    CGparameter pos=cellGcmCgGetNamedParameter(vertex,"position");
    if(!pos)return -1;
    position_index=(unsigned)cellGcmCgGetParameterResource(vertex,pos)-CG_ATTR0;
    motion=cellGcmCgGetNamedParameter(vertex,"motion");if(!motion)return -1;
    for(unsigned i=0;i<2;++i){
        void *code=NULL;cellGcmCgInitProgram(fragment[i]);cellGcmCgGetUCode(fragment[i],&code,&size);
        if(!code || !size)return -1;
        void *local=rsxMemalign(64,size);if(!local)return -1;
        memcpy(local,code,size);if(cellGcmAddressToOffset(local,&fragment_offset[i]))return -1;
    }
    static const float vertices[9]={-1,-1,0, 3,-1,0, -1,3,0};
    void *vb=rsxMemalign(128,sizeof vertices);if(!vb)return -1;
    memcpy(vb,vertices,sizeof vertices);if(cellGcmAddressToOffset(vb,&vertex_offset))return -1;
    if(cellSysutilRegisterCallback(0,on_sysutil,NULL))return -1;
    callback_owned=1;
    if(cellPadInit(7)==CELL_OK)pad_owned=1;
    cellGcmSetFlipHandler(on_flip);flip_owned=1;cellGcmSetFlipMode(GCM_FLIP_VSYNC);
    complete_label=cellGcmGetLabelAddress(240);if(!complete_label)return -1;
    *complete_label=0;__asm__ volatile("sync" ::: "memory");
    return 0;
}
static char status_line[96];
void scene_set_status(const char *text){snprintf(status_line,sizeof status_line,"%s",text?text:"");}
int scene_render(int heavy,const char *phase,unsigned workers,uint64_t compute_us,
                 uint64_t frame_us,uint64_t *gpu_begin,uint64_t *gpu_end,int *gpu_valid)
{
    if(failed || submitted==UINT32_MAX)return -1;
    CellGcmContextData *ctx=CELL_GCM_CURRENT;CellGcmSurface sf={0};
    sf.colorFormat=GCM_SURFACE_X8R8G8B8;sf.colorTarget=GCM_SURFACE_TARGET_0;
    for(unsigned i=0;i<4;++i){sf.colorLocation[i]=GCM_LOCATION_RSX;sf.colorPitch[i]=64;}
    sf.colorOffset[0]=colors[current];sf.colorPitch[0]=pitch;
    sf.depthFormat=GCM_SURFACE_ZETA_Z16;sf.depthLocation=GCM_LOCATION_RSX;
    sf.depthOffset=depth;sf.depthPitch=pitch;sf.type=GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias=GCM_SURFACE_CENTER_1;sf.width=width;sf.height=height;
    float scale[4]={width/2.0f,-(float)height/2.0f,0.5f,0};
    float offset[4]={width/2.0f,height/2.0f,0.5f,0};
    {
        PS3DK_PROF_ZONE("render.submit");
#if SAMPLE_PROF_EXT
        ps3dk_prof_gpu_zone setup_zone=PS3DK_PROF_GPU_ZONE_INIT,draw_zone=PS3DK_PROF_GPU_ZONE_INIT;
        SCENE_GPU_BEGIN(setup_zone,"rsx.setup");
#endif
        cellGcmSetSurface(ctx,&sf);cellGcmSetViewport(ctx,0,0,width,height,0,1,scale,offset);
        cellGcmSetScissor(ctx,0,0,width,height);cellGcmSetDepthTestEnable(ctx,GCM_FALSE);
        cellGcmSetCullFaceEnable(ctx,GCM_FALSE);cellGcmSetBlendEnable(ctx,GCM_FALSE);
        cellGcmSetColorMask(ctx,GCM_COLOR_MASK_R|GCM_COLOR_MASK_G|GCM_COLOR_MASK_B|GCM_COLOR_MASK_A);
        cellGcmSetVertexProgram(ctx,vertex,vertex_code);
        float shift[4]={(float)(submitted%600)/600.0f,0,0,0};
        cellGcmSetVertexProgramParameter(ctx,motion,shift);
        cellGcmSetVertexDataArray(ctx,position_index,0,3*sizeof(float),3,GCM_VERTEX_DATA_TYPE_F32,GCM_LOCATION_RSX,vertex_offset);
        cellGcmSetFragmentProgram(ctx,fragment[heavy!=0],fragment_offset[heavy!=0]);
#if SAMPLE_PROF_EXT
        SCENE_GPU_END(setup_zone);
#endif
        cellGcmSetTimeStamp(ctx,0);
#if SAMPLE_PROF_EXT
        SCENE_GPU_BEGIN(draw_zone,"rsx.draw");
#endif
        for(unsigned i=0;i<(heavy?8u:1u);++i)cellGcmSetDrawArrays(ctx,GCM_TYPE_TRIANGLES,0,3);
#if SAMPLE_PROF_EXT
        SCENE_GPU_END(draw_zone);
#endif
        cellGcmSetTimeStamp(ctx,1);
        cellGcmSetWriteBackEndLabel(ctx,240,submitted+1);
        cellDbgFontPrintf(0.04f,0.05f,1.2f,0xffffffff,"PS3DK: PPU / SPE / RSX WORKLOAD");
        cellDbgFontPrintf(0.04f,0.14f,1.2f,0xff80ff80,"%s",phase);
        cellDbgFontPrintf(0.04f,0.23f,1.2f,0xffffffff,"SPE workers: %u | compute: %llu us | frame: %llu us",workers,(unsigned long long)compute_us,(unsigned long long)frame_us);
        char gpu_text[48];
        if(*gpu_valid)snprintf(gpu_text,sizeof gpu_text,"%llu raw ticks",(unsigned long long)(*gpu_end-*gpu_begin));
        else snprintf(gpu_text,sizeof gpu_text,"unavailable");
        cellDbgFontPrintf(0.04f,0.32f,1.2f,0xffffffff,"RSX scene: %s | prior scene: %s",heavy?"HEAVY (8 layers)":"LIGHT (1 layer)",gpu_text);
        cellDbgFontPrintf(0.04f,0.41f,1.2f,0xffdddddd,"Raw GPU ticks are not calibrated time. SPE ticks are per-job.");
        cellDbgFontPrintf(0.04f,0.50f,1.2f,0xffffffff,"Fixed batch: 16384 items x 128 rounds; PPU/SPE digest checked.");
        if(status_line[0])cellDbgFontPrintf(0.04f,0.66f,1.2f,0xff80ffff,"%s",status_line);
        cellDbgFontPrintf(0.04f,0.80f,1.2f,0xffffffff,"PPU capture: %s | CSV/logs: /dev_hdd0/tmp",PS3DK_PROF?"ON":"OFF");
        cellDbgFontPrintf(0.04f,0.88f,1.2f,0xffffffff,"START / XMB quit exits. Phases repeat until session deadline.");
        if(cellDbgFontDrawGcm()){failed=1;return -1;}
    }
    unsigned before=atomic_load_explicit(&flips,memory_order_acquire);
    cellGcmResetFlipStatus();
    if(cellGcmSetFlip(ctx,current)){failed=1;return -1;}
    cellGcmFlush(ctx);++submitted;
    {
        PS3DK_PROF_ZONE("render.flip_wait");
        uint64_t start=sample_now_us();
        for(;;){
            unsigned after=atomic_load_explicit(&flips,memory_order_acquire);
            if(after>before+1){failed=1;return -1;}
            if(after==before+1 && cellGcmGetFlipStatus()==0 && *complete_label==submitted)break;
            if(scene_poll()<0 || sample_now_us()-start>=2000000){failed=1;return -1;}
            usleep(200);
        }
    }
    __asm__ volatile("sync" ::: "memory");
    *gpu_begin=cellGcmGetTimeStamp(0);*gpu_end=cellGcmGetTimeStamp(1);
    *gpu_valid=*gpu_begin!=0 && *gpu_end>=*gpu_begin;
    current^=1;return 0;
}
void scene_close(void)
{
    if(flip_owned){cellGcmSetFlipHandler(NULL);flip_owned=0;}
    if(callback_owned){cellSysutilUnregisterCallback(0);callback_owned=0;}
    if(pad_owned){cellPadEnd();pad_owned=0;}
    if(font_owned && !failed){cellDbgFontExitGcm();font_owned=0;}
    /* All mapped GPU storage stays allocated until process exit, including
     * timeout/error paths where the RSX may still reference it. */
}
