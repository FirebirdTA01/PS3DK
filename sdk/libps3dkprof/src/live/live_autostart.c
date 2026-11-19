/* Live viewer auto-start (opt-in): the live agent with no app code.  docs/profiler-guide.md section 4.3.
 *
 * Linked only on request: the program references __ps3dk_prof_live_autostart_anchor (CMake PS3_PROF_LIVE_AUTOSTART,
 * or PS3DK_PROF_LIVE_AUTOSTART / PS3DK_PROF_LIVE_AUTOSTART_OPTIONS in <ps3dk/prof.h>).  The core calls the two hooks
 * below through weak references (prof_frame_hook.h), so without this member nothing changes.
 *
 * Start: after every successful init (constructor(100) in the automatic lifecycles, the app's call otherwise) the
 * init hook only creates a start-up thread.  That thread checks that the capture can be served (a FILE sink on
 * /dev_hdd0/, no rolling), takes the profiler's network hold WITHOUT the live lock (LV2: libnet's holders,
 * sdk/libnet/src/net_hold.h: it joins a network an app front end brought up, or brings one up with its own static
 * pool), then, under the live lock, starts the agent exactly as ps3dk_prof_live_start would, unless the shutdown,
 * the exit path, an eviction or the app's own start came first.  Any failure prints one line; profiling continues.
 *
 * Eviction: when the app takes the network back (its finalize of a network it owns, or a Cell-style init with its
 * own pool over the profiler's), libnet calls on_evict before the app's call reaches the firmware: the agent is
 * stopped at once (a viewer sees a disconnect), the hold is dropped, and nothing here calls the network again.
 *
 * Stop: the shutdown hook runs after the shutdown published its finalization, so a connected viewer receives END:
 * it waits a bounded time for the start-up thread, stops the agent it started (graceful 2 s + join 1 s, then once
 * more 0 + 2 s), and releases its network hold only after a successful stop.  It never releases a hold it did not
 * take.  A manual-lifecycle program that exits without shutdown is covered by a late destructor (no END).
 *
 * An app's own ps3dk_prof_live_start while this agent runs is a no-op (OK) when its nonzero fields match, EALREADY
 * otherwise (live_api.c); if the auto-start failed or has not started yet, the app's start works exactly as without
 * this member (the auto-start then backs off). */
#include "live_api.h"
#include "live_os.h"
#include "prof_frame_hook.h"
#include <stdatomic.h>
#include <string.h>

#ifdef PS3DK_PROF_TEST_HOOKS
#include "prof_test_hooks.h"
#endif

/* Requested with -u (or the header's .globl): extracting this member is what turns auto-start on. */
void __ps3dk_prof_live_autostart_anchor(void) {}

/* The optional settings (PS3DK_PROF_LIVE_AUTOSTART_OPTIONS), and the automatic lifecycles' anchors (auto/startup.c,
 * auto_rsx/auto_startup.c): all weak, absent unless that object is linked.  Either anchor means an automatic lifecycle
 * shuts the profiler down in its own destructor(100). */
extern const ps3dk_prof_live_autostart_options ps3dk_prof_live_autostart_settings __attribute__((weak));
extern void __ps3dk_prof_startup_anchor(void) __attribute__((weak));
extern void __ps3dk_prof_auto_startup_anchor(void) __attribute__((weak));

#define START_STACK_BYTES (64u * 1024u)
#define START_WAIT_US 5000000u /* the most teardown waits for the start-up thread before it goes on without it */

static atomic_uint g_init_seen;   /* the init hook ran (once per process) */
static atomic_uint g_thread_live; /* the start-up thread exists and is not joined yet */
static atomic_uint g_start_done;  /* the start-up thread has finished its work */
static atomic_uint g_torn_down;   /* the shutdown hook or the exit destructor ran */
static pal_thread g_thread;
/* Under the live lock: this runtime holds the network; the network was taken back by the app (no use after). */
static int g_net_held, g_evicted;

static void say(const char *text)
{
    int err = 0;
    unsigned long n = strlen(text);
    while (n) {
        long w = pal_write(2, text, n, &err);
        if (w <= 0 || (unsigned long)w > n)
            break;
        text += w;
        n -= (unsigned long)w;
    }
}

