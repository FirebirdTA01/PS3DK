// hello-spu-dma — PPU side.
//
// Hands an SPU thread two EAs: a 16-element input buffer and a same-sized
// output buffer.  The SPU reads input via cellDmaGet, doubles each element,
// writes via cellDmaPut, and pings a completion word.
// Verifies the SPU-side <cell/dma.h> wrappers round-trip correctly.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/spu.h>

#include "hello_spu_dma_bin.h"

#define ptr2ea(x) ((u64)((uintptr_t)(x)))

#define N_ELEMS 16

static u32 input  [N_ELEMS] __attribute__((aligned(128)));
static u32 output [N_ELEMS] __attribute__((aligned(128)));
static u32 spu_done           __attribute__((aligned(128))) = 0;

int main(int argc, char **argv)
{
    u32 cause, status;
    sysSpuImage image;
    u32 thread_id, group_id;

    printf("hello-spu-dma: ppu main starting\n");

    for (u32 i = 0; i < N_ELEMS; ++i) {
        input[i] = i + 1;       // 1..16
        output[i] = 0xdeadbeef; // sentinel
    }

    sysSpuInitialize(6, 0);
    sysSpuImageImport(&image, hello_spu_dma_bin, 0);

    sysSpuThreadGroupAttribute grpattr = {
        .nsize     = 12,
        .name      = (const char *)ptr2ea("hello_dma_g"),
        .type      = 0,
        .option.ct = 0,
    };
    sysSpuThreadGroupCreate(&group_id, 1, 100, &grpattr);

    sysSpuThreadAttribute attr = {
        .name   = (const char *)ptr2ea("hello_dma_t"),
        .nsize  = 12,
        .option = SPU_THREAD_ATTR_NONE,
    };
    sysSpuThreadArgument arg = {
        .arg0 = ptr2ea(output),
        .arg1 = ptr2ea(input),
        .arg2 = ptr2ea(&spu_done),
        .arg3 = N_ELEMS,
    };

    sysSpuThreadInitialize(&thread_id, group_id, 0, &image, &attr, &arg);
    printf("hello-spu-dma: ppu starting spu thread group\n");
    sysSpuThreadGroupStart(group_id);

    while (spu_done == 0) { /* spin */ }

    sysSpuThreadGroupJoin(group_id, &cause, &status);
    sysSpuImageClose(&image);

    int fail = 0;
    for (u32 i = 0; i < N_ELEMS; ++i) {
        u32 expected = (i + 1) * 2;
        if (output[i] != expected) {
            printf("hello-spu-dma: FAIL output[%u]=%u expected=%u\n",
                   i, output[i], expected);
            fail = 1;
        }
    }
    if (!fail) {
        printf("hello-spu-dma: PASS - %u/%u elements doubled correctly\n",
               N_ELEMS, N_ELEMS);
    }
    printf("hello-spu-dma: spu finished; cause=%u status=%u done=0x%x\n",
           cause, status, spu_done);
    return fail;
}
