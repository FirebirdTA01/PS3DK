/* libnet's network holders (sdk/libnet/src/net_hold.c) on the host, with the raw PRX calls and the sysmodule load
 * replaced by a model of the firmware: init only while the network is down, finalize only while it is up, a counted
 * module load, and the network's pool written by the "firmware" while it is up (so a freed pool is an ASan report).
 * One case per process.  usage: libnet-hold-test --list | <case> */
#define _GNU_SOURCE
#include "net_hold.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define CHECK(c)                                                                                                    \
    do {                                                                                                            \
        if (!(c)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                            \
            exit(1);                                                                                                \
        }                                                                                                           \
    } while (0)

/* ---- the modelled firmware ---------------------------------------------------------------------------------------- */

struct wire_init { uint32_t memory, size; int32_t flags; };
#define DOUBLE_INIT (-0x7fffff01) /* the model's "already initialized" (the firmware's is measured on target) */
#define NOT_INIT (-0x7fffff02)
static atomic_int net_up, modules, inits, finals, loads, unloads, init_fail, final_fail, load_fail, protocol;
/* The firmware as measured on HEN and in RPCS3: a second initialize while up answers 0 (no refusal). */
static atomic_int double_ok;
static atomic_uint last_size;
static unsigned char *volatile active_pool; /* the pool the "firmware" uses while up */

static void firmware_touch(void)
{
    unsigned char *p = active_pool;
    if (p)
        p[0]++, p[127]++; /* the firmware works in the pool it was given */
}

int __ps3dk_raw_sys_net_initialize_network_ex(const struct wire_init *w)
{
    atomic_fetch_add(&inits, 1);
    if (atomic_load(&init_fail))
        return atomic_load(&init_fail);
    if (!atomic_load(&modules))
        atomic_store(&protocol, 1); /* sys_net called with the NET module unloaded */
    if (atomic_exchange(&net_up, 1))
        return atomic_load(&double_ok) ? 0 : DOUBLE_INIT;
    atomic_store(&last_size, w->size);
    active_pool = (unsigned char *)(uintptr_t)w->memory;
    firmware_touch();
    return 0;
}

int __ps3dk_raw_sys_net_finalize_network(void)
{
    atomic_fetch_add(&finals, 1);
    if (atomic_load(&final_fail))
        return atomic_load(&final_fail);
    if (!atomic_load(&modules))
        atomic_store(&protocol, 1);
    if (!atomic_load(&net_up))
        return NOT_INIT;
    firmware_touch();
    active_pool = NULL;
    atomic_store(&net_up, 0);
    return 0;
}

static int load(void)
{
    atomic_fetch_add(&loads, 1);
    if (atomic_load(&load_fail))
        return atomic_load(&load_fail);
    atomic_fetch_add(&modules, 1);
    return 0;
}

static int is_loaded(void) { return atomic_load(&modules) > 0; }
/* The profiler's probe (a socket created and closed): up when a network is up, 0 when it is not initialized, or the
 * error the test injects. */
static atomic_int probe_error, probes;
static int probe_up(void)
{
    atomic_fetch_add(&probes, 1);
    if (atomic_load(&probe_error))
        return atomic_load(&probe_error);
    return atomic_load(&net_up) ? 1 : 0;
}

static int unload(void)
{
    atomic_fetch_add(&unloads, 1);
    if (atomic_fetch_sub(&modules, 1) <= 0)
        atomic_store(&protocol, 1); /* an unload with no load counted */
    return 0;
}

/* ---- the front ends and a stand-in profiler --------------------------------------------------------------------- */

static int legacy_init(void)
{
    uint32_t bytes;
    void *pool = __ps3dk_net_default_pool(&bytes);
    CHECK(pool && bytes == 128u * 1024u);
    return __ps3dk_net_legacy_init(pool, bytes, load, unload);
}
static int legacy_deinit(void) { return __ps3dk_net_legacy_deinit(); }
static unsigned char static_cell_pool[256 * 1024];
/* An app's own pool below 4 GiB (the layer refuses a higher one, as the console never has one); unmapping it is the
 * app's free, so a later firmware touch faults the test. */
static unsigned char *app_pool(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(p != MAP_FAILED);
    return p;
}
static int cell_init_pool(void *pool, uint32_t bytes) { return __ps3dk_net_cell_init(pool, bytes, 0); }
static int cell_init(void) { return cell_init_pool(static_cell_pool, sizeof static_cell_pool); }
static int cell_finalize(void) { return __ps3dk_net_cell_finalize(); }
/* A Cell-style app loads the NET module itself, around its own init/finalize. */
static void cell_app_load(void) { CHECK(load() == 0); }
static void cell_app_unload(void) { CHECK(unload() == 0); }