/* "<prefix><value><suffix>": a decimal or 0x-hex number without stdio. */
static void say_num(const char *prefix, uint32_t v, int hex, const char *suffix)
{
    char buf[160], digits[12];
    size_t n = strlen(prefix), k = 0;
    if (n > sizeof buf - 40)
        n = sizeof buf - 40;
    memcpy(buf, prefix, n);
    do {
        uint32_t d = hex ? v & 15u : v % 10u;
        digits[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v = hex ? v >> 4 : v / 10u;
    } while (v && k < sizeof digits);
    if (hex) {
        buf[n++] = '0';
        buf[n++] = 'x';
    }
    while (k)
        buf[n++] = digits[--k];
    size_t s = strlen(suffix);
    if (s > sizeof buf - 1 - n)
        s = sizeof buf - 1 - n;
    memcpy(buf + n, suffix, s);
    buf[n + s] = 0;
    say(buf);
}

/* Stops the agent this runtime started (under the live lock): graceful_us for a viewer, then once more with none.
 * The resulting state is returned through *state; the result of the last stop is returned. */
static int stop_auto_agent(uint64_t graceful_us, int *state)
{
    int owner_auto, rc = PS3DK_PROF_OK;
    *state = ps3dk_prof__live_state_locked(&owner_auto);
    if (owner_auto && *state == LIVE_API_RUNNING) {
        rc = ps3dk_prof__live_stop_locked(graceful_us, 1000000u);
        if (rc == PS3DK_PROF_ETIMEDOUT || rc == PS3DK_PROF_ECLOCK)
            rc = ps3dk_prof__live_stop_locked(0, 2000000u);
        *state = ps3dk_prof__live_state_locked(&owner_auto);
    }
    return rc;
}

/* ---- the network hold ---------------------------------------------------------------------------------------- */

static void on_evict(void);

#ifdef PS3DK_PROF_PAL_LV2
#include <cell/sysmodule.h>
/* sdk/libnet/src/net_hold.h (libnet, linked with -lnet) */
extern int __ps3dk_net_prof_acquire(void *pool, uint32_t pool_bytes, int (*load)(void), int (*unload)(void),
                                    int (*is_loaded)(void), void (*evict)(void));
extern int __ps3dk_net_prof_release(void);
extern void *__ps3dk_net_default_pool(uint32_t *bytes);

static int load_net(void)
{
    return cellSysmoduleLoadModule(CELL_SYSMODULE_NET);
}

static int unload_net(void)
{
    return cellSysmoduleUnloadModule(CELL_SYSMODULE_NET);
}

static int net_loaded(void)
{
    return cellSysmoduleIsLoaded(CELL_SYSMODULE_NET) == CELL_SYSMODULE_LOADED;
}

static int net_acquire(void *pool, uint32_t bytes)
{
    if (!pool)
        pool = __ps3dk_net_default_pool(&bytes);
    return __ps3dk_net_prof_acquire(pool, bytes, load_net, unload_net, net_loaded, on_evict);
}

static int net_release(void)
{
    return __ps3dk_net_prof_release();
}
#else
/* Host builds: the test decides (prof_test_hooks.h); without a hook the network is simply there. */
static int net_acquire(void *pool, uint32_t bytes)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.live_net_acquire)
        return ps3dk_prof_test_hooks.live_net_acquire(pool, bytes, on_evict);
#endif
    (void)pool, (void)bytes;
    return 0;
}

static int net_release(void)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.live_net_release)
        return ps3dk_prof_test_hooks.live_net_release();
#endif
    return 0;
}
#endif

/* libnet, without its own lock held, before an app's call takes the network back: stop every use of it now. */
static void on_evict(void)
{
    ps3dk_prof__live_lock();
    g_evicted = 1;
    int state, rc = stop_auto_agent(0, &state);
    if (g_net_held) {
        (void)net_release(); /* during an eviction this only drops the hold */
        g_net_held = 0;
    }
    if (rc == PS3DK_PROF_OK)
        say("ps3prof: live auto-start stopped: the app took the network back (its own teardown or its own pool)\n");
    else
        say_num("ps3prof: live auto-start: the app took the network back but the agent did not stop (", (uint32_t)-rc,
                0, ")\n");
    ps3dk_prof__live_unlock();
}

/* ---- start ---------------------------------------------------------------------------------------------------- */

static const ps3dk_prof_live_autostart_options *options(void)
{
    const ps3dk_prof_live_autostart_options *o = &ps3dk_prof_live_autostart_settings;
    if (!o)
        return NULL;
    if (o->size != sizeof *o || o->reserved || o->reserved2 || (!o->net_pool) != (!o->net_pool_bytes)) {
        say("ps3prof: live auto-start options refused (size, reserved fields or pool); defaults used\n");
        return NULL;
    }
    return o;
}

