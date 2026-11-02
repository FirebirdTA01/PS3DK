/* P3.2 integration slice I1: init_ex configuration, the init-block layout, failed-init unwind and the no-allocation
 * window, against the real runtime built with PS3DK_PROF_WITH_SPU and the host PAL.  One case per process (init is
 * one-shot).  Allocation instrumentation: the link wraps malloc, calloc, realloc, free, posix_memalign and
 * aligned_alloc (-Wl,--wrap=...), which also covers pal_alloc; the "alloc-control" case proves a post-init allocation
 * in the window is caught.
 *
 * usage: i1 <case> <out-dir>; prints "I1 <case> PASS" or "I1 <case> FAIL ..." and LAYOUT lines for the driver. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ps3dk/prof.h>

#include "prof_spu.h"
#include "prof_test_hooks.h"
#include "psp_ppu.h"

size_t ps3dk_prof__test_spu_bytes(void);
size_t ps3dk_prof__test_block_bytes(void);
unsigned ps3dk_prof__test_ext_flags(void);

/* ---- allocation instrumentation ---- */
static atomic_int g_counting;
static atomic_long g_allocs;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
int __real_posix_memalign(void **, size_t, size_t);
void *__real_aligned_alloc(size_t, size_t);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t a, size_t b);
void *__wrap_realloc(void *p, size_t n);
void __wrap_free(void *p);
int __wrap_posix_memalign(void **p, size_t a, size_t n);
void *__wrap_aligned_alloc(size_t a, size_t n);
static void note(void)
{
    if (atomic_load(&g_counting))
        atomic_fetch_add(&g_allocs, 1);
}
void *__wrap_malloc(size_t n) { note(); return __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { note(); return __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { note(); return __real_realloc(p, n); }
void __wrap_free(void *p) { note(); __real_free(p); }
int __wrap_posix_memalign(void **p, size_t a, size_t n) { note(); return __real_posix_memalign(p, a, n); }
void *__wrap_aligned_alloc(size_t a, size_t n) { note(); return __real_aligned_alloc(a, n); }

static const char *g_case, *g_out;
static int fail(const char *why, long a, long b)
{
    printf("I1 %s FAIL %s (%ld, want %ld)\n", g_case, why, a, b);
    return 1;
}

static void base_cfg(ps3dk_prof_config *c, uint32_t mode)
{
    ps3dk_prof_config_defaults(c);
    c->mode = mode;
    c->app = "i1";
    c->build_id = "i1";
}

static ps3dk_prof_ext_config ext_on(uint32_t lanes)
{
    ps3dk_prof_ext_config x;
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.spu_lanes_max = lanes;
    return x;
}

static size_t a128(size_t n) { return (n + 127) & ~(size_t)127; }

/* the documented extension subtotal for (lanes, ring, image), with the default max_threads (64): the I2 owner
 * record (16 bytes per v1 slot) is part of it */
static size_t expected_ext(uint32_t lanes, uint32_t ring, uint32_t image)
{
    return a128(sizeof(psp_ppu)) + a128(PSP_SCRATCH_BYTES) + a128((size_t)ring * 8) + a128(image) +
           a128((size_t)lanes * ((size_t)ring * 8 + 256)) + a128((size_t)64 * 16);
}

static int sink_dir(void) { return ps3dk_prof_set_sink_path(g_out); }

/* a short v1 workload, APP or THREAD */
static int workload(uint32_t mode)
{
    if (ps3dk_prof_thread_begin("main") != PS3DK_PROF_OK)
        return -1;
    for (int i = 0; i < 4; i++) {
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("w");
        ps3dk_prof_frame();
        PS3DK_PROF_ZONE_END(z);
        if (mode == PS3DK_PROF_MODE_APP && ps3dk_prof_drain() != PS3DK_PROF_OK)
            return -2;
    }
    ps3dk_prof_thread_end();
    return 0;
}

/* expect EINVAL for x, then prove nothing changed: a valid enabled init succeeds in the same process */
static int refused_then_ok(ps3dk_prof_ext_config x)
{
    ps3dk_prof_config c;
    ps3dk_prof_ext_config ok = ext_on(2);
    int rc;
    base_cfg(&c, PS3DK_PROF_MODE_APP);
    if (sink_dir() != 0)
        return fail("sink", 0, 0);
    rc = ps3dk_prof_init_ex(&c, &x);
    if (rc != PS3DK_PROF_EINVAL)
        return fail("refusal", rc, PS3DK_PROF_EINVAL);
    rc = ps3dk_prof_init_ex(&c, &ok);
    if (rc != PS3DK_PROF_OK)
        return fail("retry", rc, 0);
    ps3dk_prof_shutdown();
    printf("I1 %s PASS\n", g_case);
    return 0;
}

int main(int argc, char **argv)
{
    ps3dk_prof_config c;
    ps3dk_prof_ext_config x = ext_on(4);
    int rc;
    if (argc != 3)
        return 2;
    g_case = argv[1];
    g_out = argv[2];
    base_cfg(&c, PS3DK_PROF_MODE_APP);

    /* ---- refusals: each followed by a successful retry in the same process ---- */
    if (!strcmp(g_case, "size-39")) { x.size = 39; return refused_then_ok(x); }
    if (!strcmp(g_case, "size-41")) { x.size = 41; return refused_then_ok(x); }
    if (!strcmp(g_case, "size-0")) { x.size = 0; return refused_then_ok(x); }
    if (!strncmp(g_case, "reserved-", 9)) { x.reserved[g_case[9] - '0'] = 1; return refused_then_ok(x); }
    if (!strcmp(g_case, "disabled-ring")) { x = ext_on(0); x.spu_ring_slots = 64; return refused_then_ok(x); }
    if (!strcmp(g_case, "disabled-image")) { x = ext_on(0); x.spu_image_bytes_max = 4096; return refused_then_ok(x); }
    if (!strcmp(g_case, "disabled-mem")) { x = ext_on(0); x.spu_mem_max = 1; return refused_then_ok(x); }
    if (!strcmp(g_case, "lanes-65")) { x = ext_on(65); return refused_then_ok(x); }
    if (!strcmp(g_case, "ring-32")) { x.spu_ring_slots = 32; return refused_then_ok(x); }
    if (!strcmp(g_case, "ring-not-pow2")) { x.spu_ring_slots = 100; return refused_then_ok(x); }
    if (!strcmp(g_case, "ring-2m")) { x.spu_ring_slots = 2097152; return refused_then_ok(x); }
    if (!strcmp(g_case, "image-4095")) { x.spu_image_bytes_max = 4095; return refused_then_ok(x); }
    if (!strcmp(g_case, "image-too-big")) { x.spu_image_bytes_max = 16777217; return refused_then_ok(x); }
    if (!strcmp(g_case, "cap-ext-small")) { x.spu_mem_max = 1048576; return refused_then_ok(x); }
    if (!strcmp(g_case, "cap-total")) { /* 64 x 8 MiB rings: the whole block passes P_TOTAL_MAX */
        x = ext_on(64);
        x.spu_ring_slots = 1048576;
        x.spu_mem_max = UINT64_MAX;
        return refused_then_ok(x);
    }
    if (!strcmp(g_case, "cap-ext-minus-1")) {
        x = ext_on(1);
        x.spu_mem_max = expected_ext(1, 8192, 65536) - 1;
        return refused_then_ok(x);
    }

    /* ---- acceptances with measured layout ---- */
    if (!strcmp(g_case, "cap-ext-exact") || !strcmp(g_case, "defaults") || !strcmp(g_case, "disabled-layout") ||
        !strcmp(g_case, "null-layout")) {
        uint32_t lanes = !strcmp(g_case, "defaults") ? 4 : 1;
        if (sink_dir() != 0)
            return fail("sink", 0, 0);
        if (!strcmp(g_case, "cap-ext-exact")) {
            x = ext_on(1);
            x.spu_mem_max = expected_ext(1, 8192, 65536);
        } else if (!strcmp(g_case, "defaults")) {
            x = ext_on(4);
        } else {
            x = ext_on(0); /* disabled: size set, every other field 0 */
        }
        rc = !strcmp(g_case, "null-layout") ? ps3dk_prof_init_ex(&c, NULL) : ps3dk_prof_init_ex(&c, &x);
        if (rc != PS3DK_PROF_OK)
            return fail("init", rc, 0);
        printf("LAYOUT %s block %zu spu %zu flags %u\n", g_case, ps3dk_prof__test_block_bytes(),
               ps3dk_prof__test_spu_bytes(), ps3dk_prof__test_ext_flags());
        if (x.spu_lanes_max && strcmp(g_case, "null-layout")) {
            size_t want = expected_ext(lanes, 8192, 65536);
            if (ps3dk_prof__test_spu_bytes() != want)
                return fail("extension subtotal", (long)ps3dk_prof__test_spu_bytes(), (long)want);
            if (ps3dk_prof__test_ext_flags() != 1)
                return fail("flags", (long)ps3dk_prof__test_ext_flags(), 1);
        } else if (ps3dk_prof__test_spu_bytes() != 0 || ps3dk_prof__test_ext_flags() != 0) {
            return fail("disabled has extension state", (long)ps3dk_prof__test_spu_bytes(), 0);
        }
        ps3dk_prof_shutdown();
        printf("I1 %s PASS\n", g_case);
        return 0;
    }

    /* ---- captures: enabled (an empty extension file) and disabled (a v1 file) ---- */
    if (!strcmp(g_case, "enabled-capture") || !strcmp(g_case, "disabled-capture")) {
        if (!strcmp(g_case, "disabled-capture"))
            x = ext_on(0);
        if (sink_dir() != 0)
            return fail("sink", 0, 0);
        rc = ps3dk_prof_init_ex(&c, &x);
        if (rc != PS3DK_PROF_OK || workload(PS3DK_PROF_MODE_APP) != 0)
            return fail("init/workload", rc, 0);
        rc = ps3dk_prof_shutdown();
        if (rc != PS3DK_PROF_OK)
            return fail("shutdown", rc, 0);
        printf("I1 %s PASS\n", g_case);
        return 0;
    }

    /* ---- failed init: unwind, then retry ---- */
    if (!strcmp(g_case, "unwind-alloc") || !strcmp(g_case, "unwind-clock")) {
        int want = !strcmp(g_case, "unwind-alloc") ? PS3DK_PROF_ENOMEM : PS3DK_PROF_ECLOCK;
        if (sink_dir() != 0)
            return fail("sink", 0, 0);
        if (want == PS3DK_PROF_ENOMEM)
            ps3dk_prof_test_hooks.alloc_fail = 1;
        else
            ps3dk_prof_test_hooks.mono_clock_fail = 1; /* after the block exists: the unwind must free it */
        rc = ps3dk_prof_init_ex(&c, &x);
        ps3dk_prof_test_hooks.alloc_fail = 0;
        ps3dk_prof_test_hooks.mono_clock_fail = 0;
        if (rc != want)
            return fail("first init", rc, want);
        if (sink_dir() != 0)
            return fail("sink again", 0, 0);
        rc = ps3dk_prof_init_ex(&c, &x);
        if (rc != PS3DK_PROF_OK)
            return fail("retry", rc, 0);
        if (ps3dk_prof__test_ext_flags() != 1 || ps3dk_prof__test_spu_bytes() != expected_ext(4, 8192, 65536))
            return fail("retry layout", (long)ps3dk_prof__test_spu_bytes(), (long)expected_ext(4, 8192, 65536));
        ps3dk_prof_shutdown();
        printf("I1 %s PASS\n", g_case);
        return 0;
    }

    /* ---- the allocation window: from a successful init through shutdown, in APP and THREAD mode ---- */
    if (!strcmp(g_case, "alloc-app") || !strcmp(g_case, "alloc-thread") || !strcmp(g_case, "alloc-control")) {
        uint32_t mode = !strcmp(g_case, "alloc-thread") ? PS3DK_PROF_MODE_THREAD : PS3DK_PROF_MODE_APP;
        long n;
        base_cfg(&c, mode);
        if (sink_dir() != 0)
            return fail("sink", 0, 0);
        rc = ps3dk_prof_init_ex(&c, &x);
        if (rc != PS3DK_PROF_OK)
            return fail("init", rc, 0);
        atomic_store(&g_counting, 1);
        if (!strcmp(g_case, "alloc-control")) {
            /* the positive control: a direct allocation inside the window.  The volatile pointer keeps the compiler
             * from eliding the malloc/free pair (it did, at -O1, in the first run of this gate). */
            void *volatile q = malloc(16);
            free(q);
        }
        if (workload(mode) != 0)
            return fail("workload", 0, 0);
        rc = ps3dk_prof_shutdown();
        atomic_store(&g_counting, 0);
        n = atomic_load(&g_allocs);
        printf("ALLOCS %s %ld\n", g_case, n);
        if (rc != PS3DK_PROF_OK)
            return fail("shutdown", rc, 0);
        if (!strcmp(g_case, "alloc-control") ? n < 2 : n != 0)
            return fail("allocation count", n, !strcmp(g_case, "alloc-control") ? 2 : 0);
        printf("I1 %s PASS\n", g_case);
        return 0;
    }
    printf("I1 %s FAIL unknown case\n", g_case);
    return 1;
}
