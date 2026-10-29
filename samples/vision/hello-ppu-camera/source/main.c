#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <sys/memory.h>
#include <cell/camera.h>
#include <cell/sysmodule.h>
#include <cell/sysutil.h>
#include "eye_capture.h"
#include "eye_display.h"
#include "eye_yuv.h"

SYS_PROCESS_PARAM(1001,0x100000);
enum { WIDTH=640, HEIGHT=480, FRAME_BYTES=WIDTH*HEIGHT*2, RGBA_BYTES=WIDTH*HEIGHT*4 };
#ifndef EYE_FRAMES
#define EYE_FRAMES 120
#endif
#ifndef EYE_BUDGET_US
#define EYE_BUDGET_US UINT64_C(30000000)
#endif
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"exit callback requires lock-free atomics");
static atomic_uint quitting;
typedef struct Camera {
    CellCameraInfoEx info;
    sys_memory_container_t container;
    uint8_t *rgba;
    int open;
} Camera;
static const char *stage_name(enum EyeStage s)
{
    static const char *names[]={"load","init","open","start","stop","close","end","unload",
                               "read","present","callbacks","deadline","frame"};
    return (unsigned)s<sizeof(names)/sizeof(names[0]) ? names[s] : "unknown";
}
static void exit_callback(uint64_t status,uint64_t param,void *data)
{
    (void)param; (void)data;
    if (status==CELL_SYSUTIL_REQUEST_EXITGAME)
        atomic_store_explicit(&quitting,1,memory_order_release);
}
static int pump(void *p)
{
    (void)p;
    int rc=cellSysutilCheckCallback();
    return rc ? rc : atomic_load_explicit(&quitting,memory_order_acquire) ? 1 : 0;
}
static int control(void *p,enum EyeStage stage)
{
    Camera *camera=p;
    int rc;
    switch (stage) {
    case EYE_LOAD: rc=cellSysmoduleLoadModule(CELL_SYSMODULE_CAMERA); break;
    case EYE_INIT: rc=cellCameraInit(); break;
    case EYE_OPEN:
        if (cellCameraIsAttached(0)!=1) { rc=(int)CELL_CAMERA_ERROR_DEVICE_NOT_FOUND; break; }
        camera->info.format=CELL_CAMERA_YUV422;
        camera->info.resolution=CELL_CAMERA_VGA;
        camera->info.framerate=30;
        camera->info.info_ver=CELL_CAMERA_INFO_VER;
        camera->info.container=camera->container;
        camera->info.read_mode=CELL_CAMERA_READ_FUNCCALL;
        rc=cellCameraOpenEx(0,&camera->info);
        if (!rc) camera->open=1;
        break;
    case EYE_START:
        printf("EYE_FORMAT requested=Y0_U_Y1_V/640x480/30 returned=%d/%dx%d/%d mode=%d bytes=%d\n",
               (int)camera->info.format,camera->info.width,camera->info.height,camera->info.framerate,
               camera->info.read_mode,camera->info.bytesize);
        if (camera->info.format!=CELL_CAMERA_YUV422 || camera->info.width!=WIDTH ||
            camera->info.height!=HEIGHT || camera->info.framerate!=30 ||
            camera->info.read_mode!=CELL_CAMERA_READ_FUNCCALL ||
            camera->info.bytesize!=FRAME_BYTES || !camera->info.buffer) {
            rc=EYE_INVALID_FRAME; break;
        }
        rc=cellCameraStart(0); break;
    case EYE_STOP: rc=cellCameraStop(0); break;
    case EYE_CLOSE:
        rc=cellCameraClose(0);
        if (!rc) camera->open=0;
        break;
    case EYE_END: rc=cellCameraEnd(); break;
    case EYE_UNLOAD: rc=cellSysmoduleUnloadModule(CELL_SYSMODULE_CAMERA); break;
    default: rc=EYE_BAD_ARGUMENT; break;
    }
    printf("EYE_STAGE %s rc=0x%08x\n",stage_name(stage),(unsigned)rc);
    return rc;
}
static int read_frame(void *p,EyeFrame *frame)
{
    Camera *camera=p;
    CellCameraReadEx read={0};
    read.version=CELL_CAMERA_READ_VER;
    read.pbuf=camera->info.buffer;
    int rc=cellCameraReadEx(0,&read);
    if ((uint32_t)rc==CELL_CAMERA_ERROR_BUSY || (uint32_t)rc==CELL_CAMERA_ERROR_TIMEOUT) return 1;
    if (rc) return rc;
    /* Before the first frame the API can return no payload. Consume the
     * module-owned function-call buffer before asking for the next frame. */
    if (!read.bytesread) return 1;
    if (read.bytesread!=FRAME_BYTES) return EYE_INVALID_FRAME;
    frame->data=camera->info.buffer;
    frame->bytes=read.bytesread;
    frame->number=read.frame;
    frame->timestamp=(uint64_t)read.timestamp;
    return 0;
}
static int present(void *p,const EyeFrame *frame)
{
    Camera *camera=p;
    if (eye_yuv422_rgba(frame->data,frame->bytes,WIDTH*2,camera->rgba,RGBA_BYTES,WIDTH*4,WIDTH,HEIGHT))
        return EYE_INVALID_FRAME;
    return eye_display_present(camera->rgba,RGBA_BYTES,pump,p);
}
static uint64_t now(void *p) { (void)p; return (uint64_t)sys_time_get_system_time(); }
static void wait_frame(void *p) { (void)p; usleep(500); }

