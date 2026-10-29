#include "eye_display.h"
#include "eye_yuv.h"
#include <cell/gcm.h>
#include <sysutil/video.h>
#include <sys/sys_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "../../../gcm/common/gcm_host_buffer.h"

enum { WIDTH=640, HEIGHT=480, FIFO_BYTES=1<<20, HOST_BYTES=4<<20 };
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"flip callback requires lock-free atomics");
static atomic_uint flips;
static uint8_t *pixels[2];
static uint32_t pitch, display_width, display_height;
static unsigned current=1;
static int ready;
static void flipped(uint32_t head)
{
    (void)head;
    atomic_fetch_add_explicit(&flips,1,memory_order_release);
}
int eye_display_init(void)
{
    sys_addr_t host=0;
    int rc=gcm_host_buffer_alloc(HOST_BYTES,&host);
    if (rc) return rc;
    rc=cellGcmInit(FIFO_BYTES,HOST_BYTES,(void *)(uintptr_t)host);
    /* GCM may map the FIFO even on partial failure. Retain until exit. */
    if (rc) return rc;
    videoState state;
    videoResolution resolution;
    videoConfiguration video={0};
    if ((rc=videoGetState(0,0,&state))) return rc;
    if (state.state!=0) return -1;
    if ((rc=videoGetResolution(state.displayMode.resolution,&resolution))) return rc;
    display_width=resolution.width; display_height=resolution.height;
    if (display_width<WIDTH || display_height<HEIGHT || display_width>4096 || display_height>4096) return -1;
    pitch=(display_width*4+63)&~63u;
    video.resolution=state.displayMode.resolution;
    video.format=VIDEO_BUFFER_FORMAT_XRGB;
    video.aspect=state.displayMode.aspect;
    video.pitch=pitch;
    if ((rc=videoConfigure(0,&video,NULL,0))) return rc;
    CellGcmConfig config;
    cellGcmGetConfiguration(&config);
    size_t size=(size_t)pitch*display_height;
    if (!config.localAddress || (uint64_t)size*2>config.localSize) return -1;
    for (unsigned n=0;n<2;++n) {
        uint32_t offset;
        pixels[n]=(uint8_t *)config.localAddress+n*size;
        if ((rc=cellGcmAddressToOffset(pixels[n],&offset))) return rc;
        if ((rc=cellGcmSetDisplayBuffer((uint8_t)n,offset,pitch,display_width,display_height))) return rc;
        for (size_t i=0;i<size;++i) pixels[n][i]=0;
    }
    __asm__ volatile("sync" ::: "memory");
    cellGcmSetFlipMode(GCM_FLIP_VSYNC);
    cellGcmSetFlipHandler(flipped);
    ready=1;
    printf("EYE_DISPLAY width=%u height=%u pitch=%u image=%ux%u centered=1\n",display_width,display_height,pitch,WIDTH,HEIGHT);
    return 0;
}
int eye_display_present(const uint8_t *rgba,size_t bytes,int (*pump)(void *),void *context)
{
    if (!ready) return -1;
    size_t begin=(size_t)((display_height-HEIGHT)/2)*pitch+(display_width-WIDTH)/2*4;
    size_t available=(size_t)pitch*display_height-begin;
    if (eye_rgba_xrgb(rgba,bytes,WIDTH*4,pixels[current]+begin,available,pitch,WIDTH,HEIGHT)) return -1;
    /* Publish CPU pixel stores before flip; only reuse the old buffer once
     * the new scanout is confirmed by callback and flip status. */
    __asm__ volatile("sync" ::: "memory");
    unsigned before=atomic_load_explicit(&flips,memory_order_acquire);
    cellGcmResetFlipStatus();
    int rc=cellGcmSetFlip(CELL_GCM_CURRENT,(uint8_t)current);
    if (rc) return rc;
    cellGcmFlush(CELL_GCM_CURRENT);
    uint64_t start=(uint64_t)sys_time_get_system_time();
    for (;;) {
        unsigned after=atomic_load_explicit(&flips,memory_order_acquire);
        if (after-before>1) return -1;
        if (after==before+1 && cellGcmGetFlipStatus()==0) break;
        rc=pump(context);
        if (rc<0) return rc;
        /* Record user exit, but drain the outstanding flip. */
        if ((uint64_t)sys_time_get_system_time()-start>=2000000) return -1;
        usleep(500);
    }
    current^=1;
    return 0;
}
void eye_display_end(void)
{
    if (ready) cellGcmSetFlipHandler(NULL);
    ready=0;
    /* Mapped memory and stalled scanout remain valid until process exit. */
}
