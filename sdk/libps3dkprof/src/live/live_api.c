/* The public live link API (ps3dk/prof.h): ps3dk_prof_live_start and
 * ps3dk_prof_live_stop over one static agent that serves the profiler's own
 * FILE-sink capture. The app passes no path: start reads the runtime's
 * capture record, and END waits for the runtime's finalization record.
 *
 * The app's calls are caller-serialized: init, live_start, shutdown and
 * live_stop are never concurrent. The opt-in auto-start runtime
 * (live_autostart.c) runs beside them, so start, stop and stats also take
 * one internal lock. The agent object is never freed; after ETIMEDOUT or
 * ECLOCK its thread may still use it.
 *
 * Capture triggers (protocol v2): the agent thread queues a viewer's command
 * in a one-entry mailbox; the thread that marks frames takes it in its frame
 * mark (prof_frame_hook.h), calls the public capture-window API there, and
 * posts the result, which the agent sends as the ACK. The capture-window API
 * is the frame thread's (a MANUAL or FRAMES window belongs to the thread that
 * marks frames, and its descriptor is registered on the caller's ring), so
 * the agent thread never calls it. */
#include "live_api.h"
#include "live_os.h"
#include "prof_frame_hook.h"
#include <string.h>

#ifdef PS3DK_PROF_TEST_HOOKS
#include "prof_test_hooks.h"
#endif

enum { API_NEVER, API_RUNNING, API_STOPPED, API_JOIN_FAILED };

static live_agent g_agent; /* zero-initialized static storage, as live_agent.h requires */
static int g_api_state;

/* Start, stop, stats and the auto-start runtime (live_autostart.c) are serialized by this lock: the app's calls stay
 * caller-serialized among themselves, but the auto-start thread and the shutdown hook run beside them.  A cold-path
 * spin with a 1 ms sleep, so it needs no initialization (it may be taken from a constructor). */
static atomic_uint g_lock;
/* Who started the running agent: the app (ps3dk_prof_live_start) or the auto-start runtime. */
static int g_owner_auto;
/* The configuration the auto-start runtime asked for (before test overrides), for the explicit-start rule. */
static live_config g_auto_requested;

void ps3dk_prof__live_lock(void)
{
    for (;;) {
        unsigned z = 0;
        if (atomic_compare_exchange_weak_explicit(&g_lock, &z, 1u, memory_order_acquire, memory_order_relaxed))
            return;
        pal_sleep_us(1000);
    }
}

void ps3dk_prof__live_unlock(void)
{
    atomic_store_explicit(&g_lock, 0u, memory_order_release);
}

/* ---- capture triggers ------------------------------------------------------------------------------------------- */

/* The window names a viewer sent, each a descriptor the runtime keeps a pointer to for the rest of the process: a
 * fixed table, filled once per distinct name and never rewritten or freed.  The same name reuses its entry (and so
 * its descriptor); a new name once the table is full is refused EFULL.  Written by the agent thread only, before the
 * mailbox publication (release) that the frame thread acquires. */
#define LIVE_NAME_SLOTS 32u
typedef struct live_name {
    ps3dk_prof_desc desc;
    char text[LIVE_MAX_WINDOW_NAME + 1];
} live_name;
static live_name g_names[LIVE_NAME_SLOTS];
static uint32_t g_names_used;

/* The mailbox.  IDLE -> PENDING (agent: submit) -> TAKEN (frame thread) -> DONE (frame thread) -> IDLE (agent: poll).
 * The agent withdraws a request with PENDING -> IDLE (cancel; poll at shutdown); whoever wins the CAS on PENDING owns
 * the request.  A DONE whose link already ended is discarded by the next submit. */
enum { MB_IDLE, MB_PENDING, MB_TAKEN, MB_DONE };
static struct {
    atomic_uint state;
    uint32_t type, frames;
    live_name *name;
    int32_t result;
    uint32_t window_id;
} g_mb;
/* The window the live link opened last (0: none); read and written only by the request owner (TAKEN). */
static uint32_t g_live_window;