static unsigned char prof_pool[64 * 1024];
static atomic_int evicts, agent_running, net_up_at_evict;
/* The profiler's own lock (live_api.c's live lock): its bring-up and its eviction are serialized on it. */
static pthread_mutex_t prof_lock = PTHREAD_MUTEX_INITIALIZER;
static void prof_evict(void)
{
    pthread_mutex_lock(&prof_lock);
    atomic_fetch_add(&evicts, 1);
    atomic_store(&net_up_at_evict, atomic_load(&net_up)); /* told BEFORE the teardown */
    atomic_store(&agent_running, 0);
    CHECK(__ps3dk_net_prof_release() == 0);
    pthread_mutex_unlock(&prof_lock);
}
static int prof_acquire(void)
{
    pthread_mutex_lock(&prof_lock);
    int rc = __ps3dk_net_prof_acquire(prof_pool, sizeof prof_pool, load, unload, is_loaded, probe_up, prof_evict);
    if (!rc)
        atomic_store(&agent_running, 1);
    pthread_mutex_unlock(&prof_lock);
    return rc;
}
static int prof_release(void)
{
    pthread_mutex_lock(&prof_lock);
    atomic_store(&agent_running, 0);
    int rc = __ps3dk_net_prof_release();
    pthread_mutex_unlock(&prof_lock);
    return rc;
}
/* The agent's socket use: only while it runs (under the profiler's lock), and the network must be up then. */
static void agent_uses_network(void)
{
    pthread_mutex_lock(&prof_lock);
    if (atomic_load(&agent_running)) {
        CHECK(atomic_load(&net_up));
        firmware_touch();
    }
    pthread_mutex_unlock(&prof_lock);
}

#define COUNTS(i, f, l, u)                                                                                          \
    CHECK(atomic_load(&inits) == (i) && atomic_load(&finals) == (f) && atomic_load(&loads) == (l) &&                \
          atomic_load(&unloads) == (u))

/* ---- no profiler: every app call exactly as before holders existed ---------------------------------------------- */

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
    CHECK(legacy_deinit() == -9); /* a failed finalize keeps it up */
    atomic_store(&final_fail, 0);
    CHECK(legacy_deinit() == 0 && !atomic_load(&net_up));
    COUNTS(2, 2, 3, 2);
    CHECK(legacy_deinit() == 0);
    COUNTS(2, 2, 3, 2);
}

static void case_cell_alone(void)
{
    cell_app_load();
    CHECK(cell_init() == 0);
    CHECK(cell_init() == DOUBLE_INIT); /* straight to the firmware, as before */
    CHECK(cell_finalize() == 0);
    CHECK(cell_finalize() == NOT_INIT);
    COUNTS(2, 2, 1, 0);
    cell_app_unload();
}

/* The two front ends mixed, no profiler: the results they gave before (each reaches the firmware on its own). */
static void case_mixed_cell_then_legacy(void)
{
    cell_app_load();
    CHECK(cell_init() == 0);
    CHECK(legacy_init() == DOUBLE_INIT); /* load, init refused, unload: netInitialize's own sequence */
    COUNTS(2, 0, 2, 1);
    CHECK(legacy_deinit() == 0); /* never initialized: no call */
    CHECK(cell_finalize() == 0);
    COUNTS(2, 1, 2, 1);
    cell_app_unload();
}

static void case_mixed_legacy_then_cell(void)
{
    CHECK(legacy_init() == 0);
    CHECK(cell_init() == DOUBLE_INIT);
    CHECK(cell_finalize() == 0 && !atomic_load(&net_up)); /* a Cell finalize tears it down, as before */
    CHECK(legacy_deinit() == NOT_INIT);                   /* and netDeinitialize then gets the firmware's refusal */
    COUNTS(2, 2, 1, 0);
    CHECK(legacy_deinit() == NOT_INIT); /* and keeps getting it, as before: its flag stays set */
}

/* ---- the profiler ---------------------------------------------------------------------------------------------- */

static void case_prof_alone(void)
{
    CHECK(prof_acquire() == 0 && prof_acquire() == 0);
    COUNTS(1, 0, 1, 0);
    CHECK(atomic_load(&last_size) == sizeof prof_pool);
    agent_uses_network();
    CHECK(prof_release() == 0 && !atomic_load(&net_up) && !atomic_load(&modules));
    COUNTS(1, 1, 1, 1);
    CHECK(prof_release() == 0);
    COUNTS(1, 1, 1, 1);
}

