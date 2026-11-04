/* The public live link API (ps3dk/prof.h): ps3dk_prof_live_start and
 * ps3dk_prof_live_stop over one static agent that serves the profiler's own
 * FILE-sink capture. The app passes no path: start reads the runtime's
 * capture record, and END waits for the runtime's finalization record.
 *
 * Calls are caller-serialized: init, live_start, shutdown and live_stop are
 * never concurrent. The agent object is never freed; after ETIMEDOUT or
 * ECLOCK its thread may still use it. */
#include "live_api.h"
#include "live_os.h"
#include <string.h>

#ifdef PS3DK_PROF_TEST_HOOKS
#include "prof_test_hooks.h"
#endif

enum { API_NEVER, API_RUNNING, API_STOPPED, API_JOIN_FAILED };

static live_agent g_agent; /* zero-initialized static storage, as live_agent.h requires */
static int g_api_state;

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

int ps3dk_prof_live_start(const ps3dk_prof_live_config *cfg)
{
    ps3dk_prof_capture_record rec;
    int rc = ps3dk_prof_get_capture_record(&rec);
    if (rc == PS3DK_PROF_ENOTINIT)
        return PS3DK_PROF_ENOTINIT;
    if (ps3dk_prof__shutdown_claimed())
        return PS3DK_PROF_ESTOPPED;
    if (rc != PS3DK_PROF_OK)
        return rc == PS3DK_PROF_ENOTSUP ? PS3DK_PROF_ENOTSUP : rc; /* ENOTSUP: a callback sink, no file */
    if (!live_os_capture_supported(rec.path))
        return PS3DK_PROF_ENOTSUP;
    if (g_api_state != API_NEVER)
        return PS3DK_PROF_EALREADY;
    live_config c;
    rc = map_config(cfg, &c);
    if (rc != PS3DK_PROF_OK)
        return rc;
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
                       .finalization = runtime_finalization};
    switch (live_agent_start(&g_agent, &c, &src)) {
    case LIVE_OK:
        g_api_state = API_RUNNING;
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

int ps3dk_prof_live_stop(uint64_t graceful_us, uint64_t join_us)
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

int ps3dk_prof_live_get_stats(ps3dk_prof_live_stats *out)
{
    if (!out)
        return PS3DK_PROF_EINVAL;
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
