/* Inspect commands in private memory; nothing is submitted to the RSX. */
#include <cell/gcm.h>
#include <Cg/cgBinary.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fpshader_fpo.h"

int main(void)
{
    CGprogram program = (CGprogram)fpshader_fpo;
    const CgBinaryProgram *header = (const CgBinaryProgram *)program;
    void *ucode = NULL;
    uint32_t ucode_size = 0;
    uint32_t safe[4] = {0x13579bdf, 0, 0, 0x2468ace0};
    uint32_t unchecked[4];
    CellGcmContextData ctx;
    memcpy(unchecked, safe, sizeof safe);

    /* The embedded shader is built with the sample, not an arbitrary file. */
    cellGcmCgInitProgram(program);
    cellGcmCgGetUCode(program, &ucode, &ucode_size);
    if (!ucode || !ucode_size || header->program > header->totalSize ||
        sizeof(CgBinaryFragmentProgram) > header->totalSize - header->program) {
        puts("fp-control: FAIL invalid embedded program");
        return 1;
    }
    const CgBinaryFragmentProgram *fp = (const CgBinaryFragmentProgram *)
        ((const uint8_t *)program + header->program);

    memset(&ctx, 0, sizeof ctx);
    ctx.begin = ctx.current = safe + 1;
    ctx.end = safe + 3;
    cellGcmSetFragmentProgramControl(&ctx, program, 0, 0, 0);
    int ok = ctx.current == safe + 3;

    /* Unsafe requires the caller to reserve space. These two words are
     * private and preallocated, so no refill callback or GCM init is needed. */
    ctx.begin = ctx.current = unchecked + 1;
    ctx.end = unchecked + 3;
    cellGcmSetFragmentProgramControlUnsafe(&ctx, program, 0, 0, 0);
    ok = ok && ctx.current == unchecked + 3 &&
         memcmp(safe, unchecked, sizeof safe) == 0 &&
         safe[0] == 0x13579bdf && safe[3] == 0x2468ace0 &&
         safe[1] == 0x00041d60u;
    printf("fp-control: profile=%u registers=%u half=%u depth=%u kill=%u\n",
           (unsigned)cellGcmCgGetProgramProfile(program), fp->registerCount,
           fp->outputFromH0, fp->depthReplace, fp->pixelKill);
    printf("fp-control: method=%08x value=%08x ucode_bytes=%u\n",
           (unsigned)safe[1], (unsigned)safe[2], (unsigned)ucode_size);
    printf("hello-ppu-gcm-fp-control: %s; command emission only\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