int main(void)
{
    Camera camera={0}; EyeStats stats={0};
    int rc=0, callback_owned=0, container_owned=0, capture_started=0;
    const char *setup_stage="allocate";
    camera.rgba=malloc(RGBA_BYTES);
    if (!camera.rgba) { rc=-1; goto finish; }
    setup_stage="container";
    rc=sys_memory_container_create(&camera.container,2*1024*1024);
    if (rc) goto finish;
    container_owned=1;
    setup_stage="register_callback";
    rc=cellSysutilRegisterCallback(0,exit_callback,NULL);
    if (rc) goto finish;
    callback_owned=1;
    setup_stage="display_init";
    rc=eye_display_init();
    if (rc) goto finish;
    printf("EYE_BEGIN width=640 height=480 fps=30 format=Y0_U_Y1_V frames=%u budget_us=%" PRIu64 "\n",
           (unsigned)EYE_FRAMES,(uint64_t)EYE_BUDGET_US);
    EyeOps ops={&camera,control,read_frame,present,pump,now,wait_frame};
    capture_started=1;
    rc=eye_capture_run(&ops,EYE_FRAMES,EYE_BUDGET_US,&stats);
finish:
    if (!capture_started) stats.first_error=rc;
    eye_display_end();
    if (callback_owned) {
        int cleanup=cellSysutilUnregisterCallback(0);
        printf("EYE_CLEANUP callback rc=0x%08x\n",(unsigned)cleanup);
        if (cleanup) { ++stats.cleanup_failures; if (rc>=0) rc=cleanup; }
    }
    if (container_owned && !camera.open) {
        int cleanup=sys_memory_container_destroy(camera.container);
        printf("EYE_CLEANUP container rc=0x%08x\n",(unsigned)cleanup);
        if (cleanup) { ++stats.cleanup_failures; if (rc>=0) rc=cleanup; }
    }
    /* A failed close may leave the container referenced; process teardown
     * reclaims it. The camera buffer is never freed by the application. */
    if (camera.open) puts("EYE_CLEANUP container retained after failed close");
    free(camera.rgba);
    uint64_t fps_milli=stats.elapsed_us ? (uint64_t)stats.fresh*UINT64_C(1000000000)/stats.elapsed_us : 0;
    printf("EYE_SUMMARY fresh=%u duplicates=%u displayed=%u bytes_per_frame=%u elapsed_us=%" PRIu64
           " observed_fps_milli=%" PRIu64 " first_timestamp=%" PRIu64 " last_timestamp=%" PRIu64
           " cleanup_failures=%u first_stage=%s first_rc=0x%08x\n",
           stats.fresh,stats.duplicates,stats.displayed,FRAME_BYTES,stats.elapsed_us,fps_milli,
           stats.first_timestamp,stats.last_timestamp,stats.cleanup_failures,
           capture_started ? stage_name(stats.error_stage) : setup_stage,(unsigned)stats.first_error);
    if (rc<0) { printf("EYE_FAIL rc=0x%08x\n",(unsigned)rc); return 1; }
    if (rc==1) { puts("EYE_QUIT"); return 0; }
    if (stats.displayed!=EYE_FRAMES) { puts("EYE_FAIL incomplete"); return 1; }
    puts("EYE_OK");
    return 0;
}
