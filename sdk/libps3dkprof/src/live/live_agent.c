/* Live link agent. See live_agent.h for the contract and live_proto.h for
 * the wire schema. Every socket, poll and file call goes through the OS seam
 * (live_os.h); this file holds only the protocol and the state machine. */
#include "live_agent.h"
#include "live_os.h"
#include <ps3dk/prof.h> /* the PS3DK_PROF_* codes a CAPTURE_ACK carries */
#include <string.h>

#ifdef PS3DK_PROF_TEST_HOOKS
live_test_hooks ps3dk_prof_test_live_hooks;
#endif

/* Agent states. JOIN_FAILED is terminal: the handle is consumed. */
enum { ST_IDLE, ST_RUNNING, ST_STOPPING, ST_STOPPED, ST_JOIN_FAILED };
enum { C_NONE, C_HANDSHAKE, C_STREAM, C_DRAIN };

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
        .cmd_timeout_us = 5000000,
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
    /* A command of this link that no frame mark took yet is withdrawn; one
     * being applied finishes in the source, which discards its result. */
    if (c->cmd_queued)
        (void)a->cmd->cancel(a->cmd->ctx);
    /* Count first: a peer that sees the close may read the counters. */
    /* END was already counted before the write half-close. Draining its
     * remaining input does not end a second stream. */
    if (c->phase != C_DRAIN)
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
    live_put16(p, c->version); /* the version the viewer asked for */
    live_put16(p + 2, LIVE_AGENT_VERSION);
    live_put32(p + 4, (c->version >= 2 && a->cmd ? LIVE_CAP_TRIGGERS : 0u) |
                      (c->version >= 3 && a->rsxf && a->rsxf->available() ? LIVE_CAP_RSX_FRAME : 0u)); /* v1: 0 */
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

/* After HELLO: a PING (empty) or, in v2, a command within its layout's bounds. */
static int stream_len_ok(const live_conn *c, uint32_t type, uint32_t len)
{
    if (type == LIVE_T_PING)
        return len == 0;
    if (c->version < 2)
        return 0;
    switch (type) {
    case LIVE_T_CAPTURE_BEGIN:
        return len >= 8u + 1u && len <= 8u + LIVE_MAX_WINDOW_NAME;
    case LIVE_T_CAPTURE_END:
        return len == 4u;
    case LIVE_T_CAPTURE_FRAMES:
        return len >= 12u + 1u && len <= 12u + LIVE_MAX_WINDOW_NAME;
    case LIVE_T_RSX_FRAME_REQ:
        return c->version >= 3 && len >= 12u && len <= 12u + LIVE_MAX_WINDOW_NAME;
    default:
        return 0;
    }
}

static void stage_ack(live_conn *c, int32_t result, uint32_t window_id)
{
    live_put32(c->ack, c->cmd_req);
    live_put32(c->ack + 4, (uint32_t)result);
    live_put32(c->ack + 8, result == 0 ? window_id : 0u);
    live_put16(c->ack + 12, c->cmd_type);
    live_put16(c->ack + 14, 0);
    c->ack_ready = 1;
    c->cmd_queued = 0;
}

/* RSX_FRAME_REQ (v3): answered at once; the accepted capture's progress follows as STATE. */
static int on_rsx_frame(live_agent *a, live_conn *c)
{
    const uint8_t *p = c->in + LIVE_HDR_BYTES;
    uint32_t len = live_get32(c->in + 4), req = live_get32(p), flags = live_get16(p + 4);
    uint32_t frames = live_get16(p + 6), name_len = live_get16(p + 8), id = 0;
    int rc;
    if (req == 0 || frames != 1 || live_get16(p + 10) != 0 || len != 12u + name_len || c->rsxf_ack_ready)
        return -1;
    if (c->end_queued)
        return 0;
    /* one followed capture per link: a request before the previous capture's final STATE is sent would lose it */
    rc = c->rsxf_id ? PS3DK_PROF_EBUSY
         : a->rsxf && a->rsxf->available() ? a->rsxf->request(req, flags, (const char *)p + 12, name_len, &id)
                                           : PS3DK_PROF_ENOTSUP;
    live_put32(c->rsxf_ack, req);
    live_put32(c->rsxf_ack + 4, (uint32_t)rc);
    live_put32(c->rsxf_ack + 8, rc == 0 ? id : 0u);
    live_put32(c->rsxf_ack + 12, 0);
    c->rsxf_ack_ready = 1;
    if (rc == 0) {
        c->rsxf_id = id;
        c->rsxf_sent_state = 0;
    }
    return 0;
}