static void start_work(void)
{
    const ps3dk_prof_live_autostart_options *o = options();
    ps3dk_prof_live_config cfg;
    memset(&cfg, 0, sizeof cfg);
    if (o)
        cfg.port = o->port;
    int rc = ps3dk_prof__live_servable();
    if (rc == PS3DK_PROF_ESTOPPED)
        return; /* the shutdown began first: nothing to serve */
    if (rc != PS3DK_PROF_OK) {
        say("ps3prof: live auto-start off: the capture cannot be served (a FILE sink on /dev_hdd0/ without "
            "rolling is required)\n");
        return;
    }
    /* The network first, without the live lock: a slow module load never holds up teardown or the app's calls. */
    rc = net_acquire(o ? o->net_pool : NULL, o ? o->net_pool_bytes : 0u);
    if (rc != 0) {
        say_num("ps3prof: live auto-start off: network bring-up failed (", (uint32_t)rc, 1, ")\n");
        return;
    }
    ps3dk_prof__live_lock();
    int owner_auto;
    if (g_evicted) {
        say("ps3prof: live auto-start off: the app took the network back during start-up\n");
    } else if (atomic_load(&g_torn_down) || ps3dk_prof__shutdown_claimed()) {
        (void)net_release(); /* teardown went on without this thread: give the hold back */
        say("ps3prof: live auto-start: start-up finished after teardown; network hold given back\n");
    } else if (ps3dk_prof__live_state_locked(&owner_auto) != LIVE_API_NEVER) {
        (void)net_release();
        say("ps3prof: live auto-start: the app started the live agent itself\n");
    } else {
        g_net_held = 1;
        rc = ps3dk_prof__live_start_locked(&cfg, 1);
        if (rc == PS3DK_PROF_OK) {
            say_num("ps3prof: live viewer: connect to this console on port ", ps3dk_prof__live_port(), 0, "\n");
        } else {
            (void)net_release(); /* no agent thread exists after a failed start */
            g_net_held = 0;
            if (rc == PS3DK_PROF_ENET)
                say_num("ps3prof: live auto-start off: cannot listen on port ", cfg.port ? cfg.port : 31750u, 0,
                        "\n");
            else
                say_num("ps3prof: live auto-start off: start refused (", (uint32_t)-rc, 0, ")\n");
        }
    }
    ps3dk_prof__live_unlock();
}

static void *start_thread(void *arg)
{
    (void)arg;
    start_work();
    atomic_store(&g_start_done, 1u);
    return NULL;
}

void ps3dk_prof__live_autostart_after_init(void)
{
    if (atomic_exchange(&g_init_seen, 1u))
        return; /* one capture per process */
    if (pal_thread_create(&g_thread, start_thread, NULL, START_STACK_BYTES) != 0) {
        say("ps3prof: live auto-start off: start-up thread not created\n");
        return;
    }
    atomic_store(&g_thread_live, 1u);
}

/* ---- stop ----------------------------------------------------------------------------------------------------- */

static void teardown(uint64_t graceful_us)
{
    if (atomic_exchange(&g_torn_down, 1u))
        return;
    if (atomic_load(&g_thread_live)) {
        /* Bounded: a start-up thread stuck in the network bring-up is left to finish on its own (it then sees the
         * teardown and gives its hold back); it is never joined after that. */
        for (uint32_t waited = 0; !atomic_load(&g_start_done) && waited < START_WAIT_US; waited += 1000u)
            pal_sleep_us(1000);
        if (atomic_load(&g_start_done)) {
            atomic_store(&g_thread_live, 0u);
            if (pal_thread_join(g_thread) != 0)
                say("ps3prof: live auto-start: start-up thread join failed\n");
        } else {
            say("ps3prof: live auto-start: start-up thread still in network bring-up; left to finish on its own\n");
        }
    }
    ps3dk_prof__live_lock();
    int state, rc = stop_auto_agent(graceful_us, &state);
    if (g_net_held) {
        /* An agent that did not stop may still use a socket: its hold stays until the process exits. */
        if (state == LIVE_API_STOPPED || state == LIVE_API_NEVER) {
            int nrc = net_release();
            g_net_held = 0;
            if (nrc != 0)
                say_num("ps3prof: live auto-start: network release failed (", (uint32_t)nrc, 1, ")\n");
        } else {
            say_num("ps3prof: live auto-start: the agent did not stop (", (uint32_t)-rc, 0,
                    "); the network stays up until exit\n");
        }
    }
    ps3dk_prof__live_unlock();
}

void ps3dk_prof__live_autostart_after_shutdown(void)
{
    teardown(2000000u); /* the finalization is published: a connected viewer gets END */
}

/* After user destructors (default priority), before the automatic lifecycles' destructor(100).  In a manual-lifecycle
 * program that never called shutdown, the agent is cancelled here (no END: nothing was finalized) and the hold
 * released.  An automatic lifecycle shuts down in its own destructor(100), which runs the hook above with END. */
__attribute__((destructor(101))) static void autostart_exit(void)
{
    if (__ps3dk_prof_startup_anchor || __ps3dk_prof_auto_startup_anchor)
        return;
    if (!atomic_load(&g_init_seen) || ps3dk_prof__shutdown_claimed())
        return;
    teardown(0);
}