/* Strict UTF-8 (no overlong forms, no surrogates, at most U+10FFFF) without NUL: what a descriptor name may hold. */
static int name_ok(const uint8_t *s, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        uint32_t c = s[i], k, cp, min;
        if (c == 0)
            return 0;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xe0) == 0xc0) {
            k = 1;
            cp = c & 0x1f;
            min = 0x80;
        } else if ((c & 0xf0) == 0xe0) {
            k = 2;
            cp = c & 0x0f;
            min = 0x800;
        } else if ((c & 0xf8) == 0xf0) {
            k = 3;
            cp = c & 0x07;
            min = 0x10000;
        } else {
            return 0;
        }
        if (n - i - 1 < k)
            return 0;
        for (uint32_t j = 1; j <= k; ++j) {
            if ((s[i + j] & 0xc0) != 0x80)
                return 0;
            cp = cp << 6 | (s[i + j] & 0x3fu);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return 0;
        i += k + 1;
    }
    return 1;
}

/* The table entry for this name: an existing one, a new one, or NULL when the table is full. */
static live_name *name_entry(const char *name, uint32_t n)
{
    for (uint32_t i = 0; i < g_names_used; ++i)
        if (strlen(g_names[i].text) == n && memcmp(g_names[i].text, name, n) == 0)
            return &g_names[i];
    if (g_names_used == LIVE_NAME_SLOTS)
        return NULL;
    live_name *e = &g_names[g_names_used++];
    memcpy(e->text, name, n);
    e->text[n] = 0;
    /* one call site for every live window: the name alone tells them apart */
    e->desc = (ps3dk_prof_desc){.name = e->text, .logical_tag = "", .file = "ps3dk-prof live viewer", .line = 1u,
                                .scope = 1u};
    return e;
}

static int cmd_submit(void *ctx, uint32_t type, const char *name, uint32_t name_len, uint32_t frames)
{
    (void)ctx;
    if (ps3dk_prof__shutdown_claimed())
        return PS3DK_PROF_ESTOPPED;
    unsigned st = atomic_load_explicit(&g_mb.state, memory_order_acquire);
    if (st == MB_DONE) {
        /* the result of a request whose link ended while the frame thread applied it */
        atomic_store_explicit(&g_mb.state, MB_IDLE, memory_order_relaxed);
        st = MB_IDLE;
    }
    if (st != MB_IDLE)
        return PS3DK_PROF_EBUSY; /* a request of an ended link is being applied right now */
    live_name *e = NULL;
    if (type != LIVE_T_CAPTURE_END) {
        if (!name_ok((const uint8_t *)name, name_len))
            return PS3DK_PROF_EINVAL;
        e = name_entry(name, name_len);
        if (!e)
            return PS3DK_PROF_EFULL; /* LIVE_NAME_SLOTS distinct names this run */
    }
    g_mb.type = type;
    g_mb.frames = frames;
    g_mb.name = e;
    atomic_store_explicit(&g_mb.state, MB_PENDING, memory_order_release);
    return 0;
}

static int cmd_poll(void *ctx, int32_t *result, uint32_t *window_id)
{
    (void)ctx;
    unsigned st = atomic_load_explicit(&g_mb.state, memory_order_acquire);
    if (st == MB_DONE) {
        *result = g_mb.result;
        *window_id = g_mb.window_id;
        atomic_store_explicit(&g_mb.state, MB_IDLE, memory_order_relaxed);
        return 1;
    }
    /* A shutdown has begun: no frame mark will take the request any more. */
    if (st == MB_PENDING && ps3dk_prof__shutdown_claimed() &&
        atomic_compare_exchange_strong_explicit(&g_mb.state, &st, MB_IDLE, memory_order_acq_rel,
                                                memory_order_acquire)) {
        *result = PS3DK_PROF_ESTOPPED;
        *window_id = 0;
        return 1;
    }
    return 0;
}

static int cmd_cancel(void *ctx)
{
    (void)ctx;
    unsigned st = MB_PENDING;
    return atomic_compare_exchange_strong_explicit(&g_mb.state, &st, MB_IDLE, memory_order_acq_rel,
                                                   memory_order_acquire);
}