/* The followed capture's state, when it changed since the last STATE: 1 with the frame staged in out. */
static int rsxf_state_frame(live_agent *a, live_conn *c, uint8_t out[LIVE_RSXF_STATE_BYTES])
{
    live_rsxf_status s;
    if (!c->rsxf_id || !a->rsxf || c->end_queued)
        return 0;
    a->rsxf->status(&s);
    if (s.id != c->rsxf_id || s.state < 1 || s.state > 4 || s.state == c->rsxf_sent_state)
        return 0;
    memset(out, 0, LIVE_RSXF_STATE_BYTES);
    live_put32(out, s.id);
    out[4] = (uint8_t)s.state;
    out[5] = (uint8_t)s.reason;
    live_put32(out + 8, s.draws);
    live_put32(out + 12, s.request);
    live_put64(out + 16, s.frame);
    live_put64(out + 24, s.bytes);
    c->rsxf_sent_state = s.state;
    if (s.state >= 3)
        c->rsxf_id = 0; /* final */
    return 1;
}

/* A complete command frame in c->in (its length already bounded by
 * stream_len_ok). Returns -1 for a protocol violation. */
static int on_command(live_agent *a, live_conn *c, uint64_t now)
{
    const uint8_t *p = c->in + LIVE_HDR_BYTES;
    uint32_t type = live_get16(c->in), len = live_get32(c->in + 4);
    uint32_t req = live_get32(p), frames = 0, name_len = 0;
    const char *name = NULL;
    if (type == LIVE_T_RSX_FRAME_REQ)
        return on_rsx_frame(a, c);
    if (type == LIVE_T_CAPTURE_BEGIN) {
        name_len = live_get16(p + 4);
        if (live_get16(p + 6) != 0 || len != 8u + name_len)
            return -1;
        name = (const char *)p + 8;
    } else if (type == LIVE_T_CAPTURE_FRAMES) {
        frames = live_get32(p + 4);
        name_len = live_get16(p + 8);
        if (live_get16(p + 10) != 0 || len != 12u + name_len || frames < 1 || frames > LIVE_MAX_WINDOW_FRAMES)
            return -1;
        name = (const char *)p + 12;
    }
    /* one command at a time: the next only after the ACK of the last */
    if (req == 0 || c->cmd_queued || c->ack_ready)
        return -1;
    if (c->end_queued)
        return 0; /* END is final: no ACK may follow it; the viewer sees the stream end instead */
    c->cmd_req = req;
    c->cmd_type = type;
    if (!a->cmd) {
        stage_ack(c, PS3DK_PROF_ENOTSUP, 0);
        return 0;
    }
    int rc = a->cmd->submit(a->cmd->ctx, type, name, name_len, frames);
    if (rc != 0) {
        stage_ack(c, rc, 0);
        return 0;
    }
    c->cmd_queued = 1;
    c->cmd_deadline = now + a->cfg.cmd_timeout_us;
    return 0;
}

