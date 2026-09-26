/* spu-thread-args (PPU side): start one SPU thread with four distinct
 * 64-bit arguments and report whether its main() received all four.
 *
 * Prints SPU_THREAD_ARGS_OK, or SPU_THREAD_ARGS_FAIL followed by the
 * mask of wrong arguments or the PPU call that failed.
 */
#include <stdio.h>
#include <stdint.h>
#include <sys/spu.h>

#include "spu_bin.h"

int main(void)
{
    sysSpuImage image;
    u32 thread_id, group_id, cause = 0, status = 0;
    s32 exit_status = -1;
    sysSpuThreadArgument arg = {
        0x0123456789abcdefull,
        0xfedcba9876543210ull,
        0x1111222233334444ull,
        0x5555666677778888ull,
    };
    sysSpuThreadGroupAttribute grpattr = {
        .nsize = sizeof("argsgrp"),
        .name  = "argsgrp",
        .type  = 0,
    };
    sysSpuThreadAttribute attr = {
        .name   = "argsthr",
        .nsize  = sizeof("argsthr"),
        .option = SPU_THREAD_ATTR_NONE,
    };

    if (sysSpuInitialize(6, 0) != 0
            || sysSpuImageImport(&image, spu_bin, 0) != 0
            || sysSpuThreadGroupCreate(&group_id, 1, 100, &grpattr) != 0
            || sysSpuThreadInitialize(&thread_id, group_id, 0, &image, &attr, &arg) != 0
            || sysSpuThreadGroupStart(group_id) != 0
            || sysSpuThreadGroupJoin(group_id, &cause, &status) != 0
            || sysSpuThreadGetExitStatus(thread_id, &exit_status) != 0) {
        printf("SPU_THREAD_ARGS_FAIL spu thread setup\n");
        return 0;
    }
    if (exit_status == 0) {
        printf("SPU_THREAD_ARGS_OK\n");
    } else {
        printf("SPU_THREAD_ARGS_FAIL wrong argument mask 0x%x:", (unsigned)exit_status);
        for (int i = 0; i < 4; ++i)
            if (exit_status & (1 << i))
                printf(" arg%d", i + 1);
        printf("\n");
    }
    sysSpuThreadGroupDestroy(group_id);
    sysSpuImageClose(&image);
    return 0;
}
