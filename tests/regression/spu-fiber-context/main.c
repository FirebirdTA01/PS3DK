/* spu-fiber-context (PPU side): run the SPU fiber checks in one SPU thread
 * and report its exit status.
 *
 * Prints SPU_FIBER_OK, or SPU_FIBER_FAIL followed by the SPU program's
 * failed check number or the PPU call that failed.
 */
#include <stdio.h>
#include <stdint.h>
#include <sys/spu.h>

#include "spu_bin.h"

#define ptr2ea(x) ((u64)(uintptr_t)(x))

int main(void)
{
    sysSpuImage image;
    u32 thread_id, group_id, cause = 0, status = 0;
    s32 exit_status = -1;
    sysSpuThreadArgument arg = { 0, 0, 0, 0 };
    sysSpuThreadGroupAttribute grpattr = { sizeof("fibergrp"), ptr2ea("fibergrp"), 0, 0 };
    sysSpuThreadAttribute attr = { ptr2ea("fiberthr"), sizeof("fiberthr"), SPU_THREAD_ATTR_NONE };

    if (sysSpuInitialize(6, 0) != 0
            || sysSpuImageImport(&image, spu_bin, 0) != 0
            || sysSpuThreadGroupCreate(&group_id, 1, 100, &grpattr) != 0
            || sysSpuThreadInitialize(&thread_id, group_id, 0, &image, &attr, &arg) != 0
            || sysSpuThreadGroupStart(group_id) != 0
            || sysSpuThreadGroupJoin(group_id, &cause, &status) != 0
            || sysSpuThreadGetExitStatus(thread_id, &exit_status) != 0) {
        printf("SPU_FIBER_FAIL spu thread setup\n");
        return 0;
    }
    if (exit_status == 0)
        printf("SPU_FIBER_OK\n");
    else
        printf("SPU_FIBER_FAIL check %d\n", (int)exit_status);
    sysSpuThreadGroupDestroy(group_id);
    sysSpuImageClose(&image);
    return 0;
}