/* netInitialize first (an app or the ps3dk-run hook): the profiler joins and leaves without a raw call. */
static void case_legacy_then_prof(void)
{
    CHECK(legacy_init() == 0);
    CHECK(prof_acquire() == 0);
    COUNTS(1, 0, 1, 0);
    agent_uses_network();
    CHECK(prof_release() == 0 && atomic_load(&net_up)); /* the hook's network survives the profiler */
    CHECK(atomic_load(&evicts) == 0);
    CHECK(legacy_deinit() == 0 && !atomic_load(&net_up));
    COUNTS(1, 1, 1, 1);
}

/* The network is netInitialize's: its netDeinitialize is never swallowed; the profiler is told first. */
static void case_legacy_owner_deinit_evicts(void)
{
    CHECK(legacy_init() == 0);
    CHECK(prof_acquire() == 0);
    CHECK(legacy_deinit() == 0 && !atomic_load(&net_up));
    CHECK(atomic_load(&evicts) == 1 && atomic_load(&net_up_at_evict) == 1);
    COUNTS(1, 1, 1, 1);
    CHECK(prof_release() == 0); /* nothing left to release: no raw call */
    COUNTS(1, 1, 1, 1);
}

/* The profiler first: netInitialize joins its static pool; netDeinitialize leaves the profiler's network up. */
static void case_prof_then_legacy(void)
{
    CHECK(prof_acquire() == 0);
    CHECK(legacy_init() == 0);
    COUNTS(1, 0, 1, 0);
    CHECK(legacy_deinit() == 0 && atomic_load(&net_up));
    agent_uses_network();
    CHECK(prof_release() == 0 && !atomic_load(&net_up) && !atomic_load(&modules));
    COUNTS(1, 1, 1, 1);
    CHECK(atomic_load(&evicts) == 0);
}

/* The profiler first, netInitialize joins, the profiler leaves first: the network is then netDeinitialize's. */
static void case_prof_leaves_first(void)
{
    CHECK(prof_acquire() == 0);
    CHECK(legacy_init() == 0);
    CHECK(prof_release() == 0 && atomic_load(&net_up));
    COUNTS(1, 0, 1, 0);
    CHECK(legacy_deinit() == 0 && !atomic_load(&net_up) && !atomic_load(&modules));
    COUNTS(1, 1, 1, 1);
}

/* A Cell app owns its pool: init, the profiler joins, the app's finalize evicts the profiler first and reaches the
 * firmware, the app frees its pool and unloads NET itself; the profiler never touches the network again. */
static void case_cell_pool_owner(void)
{
    unsigned char *pool = app_pool(128 * 1024);
    cell_app_load();
    CHECK(cell_init_pool(pool, 128 * 1024) == 0);
    CHECK(prof_acquire() == 0);
    COUNTS(1, 0, 1, 0);
    agent_uses_network();
    CHECK(cell_finalize() == 0 && !atomic_load(&net_up)); /* not swallowed */
    CHECK(atomic_load(&evicts) == 1 && atomic_load(&net_up_at_evict) == 1);
    CHECK(munmap(pool, 128 * 1024) == 0); /* the app's own pool goes */
    cell_app_unload(); /* and the app unloads NET itself: the layer never sees it */
    agent_uses_network();      /* the stopped agent does nothing (an ASan report otherwise) */
    CHECK(prof_release() == 0); /* no raw call after an unload it did not count */
    COUNTS(1, 1, 1, 1);
    CHECK(!atomic_load(&modules));
}

/* The profiler first (its static pool), then a Cell app with its own pool: the profiler steps aside (evicted, its
 * network finalized, NET left loaded), the app's init/finalize reach the firmware as before. */
static void case_prof_then_cell(void)
{
    unsigned char *pool = app_pool(96 * 1024);
    CHECK(prof_acquire() == 0);
    cell_app_load();
    CHECK(cell_init_pool(pool, 96 * 1024) == 0);
    CHECK(atomic_load(&evicts) == 1 && atomic_load(&last_size) == 96u * 1024u);
    COUNTS(2, 1, 2, 0); /* the profiler's network finalized, the app's brought up */
    agent_uses_network();
    CHECK(cell_init_pool(pool, 96 * 1024) == DOUBLE_INIT); /* the app's repeated init: the firmware's answer */
    CHECK(cell_finalize() == 0 && !atomic_load(&net_up));
    CHECK(munmap(pool, 96 * 1024) == 0);
    cell_app_unload();
    CHECK(prof_release() == 0);
    COUNTS(3, 2, 2, 1);
    CHECK(atomic_load(&modules) == 1); /* the profiler's load stays counted: never unloaded on the app's behalf */
}

