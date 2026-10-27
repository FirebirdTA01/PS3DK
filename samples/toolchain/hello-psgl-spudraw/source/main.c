/* SPU draw RPCS3 HLE probe -- no-render.
 * Tests: PPU records hole -> SPU fills NOPs -> PPU CALL replays -> swap.
 * Gate: TTY "done" without dead FIFO means SPU RSX DMA works.
 * If dead FIFO or crash, document the limitation.
 */
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <malloc.h>
#include <ppu-types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/spu.h>
#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/synchronization.h>
#include <sys/timer.h>

#include "../../psgl-common/psgl_sample_quit.h"

#include "spu_hole_fill_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

typedef struct {
    unsigned long long initAddr, holeEA;
    uint32_t holeSizeInWord;
} __attribute__((aligned(128))) PSGLholeParams;

int main(void)
{
    PSGLinitOptions opts = {0};
    PSGLdevice *device;
    PSGLcontext *ctx;
    GLuint cmd_offset;
    void *cmdbuf;
    uint32_t holeEA, holeWords;
    PSGLholeParams *params;
    sys_spu_image_t spu_img;
    sys_spu_thread_group_t group;
    sys_spu_thread_t thread;
    int cause, status, rc;

    psglInit(&opts);
    device = psglCreateDeviceAuto(0, 0, 0);
    ctx = psglGetCurrentContext();
    if (!ctx) ctx = psglCreateContext();
    psglMakeCurrent(ctx, device);
    psgl_sample_viewport(device);

    cmdbuf = psglAllocateCommandBuffer(1024u * 1024u, &cmd_offset);
    if (!cmdbuf) { printf("spudraw: fail alloc\n"); psglExit(); return 1; }

    psglBeginCommandRecord(cmdbuf, 1024u * 1024u);
    rc = psglDrawCommandBufferHole(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT,
                                    NULL, NULL, &holeEA, &holeWords);
    psglEndCommandRecord(PSGL_ADD_RETURN);
    if (rc != 0) { printf("spudraw: fail hole\n"); psglExit(); return 1; }

    void *hole_ptr = (uint8_t *)cmdbuf + (holeEA - cmd_offset);

    params = memalign(128, sizeof(*params));
    if (!params) { printf("spudraw: fail params\n"); psglExit(); return 1; }
    memset(params, 0, sizeof(*params));
    void *init_data = psglGetSPUInitData();
    if (!init_data) {
        printf("spudraw: fail no IOIF config\n");
        psglFreeCommandBuffer(cmdbuf);
        free(params);
        psglExit();
        return 1;
    }
    params->initAddr = (unsigned long long)(uintptr_t)init_data;
    params->holeEA = (unsigned long long)(uintptr_t)hole_ptr;
    params->holeSizeInWord = holeWords;

    rc = sysSpuInitialize(6, 0);
    if (rc != 0 && rc != (int)0x80010030) {
        printf("spudraw: fail spu_init %x\n", rc); psglExit(); return 1;
    }
    rc = sys_spu_image_import(&spu_img, spu_hole_fill_bin, 0);
    if (rc != 0) { printf("spudraw: fail spu_img %x\n", rc); psglExit(); return 1; }

    sys_spu_thread_group_attribute_t ga = { .nsize = 8, .name = "spudraw", .type = 0 };
    rc = sys_spu_thread_group_create(&group, 1, 250, &ga);
    if (rc != 0) { printf("spudraw: fail group %x\n", rc); psglExit(); return 1; }

    sys_spu_thread_attribute_t ta = {
        .name = "spuhole", .nsize = 8, .option = SPU_THREAD_ATTR_NONE
    };
    sys_spu_thread_argument_t arg = { (uint64_t)(uintptr_t)params, 0, 0, 0 };
    rc = sys_spu_thread_initialize(&thread, group, 0, &spu_img, &ta, &arg);
    if (rc != 0) { printf("spudraw: fail thread %x\n", rc); psglExit(); return 1; }
    rc = sys_spu_thread_group_start(group);
    if (rc != 0) { printf("spudraw: fail start %x\n", rc); psglExit(); return 1; }

    /* Normal completion: join rc 0, every thread exited (cause
       SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT) with status 0; the group
       and image are then released, and both results are checked. */
    rc = sys_spu_thread_group_join(group, &cause, &status);
    int destroy_rc = sys_spu_thread_group_destroy(group);
    int close_rc = sys_spu_image_close(&spu_img);
    printf("spudraw: spu join rc=%d cause=%d status=%d destroy=%x close=%x\n",
           rc, cause, status, (unsigned)destroy_rc, (unsigned)close_rc);
    if (rc != 0 || cause != SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT || status != 0 ||
        destroy_rc != 0 || close_rc != 0) {
        printf("spudraw: fail spu teardown\n");
        psglExit(); return 1;
    }

    glClearColor(0.0f, 0.3f, 0.0f, 1.0f);
    printf("hello-psgl-spudraw: ready\n");
    int exit_code = psgl_sample_quit_begin("hello-psgl-spudraw");
    for (int i = 0; i < 900 && exit_code == 0; i++) {
        int quit = psgl_sample_quit_poll("hello-psgl-spudraw");
        if (quit < 0) { exit_code = 1; break; }
        if (quit > 0) break;
        glClear(GL_COLOR_BUFFER_BIT);
        psglCallCommandBuffer(cmdbuf);
        psglSwap();
        sys_timer_usleep(16000);
    }
    psgl_sample_quit_end();
    printf("hello-psgl-spudraw: %s\n", psgl_sample_quit_outcome(exit_code));

    free(params);
    psglFreeCommandBuffer(cmdbuf);
    psglExit();
    return exit_code;
}