/* Applies the request on the frame thread, inside its frame mark. */
static void apply(void)
{
    ps3dk_prof_capture_info info;
    int rc;
    uint32_t id = 0;
    if (g_mb.type == LIVE_T_CAPTURE_END) {
        (void)ps3dk_prof_capture_state(&info);
        if (info.state == 0 || info.state == 3)
            rc = PS3DK_PROF_EINVAL; /* no window to end (or it already ends at this boundary) */
        else if (!g_live_window || info.id != g_live_window)
            rc = PS3DK_PROF_EPERM; /* a window the app opened: the viewer ends only its own */
        else
            rc = ps3dk_prof_capture_end();
        id = info.id;
    } else {
        uint32_t kind = g_mb.type == LIVE_T_CAPTURE_FRAMES ? PS3DK_PROF_WINDOW_FRAMES : PS3DK_PROF_WINDOW_MANUAL;
        rc = ps3dk_prof_capture_begin(&g_mb.name->desc, kind, kind == PS3DK_PROF_WINDOW_FRAMES ? g_mb.frames : 0u);
        if (rc == PS3DK_PROF_OK) {
            /* only this thread (the window's owner and opener) can change the slot before the next frame mark */
            (void)ps3dk_prof_capture_state(&info);
            id = g_live_window = info.id;
        }
    }
    g_mb.result = rc;
    g_mb.window_id = rc == PS3DK_PROF_OK ? id : 0u;
}

static void live_frame_hook(void)
{
    unsigned st = MB_PENDING;
    if (atomic_load_explicit(&g_mb.state, memory_order_relaxed) != MB_PENDING ||
        !atomic_compare_exchange_strong_explicit(&g_mb.state, &st, MB_TAKEN, memory_order_acq_rel,
                                                 memory_order_relaxed))
        return;
    apply();
    atomic_store_explicit(&g_mb.state, MB_DONE, memory_order_release);
}

static const live_cmd_ops g_cmd_ops = {.submit = cmd_submit, .poll = cmd_poll, .cancel = cmd_cancel};

static const struct ps3dk_prof_finalization *runtime_finalization(void *ctx)
{
    (void)ctx;
    return ps3dk_prof__finalization();
}

/* The public config onto the agent's: a zero field takes its default; a
 * nonzero reserved field is EINVAL; ranges are the agent's (EINVAL there). */
static int map_config(const ps3dk_prof_live_config *in, live_config *out)
{
    live_config_defaults(out);
    out->loopback_only = 0;
    if (!in)
        return PS3DK_PROF_OK;
    if (in->reserved || in->reserved2)
        return PS3DK_PROF_EINVAL;
    if (in->port)
        out->port = in->port;
    if (in->poll_us)
        out->poll_us = in->poll_us;
    if (in->send_timeout_us)
        out->send_timeout_us = in->send_timeout_us;
    if (in->handshake_timeout_us)
        out->handshake_timeout_us = in->handshake_timeout_us;
    if (in->stack_bytes)
        out->stack_bytes = in->stack_bytes;
    if (in->cap_bytes)
        out->cap_bytes = in->cap_bytes;
    return PS3DK_PROF_OK;
}

/* The checks every start makes before it uses anything: OK with the record filled, or the refusal. */
static int servable(ps3dk_prof_capture_record *rec)
{
    int rc = ps3dk_prof_get_capture_record(rec);
    if (rc == PS3DK_PROF_ENOTINIT)
        return PS3DK_PROF_ENOTINIT;
    if (ps3dk_prof__shutdown_claimed())
        return PS3DK_PROF_ESTOPPED;
    if (rc != PS3DK_PROF_OK)
        return rc == PS3DK_PROF_ENOTSUP ? PS3DK_PROF_ENOTSUP : rc; /* ENOTSUP: a callback sink, no file */
    if (ps3dk_prof__capture_rolling())
        return PS3DK_PROF_ENOTSUP; /* append byte offsets cannot serve reused slots */
    if (!live_os_capture_supported(rec->path))
        return PS3DK_PROF_ENOTSUP;
    return PS3DK_PROF_OK;
}

