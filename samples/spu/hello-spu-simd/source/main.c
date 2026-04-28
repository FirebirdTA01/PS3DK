// hello-spu-simd — PPU side.
//
// Allocates a results buffer, hands its EA to the SPU thread, waits for
// the thread to DMA results back, then prints each field with the
// expected value next to it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <ppu-types.h>
#include <sys/spu.h>

#include "hello_spu_simd_bin.h"

#define ptr2ea(x) ((u64)((uintptr_t)(x)))

// Layout must match the SPU-side Results struct byte-for-byte.
typedef struct __attribute__((aligned(16))) {
    float    flt_lanes[4];
    int32_t  int_lanes[4];
    uint32_t bool_gather;
    uint32_t bool_any;
    uint32_t bool_all;
    uint32_t pad0;
    double   dbl_lanes[2];
    int64_t  llong_lanes[2];
    uint64_t ullong_lanes[2];
    uint16_t short_lanes[8];
    uint16_t ushort_pack;
    uint16_t pad1[7];
    uint8_t  uchar_lanes[16];
} Results;

static volatile Results g_results __attribute__((aligned(128)));

// Sentinel for "SPU hasn't written yet": we initialize a known field to
// a value the SPU can't legitimately produce, then wait for it to flip.
static int spu_done(void)
{
    // The SPU writes 1.5 to flt_lanes[0] (1*0.5 + 0.5).  If it's still
    // zero, the DMA hasn't landed yet.
    return g_results.flt_lanes[0] != 0.0f;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    sysSpuImage image;
    u32 thread_id, group_id;
    u32 cause, status;

    printf("hello-spu-simd: ppu starting\n");

    memset((void *)&g_results, 0, sizeof(g_results));

    sysSpuInitialize(6, 0);
    sysSpuImageImport(&image, hello_spu_simd_bin, 0);

    sysSpuThreadGroupAttribute grpattr = {
        .nsize     = 9,
        .name      = (const char *)ptr2ea("simd_grp"),
        .type      = 0,
        .option.ct = 0,
    };
    sysSpuThreadGroupCreate(&group_id, 1, 100, &grpattr);

    sysSpuThreadAttribute attr = {
        .name   = (const char *)ptr2ea("simd_thr"),
        .nsize  = 9,
        .option = SPU_THREAD_ATTR_NONE,
    };
    sysSpuThreadArgument arg = {
        .arg0 = ptr2ea(&g_results),
        .arg1 = 0,
        .arg2 = 0,
        .arg3 = 0,
    };

    sysSpuThreadInitialize(&thread_id, group_id, 0, &image, &attr, &arg);
    sysSpuThreadGroupStart(group_id);

    while (!spu_done()) { /* spin */ }

    sysSpuThreadGroupJoin(group_id, &cause, &status);
    sysSpuImageClose(&image);

    printf("\n=== float4 (a={1,2,3,4} * b={.5} + b) expect {1, 1.5, 2, 2.5} ===\n");
    for (int i = 0; i < 4; ++i)
        printf("  lane[%d] = %f\n", i, g_results.flt_lanes[i]);

    printf("\n=== int4 ((ia/2)<<1) expect {10,20,30,40} ===\n");
    for (int i = 0; i < 4; ++i)
        printf("  lane[%d] = %d\n", i, (int)g_results.int_lanes[i]);

    printf("\n=== bool4 (a<b for {1,2,3,4}<2.5) expect gather=0xc, any=1, all=0 ===\n");
    printf("  gather = 0x%x\n", (unsigned)g_results.bool_gather);
    printf("  any    = %u\n",   (unsigned)g_results.bool_any);
    printf("  all    = %u\n",   (unsigned)g_results.bool_all);

    printf("\n=== double2 ({1.5,2.5} * self) expect {2.25, 6.25} ===\n");
    printf("  lane[0] = %f\n", g_results.dbl_lanes[0]);
    printf("  lane[1] = %f\n", g_results.dbl_lanes[1]);

    printf("\n=== llong2 ({1,-1} + 0x100000000) expect {0x100000001, 0xffffffff} ===\n");
    printf("  lane[0] = 0x%016" PRIx64 "\n", (uint64_t)g_results.llong_lanes[0]);
    printf("  lane[1] = 0x%016" PRIx64 "\n", (uint64_t)g_results.llong_lanes[1]);

    printf("\n=== ullong2 ({0x1_00000000, 0x2_00000000} - 1) expect {0xffffffff, 0x1ffffffff} ===\n");
    printf("  lane[0] = 0x%016" PRIx64 "\n", g_results.ullong_lanes[0]);
    printf("  lane[1] = 0x%016" PRIx64 "\n", g_results.ullong_lanes[1]);

    printf("\n=== short8 ({100..800} + 11) expect {111,211,311,411,511,611,711,811} ===\n");
    for (int i = 0; i < 8; ++i)
        printf("  lane[%d] = %u\n", i, (unsigned)g_results.short_lanes[i]);

    printf("\n=== bool8 (us>17 for {0,5,10,15,20,25,30,35}) expect gather=0x0f ===\n");
    printf("  gather = 0x%02x\n", (unsigned)g_results.ushort_pack);

    printf("\n=== uchar16 ({1..16} + 100) expect {101..116} ===\n");
    for (int i = 0; i < 16; ++i)
        printf("  lane[%2d] = %u\n", i, (unsigned)g_results.uchar_lanes[i]);

    printf("\nhello-spu-simd: done; cause=%u status=%u\n", cause, status);
    return 0;
}
