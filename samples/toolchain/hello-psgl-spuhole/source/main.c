/* SPU draw hole word-count validation (PPU side, no SPU). */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x100000);

static int test_count(GLsizei count, uint32_t expected)
{
    GLuint cmd_offset;
    void *cmdbuf;
    uint32_t holeEA, holeWords, *holeStart, *end;
    int ret;
    int ok = 1;

    cmdbuf = psglAllocateCommandBuffer(1024u * 1024u, &cmd_offset);
    if (!cmdbuf) { printf("spuhole: fail alloc\n"); return 0; }

    psglBeginCommandRecord(cmdbuf, 1024u * 1024u);
    ret = psglDrawCommandBufferHole(GL_TRIANGLES, count, GL_UNSIGNED_SHORT,
                                     NULL, NULL, &holeEA, &holeWords);
    end = (uint32_t *)psglGetCommandRecordCurrent();
    holeStart = (uint32_t *)((uint8_t *)cmdbuf + (holeEA - cmd_offset));
    psglEndCommandRecord(PSGL_DO_NOT_ADD_RETURN);

    if (ret != 0) {
        printf("spuhole: fail returned %d for count %d\n", ret, count);
        ok = 0;
        goto done;
    }
    if (holeWords != expected) {
        printf("spuhole: fail count %d expected %u got %u\n", count, expected, holeWords);
        ok = 0;
        goto done;
    }
    if ((uint32_t)(end - holeStart) != holeWords) {
        printf("spuhole: fail count %d hole span %ld vs holeWords %u\n",
               count, (long)(end - holeStart), holeWords);
        ok = 0;
        goto done;
    }
    /* first word must be JTS self-loop */
    if ((holeStart[0] & 0x20000000u) != 0x20000000u) {
        printf("spuhole: fail count %d first word not JTS: %08x\n", count, holeStart[0]);
        ok = 0;
        goto done;
    }
    /* JTS must be on 128-byte boundary */
    if ((uintptr_t)holeStart & 127u) {
        printf("spuhole: fail count %d hole not 128B-aligned\n", count);
        ok = 0;
        goto done;
    }

done:
    psglFreeCommandBuffer(cmdbuf);
    return ok;
}

int main(void)
{
    PSGLinitOptions opts = {0};
    PSGLdevice *device;
    PSGLcontext *ctx;
    int ok = 1;

    psglInit(&opts);
    device = psglCreateDeviceAuto(0, 0, 0);
    ctx = psglGetCurrentContext();
    if (!ctx) ctx = psglCreateContext();
    psglMakeCurrent(ctx, device);

    if (!test_count(3, 16u)) ok = 0;
    if (!test_count(4096, 32u)) ok = 0;
    if (!test_count(1000000, 4044u)) ok = 0;

    if (ok)
        printf("hello-psgl-spuhole: ok\n");
    else
        printf("hello-psgl-spuhole: fail\n");

    psglExit();
    return ok ? 0 : 1;
}
