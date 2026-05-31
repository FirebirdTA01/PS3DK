/* SPU-driven indexed triangle draw — 19b acceptance.
 *
 * Tests: PPU sets up real render state (VP/FP + vertex/index buffers),
 * records a draw command-buffer hole, SPU fills it with a real indexed
 * draw via psglFillCommandBufferHole, PPU CALL replays every frame.
 *
 * Gate: solid-color triangle visible in RPCS3 (multi-checkpoint),
 * TTY "ready" + "done", no dead-FIFO, no segfault across 900 frames.
 *
 * This is the first exercise of the SPU-side GCM draw-index-array
 * encoder (cellGcmSetDrawIndexArrayUnsafeInline in libspuPSGL/api.c).
 */
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <Cg/cg.h>
#include <cell/gcm.h>
#include <malloc.h>
#include <ppu-types.h>
#include <rsx/mm.h>
#include <stddef.h>
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

#include "vpshader_vpo.h"
#include "fpshader_fpo.h"
#include "spu_hole_fill_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

/* Must match the SPU-side definition byte-for-byte. */
typedef struct {
    unsigned long long initAddr, holeEA;
    uint32_t holeSizeInWord;
    /* draw params for psglFillCommandBufferHole */
    uint32_t mode;
    int32_t count;
    uint32_t type;
    int32_t isIndexMainMemory;
    uint32_t indexOffset;
} __attribute__((aligned(128))) PSGLholeParams;

typedef struct {
    float pos[3];
    float color[4];
} vertex_t;

