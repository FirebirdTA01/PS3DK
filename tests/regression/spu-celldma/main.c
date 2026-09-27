/* spu-celldma, PPU side: prepares the source and the fill-patterned
 * destinations, runs the SPU program once, then checks every destination
 * byte: exactly the bytes each transfer targeted changed, and to the right
 * values.  Prints CELLDMA_OK, or CELLDMA_FAIL with the SPU's first failed
 * check and the PPU's; setup, join and status problems print
 * CELLDMA_INVALID. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/spu.h>
#include "spu_bin.h"
#include "values.h"

static uint8_t src[SRC_BYTES] __attribute__((aligned(128)));
static uint8_t dst[SLOTS * SLOT_BYTES] __attribute__((aligned(128)));
static uint8_t big[BIG_BYTES] __attribute__((aligned(128)));
static uint8_t small[128] __attribute__((aligned(128)));
static uint8_t scalars[128] __attribute__((aligned(128)));
static volatile uint32_t line[32] __attribute__((aligned(128)));
static celldma_control control;

static uint64_t ea(const volatile void *p) { return (uint64_t)(uintptr_t)p; }

/* The first failed PPU-side check, 0 when all hold. */
static unsigned judge(void)
{
    unsigned k, i;
    for (k = 0; k < SLOTS; ++k)
        for (i = 0; i < SLOT_BYTES; ++i) {
            int inside = i >= put_off[k] && i < put_off[k] + put_size[k];
            uint8_t want = inside ? put_byte(i) : FILL_DST;
            if (dst[k * SLOT_BYTES + i] != want)
                return 2;
        }
    for (i = 0; i < BIG_BYTES; ++i)
        if (big[i] != big_byte(i))
            return 4;
    for (i = 0; i < sizeof small; ++i) {
        int written = i >= 1 && i < 16;   /* 1@1, 2@2, 4@4, 8@8; byte 0 untouched */
        if (small[i] != (written ? put_byte(i) : FILL_DST))
            return 6;
    }
    {
        static const uint8_t want[16] = {
            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,   /* SCALAR64 at 0  */
            0x89, 0xab, 0xcd, 0xef,                           /* SCALAR32 at 8  */
            0xbe, 0xef,                                       /* SCALAR16 at 12 */
            FILL_DST,                                         /* untouched 14   */
            0x5a };                                           /* SCALAR8 at 15  */
        for (i = 0; i < sizeof scalars; ++i)
            if (scalars[i] != (i < 16 ? want[i] : FILL_DST))
                return 8;
    }
    if (line[0] != ATOMIC_ADDS || line[1] != LINE_LLUC || line[2] != LINE_QLLUC)
        return 9;
    for (i = 3; i < 32; ++i)
        if (line[i] != 0)
            return 9;
    return 0;
}

int main(void)
{
    sysSpuImage image;
    sys_spu_group_t group;
    sys_spu_thread_t thread;
    u32 cause = 0, status = 0;
    s32 spu_status = -1;
    unsigned i, ppu;
    sysSpuThreadGroupAttribute ga = { .nsize = sizeof("celldma"), .name = "celldma", .type = 0 };
    sysSpuThreadAttribute ta = { .name = "celldma", .nsize = sizeof("celldma"), .option = SPU_THREAD_ATTR_NONE };
    sysSpuThreadArgument args = { 0, 0, 0, 0 };   /* arg 1: control block EA */

    for (i = 0; i < SRC_BYTES; ++i)
        src[i] = src_byte(i);
    memset(dst, FILL_DST, sizeof dst);
    memset(big, FILL_DST, sizeof big);
    memset(small, FILL_DST, sizeof small);
    memset(scalars, FILL_DST, sizeof scalars);
    for (i = 0; i < 32; ++i)
        line[i] = 0;
    control.src = ea(src); control.dst = ea(dst); control.big = ea(big);
    control.small = ea(small); control.scalars = ea(scalars); control.line = ea(line);
    args = (sysSpuThreadArgument){ ea(&control), 0, 0, 0 };
    __sync_synchronize();

    (void)sysSpuInitialize(6, 0);   /* may already be initialised */
    if (sysSpuImageImport(&image, spu_bin, 0) != 0
            || sysSpuThreadGroupCreate(&group, 1, 100, &ga) != 0
            || sysSpuThreadInitialize(&thread, group, 0, &image, &ta, &args) != 0
            || sysSpuThreadGroupStart(group) != 0
            || sysSpuThreadGroupJoin(group, &cause, &status) != 0
            || sysSpuThreadGetExitStatus(thread, &spu_status) != 0) {
        printf("CELLDMA_INVALID setup or join\n");
        return 2;
    }
    __sync_synchronize();
    (void)sysSpuThreadGroupDestroy(group);
    (void)sysSpuImageClose(&image);
    if (cause != SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT || spu_status < 0 || spu_status > 10) {
        printf("CELLDMA_INVALID cause=%u status=%u spu=%d\n", cause, status, spu_status);
        return 2;
    }
    ppu = judge();
    printf("celldma: spu=%d ppu=%u\n", spu_status, ppu);
    if (spu_status || ppu) {
        printf("CELLDMA_FAIL spu=%d ppu=%u\n", spu_status, ppu);
        return 1;
    }
    printf("CELLDMA_OK\n");
    return 0;
}
