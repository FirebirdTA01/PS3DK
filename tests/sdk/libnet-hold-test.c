/* libnet's process-wide network holders (sdk/libnet/src/net_hold.c) on the host, with the raw PRX calls and the
 * sysmodule load replaced by a model of the firmware's network: init only while it is down, finalize only while it
 * is up.  One case per process.  usage: libnet-hold-test --list | <case> */
#include "net_hold.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                                                    \
    do {                                                                                                            \
        if (!(c)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                            \
            exit(1);                                                                                                \
        }                                                                                                           \
    } while (0)

/* ---- the modelled firmware ---------------------------------------------------------------------------------------- */

struct wire_init { uint32_t memory, size; int32_t flags; };
#define DOUBLE_INIT (-0x7fffff01) /* the model's "already initialized" result (the firmware's is measured on target) */
static atomic_int net_up, module_loaded, inits, finals, loads, unloads, init_fail, final_fail, load_fail, protocol;
static atomic_uint last_size;

int __ps3dk_raw_sys_net_initialize_network_ex(const struct wire_init *w)
{
    atomic_fetch_add(&inits, 1);
    if (atomic_load(&init_fail))
        return atomic_load(&init_fail);
    if (atomic_exchange(&net_up, 1))
        return DOUBLE_INIT;
    atomic_store(&last_size, w->size);
    return 0;
}

int __ps3dk_raw_sys_net_finalize_network(void)
{
    atomic_fetch_add(&finals, 1);
    if (atomic_load(&final_fail))
        return atomic_load(&final_fail);
    if (!atomic_exchange(&net_up, 0))
        return -0x7fffff02; /* not initialized */
    return 0;
}

static int load(void)
{
    atomic_fetch_add(&loads, 1);
    if (atomic_load(&load_fail))
        return atomic_load(&load_fail);
    if (atomic_exchange(&module_loaded, 1))
        atomic_store(&protocol, 1); /* the model loads once; a second load is a protocol error here */
    return 0;
}

static int unload(void)
{
    atomic_fetch_add(&unloads, 1);
    if (!atomic_exchange(&module_loaded, 0))
        atomic_store(&protocol, 1); /* never unload a module that is not loaded */
    return 0;
}

/* The front ends, as sdk/libnet/src/legacy.c, init.c and the profiler call the layer. */
static int legacy_init(void)
{
    uint32_t bytes;
    void *pool = __ps3dk_net_default_pool(&bytes);
    CHECK(pool && bytes == 128u * 1024u);
    return __ps3dk_net_hold_acquire(PS3DK_NET_HOLD_LEGACY, pool, bytes, 0, load, unload);
}
static int legacy_deinit(void) { return __ps3dk_net_hold_release(PS3DK_NET_HOLD_LEGACY); }
static unsigned char cell_pool[256 * 1024], prof_pool[64 * 1024];
static int cell_init(uint32_t bytes) { return __ps3dk_net_hold_acquire(PS3DK_NET_HOLD_CELL, cell_pool, bytes, 0, 0, 0); }
static int cell_finalize(void) { return __ps3dk_net_hold_release(PS3DK_NET_HOLD_CELL); }
static int prof_init(void)
{
    return __ps3dk_net_hold_acquire(PS3DK_NET_HOLD_PROF, prof_pool, sizeof prof_pool, 0, load, unload);
}
static int prof_release(void) { return __ps3dk_net_hold_release(PS3DK_NET_HOLD_PROF); }

#define COUNTS(i, f, l, u)                                                                                          \
    CHECK(atomic_load(&inits) == (i) && atomic_load(&finals) == (f) && atomic_load(&loads) == (l) &&                \
          atomic_load(&unloads) == (u))

/* ---- one holder: exactly the sequence each front end made before holders existed ------------------------------- */

static void case_legacy_alone(void)
{
    atomic_store(&load_fail, -7);
    CHECK(legacy_init() == -7);
    COUNTS(0, 0, 1, 0);
    atomic_store(&load_fail, 0);
    atomic_store(&init_fail, -8);
    CHECK(legacy_init() == -8);
    COUNTS(1, 0, 2, 1);
    atomic_store(&init_fail, 0);
    CHECK(legacy_init() == 0 && legacy_init() == 0); /* idempotent */
    COUNTS(2, 0, 3, 1);
    CHECK(atomic_load(&last_size) == 128u * 1024u);
    atomic_store(&final_fail, -9);
    CHECK(legacy_deinit() == -9); /* a failed finalize keeps the holder */
    COUNTS(2, 1, 3, 1);
    atomic_store(&final_fail, 0);
    CHECK(legacy_deinit() == 0 && !atomic_load(&net_up));
    COUNTS(2, 2, 3, 2);
    CHECK(legacy_deinit() == 0); /* nothing held: no call */
    COUNTS(2, 2, 3, 2);
}

