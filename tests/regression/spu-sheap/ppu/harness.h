/* spu-sheap regression rows, PPU side: shared helpers.
 *
 * Each row embeds one SPU program (spu_bin.h from ps3_add_spu_image) and
 * runs it in a thread group of 1..6 threads, each with its own four
 * 64-bit arguments.  SPU programs report through their exit status (0 =
 * pass, else the number of the first failed check) and, where a row needs
 * values, a 128-byte result block they DMA into PPU memory.
 */
#ifndef SPU_SHEAP_HARNESS_H
#define SPU_SHEAP_HARNESS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/spu.h>
#include <cell/sheap.h>
#include <cell/sysmodule.h>

#define HARNESS_MAX_THREADS 6

/* 128-byte block an SPU program writes back: values[] meaning is per row. */
typedef struct harness_result {
    uint64_t values[16];
} __attribute__((aligned(128))) harness_result;

static inline uint64_t harness_ea(const volatile void *p)
{
    return (uint64_t)(uintptr_t)p;
}

/* Load the firmware module the cellSheap stub resolves against. */
static inline int harness_load_sheap(void)
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SHEAP);

    return rc == 0 || rc == (int)CELL_SYSMODULE_LOADED ? 0 : rc;
}

/* After a PPU (firmware) Initialize, word 2 of the header holds the heap's
 * own address.  The emulator's built-in (HLE) cellSheap returns success and
 * writes nothing, which leaves the fill pattern there. */
static inline int harness_firmware_wrote_header(const void *heap)
{
    uint64_t self;

    memcpy(&self, (const uint8_t *)heap + 8, sizeof(self));
    return self == harness_ea(heap);
}

typedef struct harness_group {
    sys_spu_group_t group;
    sys_spu_thread_t threads[HARNESS_MAX_THREADS];
    unsigned count;
} harness_group;

/* Start `count` SPU threads of `image` with args[i].  Returns 0, or -1
 * when the thread group could not be started. */
static inline int harness_start(harness_group *g, sysSpuImage *image, unsigned count,
                                const uint64_t args[][4])
{
    static int initialized;
    sysSpuThreadGroupAttribute grpattr = {
        .nsize = sizeof("sheapgrp"),
        .name  = "sheapgrp",
        .type  = 0,
    };
    sysSpuThreadAttribute attr = {
        .name   = "sheapthr",
        .nsize  = sizeof("sheapthr"),
        .option = SPU_THREAD_ATTR_NONE,
    };
    unsigned i;

    if (!initialized) {
        (void)sysSpuInitialize(HARNESS_MAX_THREADS, 0);   /* may already be set up */
        initialized = 1;
    }
    if (count == 0 || count > HARNESS_MAX_THREADS
            || sysSpuThreadGroupCreate(&g->group, count, 100, &grpattr) != 0)
        return -1;
    g->count = count;
    for (i = 0; i < count; ++i) {
        sysSpuThreadArgument arg = { args[i][0], args[i][1], args[i][2], args[i][3] };

        if (sysSpuThreadInitialize(&g->threads[i], g->group, i, image, &attr, &arg) != 0) {
            sysSpuThreadGroupDestroy(g->group);
            return -1;
        }
    }
    if (sysSpuThreadGroupStart(g->group) != 0) {
        sysSpuThreadGroupDestroy(g->group);
        return -1;
    }
    return 0;
}

/* Wait for a started group and collect each thread's exit status. */
static inline int harness_join(harness_group *g, int32_t *status)
{
    u32 cause = 0, group_status = 0;
    unsigned i;
    int rc = sysSpuThreadGroupJoin(g->group, &cause, &group_status) != 0 ? -1 : 0;

    for (i = 0; i < g->count; ++i) {
        s32 st = -1;

        if (rc != 0 || sysSpuThreadGetExitStatus(g->threads[i], &st) != 0)
            st = -1;
        status[i] = st;
    }
    sysSpuThreadGroupDestroy(g->group);
    return rc;
}

/* Start and join in one call. */
static inline int harness_run(sysSpuImage *image, unsigned count,
                              const uint64_t args[][4], int32_t *status)
{
    harness_group g;

    if (harness_start(&g, image, count, args) != 0)
        return -1;
    return harness_join(&g, status);
}

#endif /* SPU_SHEAP_HARNESS_H */
