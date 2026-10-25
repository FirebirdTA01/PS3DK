/* spu-lowpart-spill (PPU side): run the SPU narrowing cases and report.
 *
 * The SPU thread reads one u64 from this program's memory through __ea
 * (its address goes in the first thread argument) and exits with a
 * bitmask of the cases that returned the wrong bytes.
 *
 * Prints SPU_LOWPART_SPILL_OK, or SPU_LOWPART_SPILL_FAIL followed by the
 * names of the failing cases or the PPU call that failed.
 */
#include <stdio.h>
#include <stdint.h>
#include <sys/spu.h>

#include "spu_bin.h"

static const char *const case_names[] = {
    "di_si_s", "di_si_u", "di_hi_s", "di_hi_u", "di_qi_s", "di_qi_u",
    "si_hi_s", "si_hi_u", "si_qi_s", "si_qi_u", "hi_qi_s", "hi_qi_u",
    "ti_di", "ti_si", "ti_qi", "v2di_e0", "v2di_e1", "v4si_e3",
    "sf_keep", "df_keep", "df_to_sf", "di_hiword", "ea_di_si",
    "stackarg_si_qi", "stackarg_si_hi", "stackarg_v2di_v4si",
};
#define CASE_COUNT ((int)(sizeof(case_names) / sizeof(case_names[0])))

/* The __ea case reads this; 16-byte aligned like every DMA source. */
static volatile uint64_t ea_v64 __attribute__((aligned(16))) = 0x8192a3b4c5d6e7f8ull;

int main(void)
{
    sysSpuImage image;
    u32 thread_id, group_id, cause = 0, status = 0;
    s32 exit_status = -1;
    sysSpuThreadArgument arg = { (u64)(uintptr_t)&ea_v64, 0, 0, 0 };
    sysSpuThreadGroupAttribute grpattr = {
        .nsize = sizeof("lowpartgrp"),
        .name  = "lowpartgrp",
        .type  = 0,
    };
    sysSpuThreadAttribute attr = {
        .name   = "lowpartthr",
        .nsize  = sizeof("lowpartthr"),
        .option = SPU_THREAD_ATTR_NONE,
    };

    if (sysSpuInitialize(6, 0) != 0
            || sysSpuImageImport(&image, spu_bin, 0) != 0
            || sysSpuThreadGroupCreate(&group_id, 1, 100, &grpattr) != 0
            || sysSpuThreadInitialize(&thread_id, group_id, 0, &image, &attr, &arg) != 0
            || sysSpuThreadGroupStart(group_id) != 0
            || sysSpuThreadGroupJoin(group_id, &cause, &status) != 0
            || sysSpuThreadGetExitStatus(thread_id, &exit_status) != 0) {
        printf("SPU_LOWPART_SPILL_FAIL spu thread setup\n");
        return 0;
    }
    if (cause != SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT || status != 0) {
        printf("SPU_LOWPART_SPILL_FAIL cause 0x%08x status 0x%08x exit %d\n",
               (unsigned)cause, (unsigned)status, (int)exit_status);
    } else if (exit_status == 0) {
        printf("SPU_LOWPART_SPILL_OK\n");
    } else {
        printf("SPU_LOWPART_SPILL_FAIL mask 0x%x:", (unsigned)exit_status);
        for (int i = 0; i < CASE_COUNT; ++i)
            if (exit_status & (1 << i))
                printf(" %s", case_names[i]);
        if ((unsigned)exit_status >> CASE_COUNT)
            printf(" unknown-bits");
        printf("\n");
    }
    sysSpuThreadGroupDestroy(group_id);
    sysSpuImageClose(&image);
    return 0;
}
