/* Live link agent. See live_agent.h for the contract and live_proto.h for
 * the wire schema. Every socket, poll and file call goes through the OS seam
 * (live_os.h); this file holds only the protocol and the state machine. */
#include "live_agent.h"
#include "live_os.h"
#include <string.h>

#ifdef PS3DK_PROF_TEST_HOOKS
live_test_hooks ps3dk_prof_test_live_hooks;
#endif

/* Agent states. JOIN_FAILED is terminal: the handle is consumed. */
enum { ST_IDLE, ST_RUNNING, ST_STOPPING, ST_STOPPED, ST_JOIN_FAILED };
enum { C_NONE, C_HANDSHAKE, C_STREAM };

static int now_us(uint64_t *t)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    if (atomic_load(&ps3dk_prof_test_live_hooks.clock_fail))
        return -1;
#endif
    return pal_mono_now_us(t);
}

/* A file read; under test hooks it can be held to stand in for an LV2 file
 * call with no deadline. */
static int32_t file_read(int fd, void *buf, uint32_t n, uint64_t off)
{
#ifdef PS3DK_PROF_TEST_HOOKS
    live_test_hooks *h = &ps3dk_prof_test_live_hooks;
    if (atomic_exchange(&h->pread_block, 0)) {
        atomic_store(&h->pread_entered, 1);
        while (!atomic_load(&h->pread_release))
            pal_sleep_us(1000);
    }
#endif
    return live_os_file_read_at(fd, buf, n, off);
}

void live_config_defaults(live_config *cfg)
{
    *cfg = (live_config){
        .port = 31750,
        .poll_us = 10000,
        .send_timeout_us = 2000000,
        .handshake_timeout_us = 2000000,
        .stack_bytes = 64 * 1024,
        .cap_bytes = LIVE_DEFAULT_CAP_BYTES,
    };
}

/* strnlen without POSIX: the length of s, or max if no NUL comes first. */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static void put_hdr(uint8_t *p, uint32_t type, uint32_t len)
{
    live_put16(p, type);
    live_put16(p + 2, 0);
    live_put32(p + 4, len);
}

static void queue_frame(live_out *o, uint32_t type, const uint8_t *payload, uint32_t len)
{
    put_hdr(o->bytes, type, len);
    if (len)
        memcpy(o->bytes + LIVE_HDR_BYTES, payload, len);
    o->len = LIVE_HDR_BYTES + len;
    o->done = 0;
    o->data_bytes = 0;
}