/* A sys_net_finalize_network from an app that never initialized: straight to the firmware with the result the same
 * sequence gives without the profiler, and the profiler is not evicted (its hold and module count stay). */
static void case_stray_cell_finalize_no_prof(void)
{
    /* the baseline: netInitialize, a stray Cell finalize, netDeinitialize, without the profiler */
    CHECK(legacy_init() == 0);
    CHECK(cell_finalize() == 0);
    CHECK(legacy_deinit() == NOT_INIT);
    COUNTS(1, 2, 1, 0);
}

static void case_stray_cell_finalize_prof_joined(void)
{
    CHECK(legacy_init() == 0);
    CHECK(prof_acquire() == 0);
    CHECK(cell_finalize() == 0);         /* the same result as without the profiler */
    CHECK(atomic_load(&evicts) == 0);    /* not evicted */
    CHECK(legacy_deinit() == NOT_INIT);  /* and the same result as without the profiler */
    CHECK(prof_release() == 0);          /* the joined hold: dropped without a raw call */
    COUNTS(1, 2, 1, 0);
}

static void case_stray_cell_finalize_prof_owner(void)
{
    CHECK(prof_acquire() == 0);
    CHECK(cell_finalize() == 0);      /* passed straight through */
    CHECK(atomic_load(&evicts) == 0); /* the profiler keeps its hold and its module count */
    CHECK(prof_release() == NOT_INIT); /* its own finalize then meets the firmware's refusal: hold kept, no unload */
    COUNTS(1, 2, 1, 0);
    CHECK(atomic_load(&modules) == 1);
}

/* netInitialize joined the profiler's network, then a Cell init: the results are those of the same sequence without
 * the profiler (netInitialize 0, Cell init refused as a double init, netDeinitialize 0 with NET unloaded). */
static void case_legacy_joined_then_cell(void)
{
    CHECK(prof_acquire() == 0);
    CHECK(legacy_init() == 0);
    cell_app_load();
    CHECK(cell_init() == DOUBLE_INIT); /* as without the profiler: netInitialize's network is up */
    CHECK(atomic_load(&evicts) == 1);  /* the profiler let go; the network is now netInitialize's */
    CHECK(legacy_deinit() == 0 && !atomic_load(&net_up));
    cell_app_unload();
    CHECK(prof_release() == 0);
    COUNTS(2, 1, 2, 2);
    CHECK(atomic_load(&modules) == 0);
}

static void case_legacy_then_cell_no_prof(void)
{
    CHECK(legacy_init() == 0);
    cell_app_load();
    CHECK(cell_init() == DOUBLE_INIT);
    CHECK(legacy_deinit() == 0 && !atomic_load(&net_up));
    cell_app_unload();
    COUNTS(2, 1, 2, 2);
    CHECK(atomic_load(&modules) == 0);
}

/* Someone outside libnet brought the network up (NET loaded, no libnet record): the profiler assumes it is foreign. */
static void foreign(int firmware_double_ok)
{
    atomic_store(&double_ok, firmware_double_ok);
    atomic_store(&net_up, 1); /* brought up outside libnet: no front end's record */
    atomic_store(&modules, 1);
    CHECK(prof_acquire() == 0); /* joined: the NET module was loaded and the probe found a network up */
    CHECK(atomic_load(&probes) == 1);
    COUNTS(0, 0, 0, 0);         /* no raw call at all: ownership is never read from an init's result */
    CHECK(prof_release() == 0 && atomic_load(&net_up) && atomic_load(&modules) == 1);
    COUNTS(0, 0, 0, 0);         /* never finalized, nothing unloaded */
}
static void case_foreign_network(void) { foreign(0); }

/* NET loaded early by the app (or a library) but no network initialized yet: the probe says so, and the profiler
 * brings the network up as its owner without loading the module, so it finalizes its own network and never unloads
 * the module someone else loaded. */
static void case_loaded_but_down(void)
{
    atomic_store(&double_ok, 1);
    atomic_store(&modules, 1); /* the app's early cellSysmoduleLoadModule */
    CHECK(prof_acquire() == 0);
    CHECK(atomic_load(&probes) == 1 && atomic_load(&net_up));
    COUNTS(1, 0, 0, 0); /* one raw init, no load */
    CHECK(prof_release() == 0 && !atomic_load(&net_up));
    COUNTS(1, 1, 0, 0); /* its own network finalized, nothing unloaded */
    CHECK(atomic_load(&modules) == 1);
}