static void case_cell_alone(void)
{
    CHECK(cell_init(sizeof cell_pool) == 0);
    COUNTS(1, 0, 0, 0);
    CHECK(cell_init(sizeof cell_pool) == DOUBLE_INIT); /* a repeated Cell init reaches the raw call, as before */
    COUNTS(2, 0, 0, 0);
    CHECK(cell_finalize() == 0);
    COUNTS(2, 1, 0, 0);
    CHECK(cell_finalize() == -0x7fffff02); /* a finalize with no holder reaches the raw call, as before */
    COUNTS(2, 2, 0, 0);
    atomic_store(&init_fail, -5);
    CHECK(cell_init(sizeof cell_pool) == -5); /* a failed init holds nothing */
    atomic_store(&init_fail, 0);
    CHECK(cell_finalize() == -0x7fffff02);
    COUNTS(3, 3, 0, 0);
}

static void case_prof_alone(void)
{
    CHECK(prof_init() == 0 && prof_init() == 0);
    COUNTS(1, 0, 1, 0);
    CHECK(atomic_load(&last_size) == sizeof prof_pool);
    CHECK(prof_release() == 0 && !atomic_load(&net_up) && !atomic_load(&module_loaded));
    COUNTS(1, 1, 1, 1);
    CHECK(prof_release() == 0);
    COUNTS(1, 1, 1, 1);
}

/* ---- several holders ------------------------------------------------------------------------------------------- */

/* The app (or the ps3dk-run hook) brought the network up first; the profiler joins and leaves without touching it. */
static void case_legacy_then_prof(void)
{
    CHECK(legacy_init() == 0);
    CHECK(prof_init() == 0);
    COUNTS(1, 0, 1, 0);
    CHECK(prof_release() == 0 && atomic_load(&net_up)); /* the hook's network survives the profiler */
    COUNTS(1, 0, 1, 0);
    CHECK(legacy_deinit() == 0 && !atomic_load(&net_up));
    COUNTS(1, 1, 1, 1);
}

/* The profiler first; the app's netInitialize joins; the app's netDeinitialize no longer pulls the network from
 * under the agent; the profiler's release finalizes it and unloads the module the profiler loaded. */
static void case_prof_then_legacy(void)
{
    CHECK(prof_init() == 0);
    CHECK(legacy_init() == 0);
    COUNTS(1, 0, 1, 0);
    CHECK(legacy_deinit() == 0 && atomic_load(&net_up));
    COUNTS(1, 0, 1, 0);
    CHECK(prof_release() == 0 && !atomic_load(&net_up) && !atomic_load(&module_loaded));
    COUNTS(1, 1, 1, 1);
}

/* Prof first, then a Cell-style app with its own larger pool: it joins (the profiler's pool stays in use, one TTY
 * line), its finalize leaves the network up, and its re-init with a different pool joins again (the documented edge). */
static void case_prof_then_cell_pool(void)
{
    CHECK(prof_init() == 0);
    CHECK(cell_init(sizeof cell_pool) == 0); /* larger than the profiler's pool: warned, joined */
    COUNTS(1, 0, 1, 0);
    CHECK(atomic_load(&last_size) == sizeof prof_pool);
    CHECK(cell_finalize() == 0 && atomic_load(&net_up));
    CHECK(cell_init(16 * 1024) == 0); /* re-init, different pool: still the profiler's network */
    COUNTS(1, 0, 1, 0);
    CHECK(prof_release() == 0 && atomic_load(&net_up)); /* the Cell holder remains */
    CHECK(cell_finalize() == 0 && !atomic_load(&net_up));
    COUNTS(1, 1, 1, 1); /* the module the profiler loaded is unloaded with the last release */
}

