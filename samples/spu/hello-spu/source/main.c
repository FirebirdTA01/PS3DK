// hello-spu — PPU side.
//
// Loads the SPU program blob (embedded via spu_bin_<target>.h), creates a
// thread group + thread, runs the SPU code, and waits for it to complete.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/spu.h>

#include "hello_spu_bin.h"

#define ptr2ea(x) ((u64)((uintptr_t)(x)))

// 16-byte aligned, cache-line safe. The SPU writes its output here via DMA.
static volatile u32 spu_done __attribute__((aligned(128))) = 0;

int main(int argc, char **argv)
{
    u32 cause, status;
    sysSpuImage image;
    u32 thread_id, group_id;

    printf("hello-spu: ppu main starting\n");

    sysSpuInitialize(6, 0);
    sysSpuImageImport(&image, hello_spu_bin, 0);

    sysSpuThreadGroupAttribute grpattr = {
        .nsize     = 8,
        .name      = (const char *)ptr2ea("hello_grp"),
        .type      = 0,
        .option.ct = 0,
    };
    sysSpuThreadGroupCreate(&group_id, 1, 100, &grpattr);

    sysSpuThreadAttribute attr = {
        .name   = (const char *)ptr2ea("hello_thr"),
        .nsize  = 9,
        .option = SPU_THREAD_ATTR_NONE,
    };
    sysSpuThreadArgument arg = {
        .arg0 = ptr2ea(&spu_done),
        .arg1 = 0,
        .arg2 = 0,
        .arg3 = 0,
    };

    sysSpuThreadInitialize(&thread_id, group_id, 0, &image, &attr, &arg);
    printf("hello-spu: ppu starting spu thread group\n");
    sysSpuThreadGroupStart(group_id);

    // Busy-wait on the SPU-flagged completion word. Real code would use the
    // event queue — this is the minimum viable check.
    while (spu_done == 0) { /* spin */ }

    sysSpuThreadGroupJoin(group_id, &cause, &status);
    sysSpuImageClose(&image);

    printf("hello-spu: spu finished; cause=%u status=%u done=%u\n",
           cause, status, spu_done);
    return 0;
}