int ps3dk_prof__live_servable(void)
{
    ps3dk_prof_capture_record rec;
    return servable(&rec);
}

/* An explicit start while the auto-started agent runs: a no-op when every field the app set (nonzero) equals what
 * the auto-start asked for; anything else is EALREADY, as for any second start. */
static int auto_compatible(const ps3dk_prof_live_config *in)
{
    const live_config *r = &g_auto_requested;
    if (!in)
        return 1;
    if (in->reserved || in->reserved2)
        return 0;
    return (!in->port || in->port == r->port) && (!in->poll_us || in->poll_us == r->poll_us) &&
           (!in->send_timeout_us || in->send_timeout_us == r->send_timeout_us) &&
           (!in->handshake_timeout_us || in->handshake_timeout_us == r->handshake_timeout_us) &&
           (!in->stack_bytes || in->stack_bytes == r->stack_bytes) && (!in->cap_bytes || in->cap_bytes == r->cap_bytes);
}

/* Under the lock.  is_auto: the auto-start runtime's own start. */
int ps3dk_prof__live_start_locked(const ps3dk_prof_live_config *cfg, int is_auto)
{
    ps3dk_prof_capture_record rec;
    int rc = servable(&rec);
    if (rc != PS3DK_PROF_OK)
        return rc;
    if (g_api_state != API_NEVER)
        return !is_auto && g_owner_auto && g_api_state == API_RUNNING && auto_compatible(cfg) ? PS3DK_PROF_OK
                                                                                                : PS3DK_PROF_EALREADY;
    live_config c;
    rc = map_config(cfg, &c);
    if (rc != PS3DK_PROF_OK)
        return rc;
    live_config requested = c;
#ifdef PS3DK_PROF_TEST_HOOKS
    if (ps3dk_prof_test_hooks.live_ephemeral_port) {
        c.port = 0;
        c.loopback_only = 1;
    }
    if (ps3dk_prof_test_hooks.live_listen_port) {
        c.port = ps3dk_prof_test_hooks.live_listen_port;
        c.loopback_only = 1;
    }
#endif
    live_source src = {.path = rec.path, .session_id = rec.session_id, .app = rec.app, .app_len = rec.app_len,
                       .build_id = (const char *)rec.build_id, .build_id_len = rec.build_id_len,
                       .finalization = runtime_finalization, .cmd = &g_cmd_ops};
    ps3dk_prof__set_frame_hook(live_frame_hook); /* before a viewer can send a command */
    rc = live_agent_start(&g_agent, &c, &src);
    if (rc != LIVE_OK)
        ps3dk_prof__set_frame_hook(NULL);
    switch (rc) {
    case LIVE_OK:
        g_api_state = API_RUNNING;
        g_owner_auto = is_auto;
        if (is_auto)
            g_auto_requested = requested;
        return PS3DK_PROF_OK;
    case LIVE_EINVAL:
        return PS3DK_PROF_EINVAL;
    case LIVE_EALREADY:
        return PS3DK_PROF_EALREADY;
    case LIVE_ENET:
        return PS3DK_PROF_ENET;
    default:
        return PS3DK_PROF_ETHREAD;
    }
}

int ps3dk_prof_live_start(const ps3dk_prof_live_config *cfg)
{
    ps3dk_prof__live_lock();
    int rc = ps3dk_prof__live_start_locked(cfg, 0);
    ps3dk_prof__live_unlock();
    return rc;
}

int ps3dk_prof__live_state_locked(int *owner_auto)
{
    *owner_auto = g_api_state != API_NEVER && g_owner_auto;
    switch (g_api_state) {
    case API_RUNNING:
        return LIVE_API_RUNNING;
    case API_STOPPED:
        return LIVE_API_STOPPED;
    case API_JOIN_FAILED:
        return LIVE_API_JOIN_FAILED;
    default:
        return LIVE_API_NEVER;
    }
}