/* A Cell app first (it loaded the module itself): the profiler joins and leaves; nothing of the app's is unloaded. */
static void case_cell_then_prof(void)
{
    CHECK(cell_init(sizeof cell_pool) == 0);
    CHECK(prof_init() == 0);
    CHECK(cell_finalize() == 0 && atomic_load(&net_up));
    CHECK(prof_release() == 0 && !atomic_load(&net_up));
    COUNTS(1, 1, 0, 0);
}

/* A Cell finalize from an app that never initialized must not finalize a network the profiler holds. */
static void case_stray_cell_finalize(void)
{
    CHECK(prof_init() == 0);
    CHECK(cell_finalize() == 0 && atomic_load(&net_up));
    COUNTS(1, 0, 1, 0);
    CHECK(prof_release() == 0);
    COUNTS(1, 1, 1, 1);
}

/* Someone outside this layer brought the network up: the profiler's first init fails with the firmware's result and
 * holds nothing (the auto-start then stays off). */
static void case_foreign_network(void)
{
    atomic_store(&net_up, 1);
    CHECK(prof_init() == DOUBLE_INIT);
    COUNTS(1, 0, 1, 1); /* loaded, failed, unloaded: as netInitialize has always done */
    CHECK(prof_release() == 0);
    COUNTS(1, 0, 1, 1);
    CHECK(atomic_load(&net_up)); /* never finalized */
}

/* The last holder's finalize fails: it stays a holder and may retry. */
static void case_last_finalize_fails(void)
{
    CHECK(legacy_init() == 0 && prof_init() == 0);
    CHECK(legacy_deinit() == 0);
    atomic_store(&final_fail, -3);
    CHECK(prof_release() == -3 && atomic_load(&net_up));
    atomic_store(&final_fail, 0);
    CHECK(prof_release() == 0 && !atomic_load(&net_up));
    COUNTS(1, 2, 1, 1);
}

/* ---- concurrency: three holders in loops; the model checks every raw call happens in a legal state -------------- */

static atomic_int go;
static void *loop(void *arg)
{
    int which = (int)(intptr_t)arg;
    while (!atomic_load(&go))
        ;
    for (int i = 0; i < 20000; ++i) {
        if (which == 0) {
            CHECK(legacy_init() == 0);
            CHECK(legacy_deinit() == 0);
        } else if (which == 1) {
            CHECK(prof_init() == 0);
            CHECK(prof_release() == 0);
        } else {
            CHECK(cell_init(sizeof cell_pool) == 0);
            CHECK(cell_finalize() == 0);
        }
    }
    return NULL;
}

static void case_concurrent(void)
{
    pthread_t t[3];
    for (int i = 0; i < 3; ++i)
        CHECK(pthread_create(&t[i], NULL, loop, (void *)(intptr_t)i) == 0);
    atomic_store(&go, 1);
    for (int i = 0; i < 3; ++i)
        CHECK(pthread_join(t[i], NULL) == 0);
    CHECK(!atomic_load(&net_up) && !atomic_load(&module_loaded) && !atomic_load(&protocol));
    CHECK(atomic_load(&inits) == atomic_load(&finals));
    printf("MEASURE concurrent inits=%d loads=%d\n", atomic_load(&inits), atomic_load(&loads));
}

static const struct {
    const char *name;
    void (*fn)(void);
} cases[] = {
    {"legacy_alone", case_legacy_alone},
    {"cell_alone", case_cell_alone},
    {"prof_alone", case_prof_alone},
    {"legacy_then_prof", case_legacy_then_prof},
    {"prof_then_legacy", case_prof_then_legacy},
    {"prof_then_cell_pool", case_prof_then_cell_pool},
    {"cell_then_prof", case_cell_then_prof},
    {"stray_cell_finalize", case_stray_cell_finalize},
    {"foreign_network", case_foreign_network},
    {"last_finalize_fails", case_last_finalize_fails},
    {"concurrent", case_concurrent},
};

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
            puts(cases[i].name);
        return 0;
    }
    for (size_t i = 0; argc == 2 && i < sizeof cases / sizeof cases[0]; ++i)
        if (!strcmp(argv[1], cases[i].name)) {
            cases[i].fn();
            CHECK(!atomic_load(&protocol));
            printf("PASS %s\n", cases[i].name);
            return 0;
        }
    fprintf(stderr, "usage: %s --list | <case>\n", argv[0]);
    return 2;
}