/* The probe fails some other way: no hold, no raw call; the profiler reports the error (auto-start off). */
static void case_loaded_probe_error(void)
{
    atomic_store(&double_ok, 1);
    atomic_store(&modules, 1);
    atomic_store(&probe_error, -5);
    CHECK(prof_acquire() == -5);
    COUNTS(0, 0, 0, 0);
    CHECK(prof_release() == 0);
    COUNTS(0, 0, 0, 0);
    CHECK(atomic_load(&modules) == 1);
}
/* The measured firmware: a double initialize answers 0, so only libnet's own records (and the loaded module) can
 * tell a foreign network; the profiler still never finalizes it. */
static void case_foreign_network_double_ok(void) { foreign(1); }

/* With the measured firmware, the profiler bringing the network up itself (module not loaded, no record) still owns
 * and finalizes only its own network; a later netInitialize joins without a second raw init. */
static void case_prof_owner_double_ok(void)
{
    atomic_store(&double_ok, 1);
    CHECK(prof_acquire() == 0);
    CHECK(legacy_init() == 0);
    COUNTS(1, 0, 1, 0);
    CHECK(legacy_deinit() == 0 && atomic_load(&net_up));
    CHECK(prof_release() == 0 && !atomic_load(&net_up) && !atomic_load(&modules));
    COUNTS(1, 1, 1, 1);
}

/* The profiler's own finalize fails: it keeps the hold and may retry. */
static void case_prof_finalize_fails(void)
{
    CHECK(prof_acquire() == 0);
    atomic_store(&final_fail, -3);
    CHECK(prof_release() == -3 && atomic_load(&net_up));
    atomic_store(&final_fail, 0);
    CHECK(prof_release() == 0 && !atomic_load(&net_up));
    COUNTS(1, 2, 1, 1);
}

/* ---- concurrency: an app's netInitialize/netDeinitialize against the profiler's acquire/release ---------------- */

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
        } else {
            int rc = prof_acquire();
            CHECK(rc == 0 || rc == -1); /* -1 only while an eviction runs */
            agent_uses_network();
            CHECK(prof_release() == 0);
        }
    }
    return NULL;
}

static void case_concurrent(void)
{
    pthread_t t[2];
    for (int i = 0; i < 2; ++i)
        CHECK(pthread_create(&t[i], NULL, loop, (void *)(intptr_t)i) == 0);
    atomic_store(&go, 1);
    for (int i = 0; i < 2; ++i)
        CHECK(pthread_join(t[i], NULL) == 0);
    CHECK(!atomic_load(&net_up) && !atomic_load(&modules) && !atomic_load(&protocol));
    CHECK(atomic_load(&inits) == atomic_load(&finals));
    printf("MEASURE concurrent inits=%d evicts=%d\n", atomic_load(&inits), atomic_load(&evicts));
}

static const struct {
    const char *name;
    void (*fn)(void);
} cases[] = {
    {"legacy_alone", case_legacy_alone},
    {"cell_alone", case_cell_alone},
    {"mixed_cell_then_legacy", case_mixed_cell_then_legacy},
    {"mixed_legacy_then_cell", case_mixed_legacy_then_cell},
    {"prof_alone", case_prof_alone},
    {"legacy_then_prof", case_legacy_then_prof},
    {"legacy_owner_deinit_evicts", case_legacy_owner_deinit_evicts},
    {"prof_then_legacy", case_prof_then_legacy},
    {"prof_leaves_first", case_prof_leaves_first},
    {"cell_pool_owner", case_cell_pool_owner},
    {"prof_then_cell", case_prof_then_cell},
    {"stray_cell_finalize_no_prof", case_stray_cell_finalize_no_prof},
    {"stray_cell_finalize_prof_joined", case_stray_cell_finalize_prof_joined},
    {"stray_cell_finalize_prof_owner", case_stray_cell_finalize_prof_owner},
    {"legacy_joined_then_cell", case_legacy_joined_then_cell},
    {"legacy_then_cell_no_prof", case_legacy_then_cell_no_prof},
    {"foreign_network", case_foreign_network},
    {"foreign_network_double_ok", case_foreign_network_double_ok},
    {"loaded_but_down", case_loaded_but_down},
    {"loaded_probe_error", case_loaded_probe_error},
    {"prof_owner_double_ok", case_prof_owner_double_ok},
    {"prof_finalize_fails", case_prof_finalize_fails},
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