static void drop(live_agent *a, live_conn *c, enum live_drop why)
{
    /* Count first: a peer that sees the close may read the counters. */
    atomic_fetch_add(&a->drops[why], 1);
    live_os_close(c->file_fd);
    live_os_close(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = c->file_fd = -1;
    c->phase = C_NONE;
}

/* The graceful interval of a requested stop has run out. */
static int cancel_due(live_agent *a, uint64_t now)
{
    return atomic_load(&a->state) == ST_STOPPING && now >= atomic_load(&a->graceful_deadline_us);
}

/* Sends what the socket accepts now. -1: the peer is gone. */
static int flush_out(live_agent *a, live_conn *c, uint64_t now)
{
    while (c->out.done < c->out.len) {
        int32_t n = live_os_send(c->fd, c->out.bytes + c->out.done, c->out.len - c->out.done);
        if (n > 0) {
            c->out.done += (uint32_t)n;
            c->last_progress = now;
            if (c->out.done == c->out.len && c->out.data_bytes)
                atomic_fetch_add(&a->bytes_delivered, c->out.data_bytes);
            continue;
        }
        if (n == LIVE_OS_AGAIN)
            return 0;
        return -1; /* closed, failed, or nothing accepted */
    }
    return 0;
}

static void queue_hello(live_agent *a, live_conn *c)
{
    uint8_t p[16 + 2 + LIVE_MAX_APP + 2 + LIVE_MAX_BUILD_ID + 2 + LIVE_MAX_NAME];
    uint32_t n;
    live_put16(p, LIVE_VERSION);
    live_put16(p + 2, LIVE_AGENT_VERSION);
    live_put32(p + 4, 0);
    live_put64(p + 8, a->session_id);
    n = 16;
    live_put16(p + n, a->app_len);
    memcpy(p + n + 2, a->app, a->app_len);
    n += 2 + a->app_len;
    live_put16(p + n, a->build_id_len);
    memcpy(p + n + 2, a->build_id, a->build_id_len);
    n += 2 + a->build_id_len;
    live_put16(p + n, a->name_len);
    memcpy(p + n + 2, a->name, a->name_len);
    n += 2 + a->name_len;
    queue_frame(&c->out, LIVE_T_HELLO, p, n);
}

/* Queues the next DATA or END frame, or nothing (wait for more data).
 * Returns -1 if the file cannot be read. */
static int queue_next(live_agent *a, live_conn *c)
{
    uint64_t room = c->sent < a->cfg.cap_bytes ? a->cfg.cap_bytes - c->sent : 0;
    if (room) {
        uint32_t want = room < LIVE_MAX_DATA ? (uint32_t)room : LIVE_MAX_DATA;
        uint8_t *payload = c->out.bytes + LIVE_HDR_BYTES;
        int32_t n = file_read(c->file_fd, payload + 8, want, c->sent);
        if (n < 0)
            return -1;
        if (n > 0) {
            live_put64(payload, c->sent);
            put_hdr(c->out.bytes, LIVE_T_DATA, 8u + (uint32_t)n);
            c->out.len = LIVE_HDR_BYTES + 8u + (uint32_t)n;
            c->out.done = 0;
            c->out.data_bytes = (uint32_t)n;
            c->sent += (uint64_t)n;
            return 0;
        }
    }
    /* At EOF (or the cap). EOF before the finalization record means "wait".
     * Once the record exists the writer has closed the file, so a read
     * after seeing it returns the true end: check once more before END. */
    const struct ps3dk_prof_finalization *f = a->finalization(a->ctx);
    if (!f)
        return 0;
    if (room) {
        uint8_t probe;
        int32_t n = file_read(c->file_fd, &probe, 1, c->sent);
        if (n < 0)
            return -1;
        if (n > 0)
            return 0; /* bytes arrived before close: send them first */
    }
    uint8_t e[LIVE_END_BYTES] = {0};
    e[0] = f->io_finalized;
    e[1] = f->trailer_written;
    e[2] = f->join_failed;
    live_put32(e + 4, (uint32_t)f->shutdown_result);
    live_put64(e + 8, f->accepted_bytes);
    live_put64(e + 16, c->sent);
    queue_frame(&c->out, LIVE_T_END, e, LIVE_END_BYTES);
    c->end_queued = 1;
    return 0;
}

/* Reads and handles what the viewer sent. Returns a drop reason, or -1 to keep going. */
static int read_in(live_agent *a, live_conn *c, uint64_t now)
{
    for (;;) {
        uint32_t need = c->in_len < LIVE_HDR_BYTES ? LIVE_HDR_BYTES : LIVE_HDR_BYTES + live_get32(c->in + 4);
        if (c->in_len >= LIVE_HDR_BYTES) {
            uint32_t type = live_get16(c->in), flags = live_get16(c->in + 2), len = live_get32(c->in + 4);
            if (flags)
                return LIVE_DROP_PROTOCOL;
            if (c->phase == C_HANDSHAKE ? type != LIVE_T_CLIENT_HELLO || len != 8
                                        : type != LIVE_T_PING || len != 0)
                return c->phase == C_HANDSHAKE ? LIVE_DROP_BAD_HELLO : LIVE_DROP_PROTOCOL;
        }
        if (c->in_len >= LIVE_HDR_BYTES && c->in_len == need) {
            if (c->phase == C_HANDSHAKE) {
                const uint8_t *p = c->in + LIVE_HDR_BYTES;
                /* A hello that completes after the deadline is still late. */
                if (now >= c->handshake_deadline)
                    return LIVE_DROP_HANDSHAKE_TIMEOUT;
                if (live_get32(p) != LIVE_MAGIC || live_get16(p + 4) != LIVE_VERSION || live_get16(p + 6) != 0)
                    return LIVE_DROP_BAD_HELLO;
                c->file_fd = live_os_file_open(a->path);
                if (c->file_fd < 0)
                    return LIVE_DROP_IO;
                /* open may block (a slow file system); a cancellation that
                 * fell due meanwhile wins over starting the stream. */
                uint64_t after;
                if (now_us(&after) != 0)
                    return LIVE_DROP_CLOCK;
                if (cancel_due(a, after))
                    return LIVE_DROP_CANCELLED;
                c->phase = C_STREAM;
                c->last_progress = after;
                c->ping_window_start = after;
                queue_hello(a, c);
            } else {
                if (now - c->ping_window_start >= 1000000) {
                    c->ping_window_start = now;
                    c->pings_in_window = 0;
                }
                if (++c->pings_in_window > LIVE_MAX_PINGS_PER_S)
                    return LIVE_DROP_PING_FLOOD;
                c->pongs_pending++;
            }
            c->in_len = 0;
            continue;
        }
        int32_t n = live_os_recv(c->fd, c->in + c->in_len, need - c->in_len);
        if (n > 0) {
            c->in_len += (uint32_t)n;
            continue;
        }
        if (n == LIVE_OS_AGAIN)
            return -1;
        return LIVE_DROP_PEER_CLOSED; /* closed or failed */
    }
}

/* BUSY is best-effort: one non-blocking send attempt, then an immediate
 * close. A refused peer never holds the agent. */
static void refuse_busy(live_agent *a, int fd)
{
    uint8_t f[LIVE_HDR_BYTES + 4];
    put_hdr(f, LIVE_T_BUSY, 4);
    live_put32(f + LIVE_HDR_BYTES, LIVE_BUSY_STREAMING);
    int nonblock = live_os_set_nonblock(fd);
#ifdef PS3DK_PROF_TEST_HOOKS
    if (atomic_load(&ps3dk_prof_test_live_hooks.busy_nonblock_fail))
        nonblock = -1;
#endif
    if (nonblock == 0) {
#ifdef PS3DK_PROF_TEST_HOOKS
        if (!atomic_load(&ps3dk_prof_test_live_hooks.busy_send_eagain))
#endif
            (void)live_os_send(fd, f, sizeof f); /* one attempt, on the now non-blocking socket */
    }
    atomic_fetch_add(&a->busy_refusals, 1); /* before the close the peer observes */
    live_os_close(fd);
}

static void *agent_main(void *arg)
{
    live_agent *a = arg;
    live_conn *c = &a->conn;
    for (;;) {
        uint64_t now;
        if (now_us(&now) != 0) {
            /* Deadlines can no longer be kept. A drop is counted only for a
             * connection that exists (a stream round may already have
             * dropped it for the same failure). */
            if (c->phase != C_NONE)
                drop(a, c, LIVE_DROP_CLOCK);
            break;
        }
        int stopping = atomic_load(&a->state) == ST_STOPPING;
        if (stopping && (c->phase != C_STREAM || cancel_due(a, now))) {
            if (c->phase != C_NONE)
                drop(a, c, LIVE_DROP_CANCELLED);
            break;
        }
        live_os_pollfd pf[2] = {{.fd = a->listen_fd, .events = LIVE_OS_IN}, {.fd = c->fd, .events = LIVE_OS_IN}};
        if (c->phase != C_NONE && c->out.done < c->out.len)
            pf[1].events |= LIVE_OS_OUT;
        (void)live_os_poll(pf, c->phase == C_NONE ? 1 : 2, a->cfg.poll_us / 1000);
        if (now_us(&now) != 0)
            continue; /* handled at the top */
        if (pf[0].revents & LIVE_OS_IN) {
            int fd = live_os_accept(a->listen_fd);
            if (fd >= 0) {
                if (c->phase != C_NONE || stopping) {
                    refuse_busy(a, fd);
                } else if (live_os_set_nonblock(fd) < 0) {
                    live_os_close(fd);
                } else {
                    memset(c, 0, sizeof *c);
                    c->fd = fd;
                    c->file_fd = -1;
                    c->phase = C_HANDSHAKE;
                    c->handshake_deadline = now + a->cfg.handshake_timeout_us;
                    atomic_fetch_add(&a->connections, 1);
                }
            }
        }
        if (c->phase == C_NONE)
            continue;
        int why = read_in(a, c, now);
        if (why >= 0) {
            drop(a, c, (enum live_drop)why);
            continue;
        }
        if (c->phase == C_HANDSHAKE) {
            if (now >= c->handshake_deadline)
                drop(a, c, LIVE_DROP_HANDSHAKE_TIMEOUT);
            continue;
        }
        /* STREAM: finish the frame in flight, then PONGs, then the next frame. */
        for (int round = 0; round < 64; ++round) {
            /* File reads may have blocked: before sending anything more,
             * a due cancellation (or a failed clock) ends the stream, so a
             * frame queued after the deadline is never sent, END included. */
            uint64_t t;
            if (now_us(&t) != 0) {
                drop(a, c, LIVE_DROP_CLOCK);
                break;
            }
            if (cancel_due(a, t)) {
                drop(a, c, LIVE_DROP_CANCELLED);
                break;
            }
            now = t;
            if (flush_out(a, c, now) < 0) {
                drop(a, c, LIVE_DROP_PEER_CLOSED);
                break;
            }
            if (c->out.done < c->out.len)
                break;
            if (c->end_queued) {
                drop(a, c, LIVE_DROP_END);
                break;
            }
            if (c->pongs_pending) {
                c->pongs_pending--;
                queue_frame(&c->out, LIVE_T_PONG, NULL, 0);
                atomic_fetch_add(&a->pongs, 1);
                continue;
            }
            c->out.len = c->out.done = c->out.data_bytes = 0;
            if (queue_next(a, c) < 0) {
                drop(a, c, LIVE_DROP_IO);
                break;
            }
            if (c->out.len == 0)
                break; /* nothing new: wait */
        }
        if (c->phase == C_STREAM && c->out.done < c->out.len && now - c->last_progress > a->cfg.send_timeout_us)
            drop(a, c, LIVE_DROP_STALLED);
        else if (c->phase == C_STREAM && c->out.done >= c->out.len)
            c->last_progress = now; /* idle is not a stall */
    }
    live_os_close(a->listen_fd);
    a->listen_fd = -1;
    atomic_store(&a->exited, 1);
    return NULL;
}

int live_agent_start(live_agent *a, const live_config *cfg, const live_source *src)
{
    if (!a || !cfg || !src)
        return LIVE_EINVAL;
    if (atomic_load(&a->state) != ST_IDLE)
        return LIVE_EALREADY;
    if (cfg->poll_us < 1000 || cfg->poll_us > 1000000 || cfg->send_timeout_us < 1000 ||
        cfg->send_timeout_us > 60000000 || cfg->handshake_timeout_us < 1000 ||
        cfg->handshake_timeout_us > 60000000 || cfg->stack_bytes < 16 * 1024 ||
        cfg->stack_bytes > 1024 * 1024 || cfg->cap_bytes == 0 || cfg->cap_bytes > (1ull << 40))
        return LIVE_EINVAL;
    size_t plen = src->path ? bounded_len(src->path, sizeof a->path) : 0;
    if (plen == 0 || plen >= sizeof a->path || !src->finalization || !src->app || src->app_len == 0 ||
        src->app_len > LIVE_MAX_APP || src->build_id_len > LIVE_MAX_BUILD_ID ||
        (src->build_id_len && !src->build_id))
        return LIVE_EINVAL;
    const char *base = strrchr(src->path, '/');
    base = base ? base + 1 : src->path;
    size_t blen = strlen(base);
    if (blen == 0 || blen > LIVE_MAX_NAME)
        return LIVE_EINVAL;

    a->cfg = *cfg;
    memcpy(a->path, src->path, plen + 1);
    memcpy(a->name, base, blen);
    a->name_len = (uint32_t)blen;
    memcpy(a->app, src->app, src->app_len);
    a->app_len = src->app_len;
    if (src->build_id_len)
        memcpy(a->build_id, src->build_id, src->build_id_len);
    a->build_id_len = src->build_id_len;
    a->session_id = src->session_id;
    a->finalization = src->finalization;
    a->ctx = src->ctx;
    memset(&a->conn, 0, sizeof a->conn);
    a->conn.fd = a->conn.file_fd = -1;
    a->conn.phase = C_NONE;
    atomic_store(&a->exited, 0);
    atomic_store(&a->graceful_deadline_us, 0);
    atomic_store(&a->connections, 0);
    atomic_store(&a->busy_refusals, 0);
    atomic_store(&a->bytes_delivered, 0);
    atomic_store(&a->pongs, 0);
    for (int i = 0; i < LIVE_DROP_COUNT; ++i)
        atomic_store(&a->drops[i], 0);

    int fd;
    uint16_t bound;
    if (live_os_listen(cfg->port, cfg->loopback_only, &fd, &bound) != 0)
        return LIVE_ENET;
    a->listen_fd = fd;
    a->bound_port = bound;
    atomic_store(&a->state, ST_RUNNING);
    if (pal_thread_create(&a->thread, agent_main, a, cfg->stack_bytes) != 0) {
        live_os_close(fd);
        a->listen_fd = -1;
        atomic_store(&a->state, ST_IDLE);
        return LIVE_ETHREAD;
    }
    return LIVE_OK;
}

uint16_t live_agent_port(const live_agent *a)
{
    return a->bound_port;
}

int live_agent_stop(live_agent *a, uint64_t graceful_us, uint64_t join_us)
{
    int st = atomic_load(&a->state);
    if (st != ST_RUNNING && st != ST_STOPPING)
        return LIVE_ESTATE;
    uint64_t start;
    if (now_us(&start) != 0)
        return LIVE_ECLOCK; /* nothing changed */
    if (st == ST_STOPPING)
        graceful_us = 0; /* a repeated stop only waits */
    if (graceful_us > UINT64_MAX - start || join_us > UINT64_MAX - start - graceful_us)
        return LIVE_EINVAL; /* nothing changed */
    uint64_t limit = start + graceful_us + join_us;
    if (st == ST_RUNNING) {
        atomic_store(&a->graceful_deadline_us, start + graceful_us);
        atomic_store(&a->state, ST_STOPPING);
    }
    while (!atomic_load(&a->exited)) {
        uint64_t now;
        if (now_us(&now) != 0)
            return LIVE_ECLOCK; /* still stopping; the thread keeps its state */
        if (now >= limit)
            return LIVE_ETIMEDOUT;
        pal_sleep_us(1000);
    }
    if (pal_thread_join(a->thread) != 0) {
        atomic_store(&a->state, ST_JOIN_FAILED);
        return LIVE_EJOIN;
    }
    atomic_store(&a->state, ST_STOPPED);
    return LIVE_OK;
}