int main(void)
{
    PSGLinitOptions opts = {0};
    PSGLdevice *device;
    PSGLcontext *ctx;
    GLuint cmdbuf_offset, ib_offset, vb_offset;
    void *cmdbuf;
    void *ibuf, *vbuf;
    uint32_t holeEA, holeWords;
    PSGLholeParams *params;
    sysSpuImage spu_img;
    sys_spu_thread_group_t group;
    sys_spu_thread_t thread;
    int cause, status, rc;

    /* ---- PSGL init ---- */
    psglInit(&opts);
    device = psglCreateDeviceAuto(0, 0, 0);
    ctx = psglGetCurrentContext();
    if (!ctx) ctx = psglCreateContext();
    psglMakeCurrent(ctx, device);
    glViewport(0, 0, 1280, 720);

    /* ---- Shaders: CgBinaryProgram → cellGcmCg* ---- */
    void *vp_ucode, *fp_ucode_blob;
    u32 vpsize = 0, fpsize = 0;
    CGprogram vpo = (CGprogram)vpshader_vpo;
    CGprogram fpo = (CGprogram)fpshader_fpo;
    cellGcmCgInitProgram(vpo);
    cellGcmCgInitProgram(fpo);
    cellGcmCgGetUCode(vpo, &vp_ucode, &vpsize);
    cellGcmCgGetUCode(fpo, &fp_ucode_blob, &fpsize);

    CGparameter mvpConst = cellGcmCgGetNamedParameter(vpo, "modelViewProj");
    CGparameter posParam = cellGcmCgGetNamedParameter(vpo, "in_position");
    CGparameter colParam = cellGcmCgGetNamedParameter(vpo, "in_color");
    int position_index = posParam
        ? (int)cellGcmCgGetParameterResource(vpo, posParam) - CG_ATTR0 : 0;
    int color_index = colParam
        ? (int)cellGcmCgGetParameterResource(vpo, colParam) - CG_ATTR0 : 3;

    /* FP ucode: allocate from RSX local memory via rsxMemalign.
       cellGcmSetFragmentProgram hardcodes GCM_LOCATION_RSX in the
       address register, so the ucode must be in RSX-local space. */
    u32 fp_ucode_local_size = (fpsize + 63u) & ~63u;
    void *fp_ucode_local = rsxMemalign(64u, fp_ucode_local_size);
    if (!fp_ucode_local) {
        printf("spudraw-triangle: fail fp_ucode alloc\n");
        psglExit(); return 1;
    }
    memcpy(fp_ucode_local, fp_ucode_blob, fpsize);
    u32 fp_offset;
    cellGcmAddressToOffset(fp_ucode_local, &fp_offset);

    /* ---- Geometry: vertex + index buffers in main memory ----
       cellGcmMapMainMemory requires 1 MiB alignment; allocate one
       large block and partition it. */
    {
        u32 vb_size = 3 * sizeof(vertex_t);  /* 84 bytes */
        u32 ib_size = 3 * sizeof(u16);       /* 6 bytes */
        u32 block_size = (vb_size + ib_size + 0xFFFFFu) & ~0xFFFFFu;
        void *block = memalign(1024u * 1024u, block_size);
        if (!block) {
            printf("spudraw-triangle: fail buffer alloc\n");
            psglExit(); return 1;
        }
        vbuf = block;
        ibuf = (u8 *)block + vb_size;
        rc = cellGcmMapMainMemory(block, block_size, &vb_offset);
        if (rc != 0) {
            printf("spudraw-triangle: fail vb map %x\n", rc);
            psglExit(); return 1;
        }
        ib_offset = vb_offset + vb_size;

        /* Fill vertex data (must happen after mapping). */
        {
            vertex_t *v = (vertex_t *)vbuf;
            v[0] = (vertex_t){{ 0.0f,  0.6f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}};
            v[1] = (vertex_t){{-0.6f, -0.6f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
            v[2] = (vertex_t){{ 0.6f, -0.6f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
        }
        {
            u16 *idx = (u16 *)ibuf;
            idx[0] = 0; idx[1] = 1; idx[2] = 2;
        }
    }

    /* ---- Command buffer: record render state + hole ---- */
    cmdbuf = psglAllocateCommandBuffer(1024u * 1024u, &cmdbuf_offset);
    if (!cmdbuf) { printf("spudraw-triangle: fail alloc cmdbuf\n"); psglExit(); return 1; }
    psglBeginCommandRecord(cmdbuf, 1024u * 1024u);

    /* Push render state BEFORE the hole.  The RSX sees these commands,
       then hits the hole (initially JTS), then the SPU fills the draw
       command, then RETURN. */
    {
        CellGcmContextData *gctx = gCellGcmCurrentContext;
        static const float mvp[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
        };

        cellGcmSetVertexProgram(gctx, vpo, vp_ucode);
        cellGcmSetVertexProgramParameter(gctx, mvpConst, mvp);

        cellGcmSetVertexDataArray(gctx, position_index, 0,
            sizeof(vertex_t), 3, CELL_GCM_VERTEX_F,
            CELL_GCM_LOCATION_MAIN,
            vb_offset + (u32)offsetof(vertex_t, pos));
        cellGcmSetVertexDataArray(gctx, color_index, 0,
            sizeof(vertex_t), 4, CELL_GCM_VERTEX_F,
            CELL_GCM_LOCATION_MAIN,
            vb_offset + (u32)offsetof(vertex_t, color));

        cellGcmSetFragmentProgram(gctx, fpo, fp_offset);
    }

    /* Record the hole.  psglDrawCommandBufferHole currently stubs
       *indexOffset = 0 — we compute the real index offset ourselves
       and pass it directly to the SPU (sample-local workaround). */
    {
        u32 stubIndexOffset;
        rc = psglDrawCommandBufferHole(GL_TRIANGLES, 3,
            GL_UNSIGNED_SHORT, NULL, &stubIndexOffset, &holeEA, &holeWords);
        if (rc != 0) {
            printf("spudraw-triangle: fail hole %d\n", rc);
            psglExit(); return 1;
        }
    }
    psglEndCommandRecord(PSGL_ADD_RETURN);

    /* ---- SPU: dispatch with extended params ---- */
    void *hole_ptr = (uint8_t *)cmdbuf + (holeEA - cmdbuf_offset);
    params = memalign(128, sizeof(*params));
    if (!params) { printf("spudraw-triangle: fail params\n"); psglExit(); return 1; }
    memset(params, 0, sizeof(*params));

    void *init_data = psglGetSPUInitData();
    if (!init_data) {
        printf("spudraw-triangle: fail no IOIF config\n");
        psglFreeCommandBuffer(cmdbuf);
        free(params);
        psglExit();
        return 1;
    }

    params->initAddr         = (unsigned long long)(uintptr_t)init_data;
    params->holeEA           = (unsigned long long)(uintptr_t)hole_ptr;
    params->holeSizeInWord   = holeWords;
    params->mode             = GL_TRIANGLES;
    params->count            = 3;
    params->type             = GL_UNSIGNED_SHORT;
    params->isIndexMainMemory = 1;    /* index buffer in main RAM */
    params->indexOffset      = ib_offset;

    rc = sysSpuInitialize(6, 0);
    if (rc != 0 && rc != (int)0x80010030) {
        printf("spudraw-triangle: fail spu_init %x\n", rc); psglExit(); return 1;
    }
    rc = sysSpuImageImport(&spu_img, spu_hole_fill_bin, 0);
    if (rc != 0) { printf("spudraw-triangle: fail spu_img %x\n", rc); psglExit(); return 1; }

    sys_spu_thread_group_attribute_t ga = { .nsize = 8, .name = "spudrawtri", .type = 0 };
    rc = sys_spu_thread_group_create(&group, 1, 250, &ga);
    if (rc != 0) { printf("spudraw-triangle: fail group %x\n", rc); psglExit(); return 1; }

    sys_spu_thread_attribute_t ta = {
        .name = "spuhole3", .nsize = 8, .option = SPU_THREAD_ATTR_NONE
    };
    sys_spu_thread_argument_t arg = { (uint64_t)(uintptr_t)params, 0, 0, 0 };
    rc = sys_spu_thread_initialize(&thread, group, 0, &spu_img, &ta, &arg);
    if (rc != 0) { printf("spudraw-triangle: fail thread %x\n", rc); psglExit(); return 1; }
    rc = sys_spu_thread_group_start(group);
    if (rc != 0) { printf("spudraw-triangle: fail start %x\n", rc); psglExit(); return 1; }

    rc = sys_spu_thread_group_join(group, &cause, &status);
    sys_spu_thread_group_destroy(group);
    sysSpuImageClose(&spu_img);
    if (rc != 0 || status != 0) {
        printf("spudraw-triangle: fail spu exit rc=%d cause=%d status=%d\n",
            rc, cause, status);
        psglExit(); return 1;
    }

    /* ---- Render loop: clear, CALL the filled command buffer, swap ---- */
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);
    printf("hello-psgl-spudraw-triangle: ready\n");
    for (int i = 0; i < 900; i++) {
        glClear(GL_COLOR_BUFFER_BIT);
        psglCallCommandBuffer(cmdbuf);
        psglSwap();
        sys_timer_usleep(16000);
    }
    printf("hello-psgl-spudraw-triangle: done\n");

    free(params);
    /* Do not rsxFree RSX-local here: the rsx-mm freelist metadata lives
       in RSX-mapped memory that is invalid at PSGL teardown (r_alloc
       faults writing 0xd... -- see RPCS3 VM access violation). Process
       exit reclaims it. */
    free(vbuf);   /* vb/ib share one block; free the base pointer */
    psglFreeCommandBuffer(cmdbuf);
    psglExit();
    return 0;
}