/* The outstanding command: finished, withdrawn at its deadline, or still waiting. */
static void service_command(live_agent *a, live_conn *c, uint64_t now)
{
    int32_t result;
    uint32_t id;
    if (!c->cmd_queued)
        return;
    if (a->cmd->poll(a->cmd->ctx, &result, &id)) {
        stage_ack(c, result, id);
        return;
    }
    if (now >= c->cmd_deadline && a->cmd->cancel(a->cmd->ctx))
        stage_ack(c, PS3DK_PROF_ETIMEDOUT, 0); /* no frame mark took it: nothing was applied */
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
            if (c->phase == C_HANDSHAKE ? type != LIVE_T_CLIENT_HELLO || len != 8 : !stream_len_ok(c, type, len))
                return c->phase == C_HANDSHAKE ? LIVE_DROP_BAD_HELLO : LIVE_DROP_PROTOCOL;
        }
        if (c->in_len >= LIVE_HDR_BYTES && c->in_len == need) {
            if (c->phase == C_HANDSHAKE) {
                const uint8_t *p = c->in + LIVE_HDR_BYTES;
                /* A hello that completes after the deadline is still late. */
                if (now >= c->handshake_deadline)
                    return LIVE_DROP_HANDSHAKE_TIMEOUT;
                uint32_t version = live_get16(p + 4);
                if (live_get32(p) != LIVE_MAGIC || version < LIVE_VERSION_MIN || version > LIVE_VERSION ||
                    live_get16(p + 6) != 0)
                    return LIVE_DROP_BAD_HELLO;
                c->version = version;
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
            } else if (live_get16(c->in) != LIVE_T_PING) {
                if (on_command(a, c, now) != 0)
                    return LIVE_DROP_PROTOCOL;
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
    live_os_thread_enter(); /* a stack measurement build paints here; otherwise nothing */
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
        if (stopping && ((c->phase != C_STREAM && c->phase != C_DRAIN) || cancel_due(a, now))) {
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
        if (c->phase == C_DRAIN) {
            /* Closing with unread PING bytes sends a TCP reset. END is
             * terminal: send FIN, then consume input until the peer closes.
             * One bounded read per poll keeps even a flooding peer subject
             * to the fixed deadline; input never extends that deadline. */
            if (now - c->last_progress >= a->cfg.send_timeout_us) {
                drop(a, c, LIVE_DROP_STALLED);
            } else {
                int32_t n = live_os_recv(c->fd, c->out.bytes, sizeof c->out.bytes);
                if (n < 0 && n != LIVE_OS_AGAIN)
                    drop(a, c, LIVE_DROP_END);
            }
        }
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
        if (c->phase == C_NONE || c->phase == C_DRAIN)
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
                /* Preserve drop_end's meaning: END was fully sent. Count
                 * before the FIN the peer observes, as the old close did. */
                atomic_fetch_add(&a->drops[LIVE_DROP_END], 1);
                c->phase = C_DRAIN;
                c->last_progress = now;
                live_os_close(c->file_fd);
                c->file_fd = -1;
                if (live_os_shutdown_write(c->fd) != 0)
                    drop(a, c, LIVE_DROP_PEER_CLOSED);
                break;
            }
            if (c->pongs_pending) {
                c->pongs_pending--;
                queue_frame(&c->out, LIVE_T_PONG, NULL, 0);
                atomic_fetch_add(&a->pongs, 1);
                continue;
            }
            if (c->rsxf_ack_ready) {
                c->rsxf_ack_ready = 0;
                queue_frame(&c->out, LIVE_T_RSX_FRAME_ACK, c->rsxf_ack, LIVE_RSXF_ACK_BYTES);
                if (live_get32(c->rsxf_ack + 4) != 0)
                    atomic_fetch_add(&a->ack_refusals, 1);
                atomic_fetch_add(&a->acks, 1);
                continue;
            }
            {
                uint8_t st[LIVE_RSXF_STATE_BYTES];
                if (rsxf_state_frame(a, c, st)) {
                    queue_frame(&c->out, LIVE_T_RSX_FRAME_STATE, st, LIVE_RSXF_STATE_BYTES);
                    continue;
                }
            }
            service_command(a, c, now);
            if (c->ack_ready) {
                c->ack_ready = 0;
                queue_frame(&c->out, LIVE_T_CAPTURE_ACK, c->ack, LIVE_ACK_BYTES);
                if (live_get32(c->ack + 4) != 0)
                    atomic_fetch_add(&a->ack_refusals, 1);
                atomic_fetch_add(&a->acks, 1); /* before the frame the peer observes */
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
    live_os_thread_exit(); /* the measurement scan, before the thread's last store */
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
        cfg->stack_bytes > 1024 * 1024 || cfg->cap_bytes == 0 || cfg->cap_bytes > (1ull << 40) ||
        cfg->cmd_timeout_us < 1000 || cfg->cmd_timeout_us > 60000000)
        return LIVE_EINVAL;
    size_t plen = src->path ? bounded_len(src->path, sizeof a->path) : 0;
    if (plen == 0 || plen >= sizeof a->path || !src->finalization || !src->app || src->app_len == 0 ||
        src->app_len > LIVE_MAX_APP || src->build_id_len > LIVE_MAX_BUILD_ID ||
        (src->build_id_len && !src->build_id) ||
        (src->cmd && (!src->cmd->submit || !src->cmd->poll || !src->cmd->cancel)) ||
        (src->rsxf && (!src->rsxf->request || !src->rsxf->status || !src->rsxf->available)))
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
    a->cmd = src->cmd;
    a->rsxf = src->rsxf;
    memset(&a->conn, 0, sizeof a->conn);
    a->conn.fd = a->conn.file_fd = -1;
    a->conn.phase = C_NONE;
    atomic_store(&a->exited, 0);
    atomic_store(&a->graceful_deadline_us, 0);
    atomic_store(&a->connections, 0);
    atomic_store(&a->busy_refusals, 0);
    atomic_store(&a->bytes_delivered, 0);
    atomic_store(&a->pongs, 0);
    atomic_store(&a->acks, 0);
    atomic_store(&a->ack_refusals, 0);
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