int ps3dk_prof_live_stop(uint64_t graceful_us, uint64_t join_us)
{
    ps3dk_prof__live_lock();
    int rc = ps3dk_prof__live_stop_locked(graceful_us, join_us);
    ps3dk_prof__live_unlock();
    return rc;
}

int ps3dk_prof__live_stop_locked(uint64_t graceful_us, uint64_t join_us)
{
    switch (g_api_state) {
    case API_NEVER:
        return PS3DK_PROF_ENOTINIT;
    case API_STOPPED:
        return PS3DK_PROF_EALREADY;
    case API_JOIN_FAILED:
        return PS3DK_PROF_ETHREAD;
    default:
        break;
    }
    switch (live_agent_stop(&g_agent, graceful_us, join_us)) {
    case LIVE_OK:
        g_api_state = API_STOPPED;
        ps3dk_prof__set_frame_hook(NULL); /* no viewer is left to answer */
        return PS3DK_PROF_OK;
    case LIVE_ETIMEDOUT:
        return PS3DK_PROF_ETIMEDOUT;
    case LIVE_ECLOCK:
        return PS3DK_PROF_ECLOCK;
    case LIVE_EINVAL:
        return PS3DK_PROF_EINVAL;
    case LIVE_EJOIN:
        g_api_state = API_JOIN_FAILED;
        return PS3DK_PROF_ETHREAD;
    default:
        return PS3DK_PROF_ETHREAD; /* LIVE_ESTATE: unreachable while RUNNING */
    }
}

_Static_assert(LIVE_DROP_COUNT == 10, "ps3dk_prof_live_stats names every drop reason");

static int get_stats_locked(ps3dk_prof_live_stats *out);

int ps3dk_prof_live_get_stats(ps3dk_prof_live_stats *out)
{
    if (!out)
        return PS3DK_PROF_EINVAL;
    ps3dk_prof__live_lock();
    int rc = get_stats_locked(out);
    ps3dk_prof__live_unlock();
    return rc;
}

static int get_stats_locked(ps3dk_prof_live_stats *out)
{
    switch (g_api_state) {
    case API_NEVER:
        return PS3DK_PROF_ENOTINIT;
    case API_RUNNING:
        return PS3DK_PROF_EBUSY;
    case API_JOIN_FAILED:
        return PS3DK_PROF_ETHREAD;
    default:
        break;
    }
    /* API_STOPPED: the thread is joined, so these are final; the explicit mapping keeps each field's reason */
    const live_agent *a = &g_agent;
    ps3dk_prof_live_stats s;
    memset(&s, 0, sizeof s);
    s.version = 1;
    s.connections = atomic_load(&a->connections);
    s.busy_refusals = atomic_load(&a->busy_refusals);
    s.bytes_delivered = atomic_load(&a->bytes_delivered);
    s.pongs = atomic_load(&a->pongs);
    s.drop_end = atomic_load(&a->drops[LIVE_DROP_END]);
    s.drop_handshake_timeout = atomic_load(&a->drops[LIVE_DROP_HANDSHAKE_TIMEOUT]);
    s.drop_bad_hello = atomic_load(&a->drops[LIVE_DROP_BAD_HELLO]);
    s.drop_protocol = atomic_load(&a->drops[LIVE_DROP_PROTOCOL]);
    s.drop_ping_flood = atomic_load(&a->drops[LIVE_DROP_PING_FLOOD]);
    s.drop_stalled = atomic_load(&a->drops[LIVE_DROP_STALLED]);
    s.drop_peer_closed = atomic_load(&a->drops[LIVE_DROP_PEER_CLOSED]);
    s.drop_io = atomic_load(&a->drops[LIVE_DROP_IO]);
    s.drop_cancelled = atomic_load(&a->drops[LIVE_DROP_CANCELLED]);
    s.drop_clock = atomic_load(&a->drops[LIVE_DROP_CLOCK]);
    *out = s;
    return PS3DK_PROF_OK;
}

uint16_t ps3dk_prof__live_port(void)
{
    return g_api_state == API_NEVER ? 0 : live_agent_port(&g_agent);
}

const live_agent *ps3dk_prof__live_agent(void)
{
    return &g_agent;
}
