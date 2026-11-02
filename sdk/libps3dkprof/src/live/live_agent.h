/* Live link agent: serves a capture FILE that another writer is growing to
 * one viewer over TCP (schema: live_proto.h). Its OS calls go through
 * live_os.h; only the host shim (live_os_host.c) exists so far. The runtime
 * hook-up, the public API and the LV2 shim come in the target checkpoint.
 *
 * The agent only reads the file. It never touches drains, rings or sink
 * callbacks, so a writer never waits on a viewer; the agent still costs
 * CPU, memory and I/O, which the tests measure.
 *
 * Ownership:
 * - Calls are caller-serialized: one controlling thread calls start and
 *   stop.
 * - The caller provides a ZERO-INITIALIZED live_agent (static storage is
 *   the intended home) and keeps it, the source's ctx and whatever the
 *   finalization callback returns valid until stop returns LIVE_OK. After
 *   LIVE_ETIMEDOUT, LIVE_ECLOCK or LIVE_EJOIN the agent thread may still
 *   use them, so they are never freed.
 * - All connection state lives in the agent object, not on the agent
 *   thread's stack (the thread itself needs little stack). The host PAL
 *   floors thread stacks at 256 KiB; the target's stack and storage budget
 *   is a gate of the target checkpoint. */
#ifndef PS3DK_PROF_LIVE_AGENT_H
#define PS3DK_PROF_LIVE_AGENT_H
#include <stdatomic.h>
#include <stdint.h>
#include "live_proto.h"
#include "pal.h"
#include "prof_finalization.h"

#define LIVE_OK 0
#define LIVE_EINVAL (-1)    /* bad config or source; stop: deadline arithmetic would overflow */
#define LIVE_EALREADY (-2)  /* start on an agent that was started */
#define LIVE_ESTATE (-3)    /* stop on an agent that is not running or stopping */
#define LIVE_ENET (-4)      /* the listener could not be created */
#define LIVE_ETHREAD (-5)   /* the agent thread could not be created */
#define LIVE_ETIMEDOUT (-6) /* stop: the thread did not exit in time; call stop again later */
#define LIVE_ECLOCK (-7)    /* stop: the monotonic clock failed; nothing changed hands; call stop again */
#define LIVE_EJOIN (-8)     /* stop: the thread exited its loop but the join failed; terminal, not retried */

typedef struct live_config {
    uint16_t port;                 /* 0: an ephemeral port (tests); see live_agent_port */
    uint8_t loopback_only;         /* bind 127.0.0.1 instead of every interface */
    uint8_t reserved;
    uint32_t poll_us;              /* 1000..1000000: wait between checks of the file and sockets */
    uint32_t send_timeout_us;      /* 1000..60000000: no send progress for this long drops the viewer */
    uint32_t handshake_timeout_us; /* 1000..60000000: CLIENT_HELLO must arrive within this */
    uint32_t stack_bytes;          /* 16 KiB..1 MiB */
    uint64_t cap_bytes;            /* 1..2^40: bytes of capture served at most */
} live_config;

/* Where the capture is and how to know it is finished. finalization returns
 * NULL until the winning shutdown has published its record (the runtime's
 * ps3dk_prof__finalization); the host tests inject one. */
typedef struct live_source {
    const char *path;          /* 1..1023 bytes */
    uint64_t session_id;
    const char *app;           /* 1..LIVE_MAX_APP bytes */
    uint32_t app_len;
    const char *build_id;      /* 0..LIVE_MAX_BUILD_ID bytes */
    uint32_t build_id_len;
    const struct ps3dk_prof_finalization *(*finalization)(void *ctx);
    void *ctx;
} live_source;

/* Why a viewer connection ended. */
enum live_drop {
    LIVE_DROP_END = 0,        /* END sent: the stream completed */
    LIVE_DROP_HANDSHAKE_TIMEOUT,
    LIVE_DROP_BAD_HELLO,
    LIVE_DROP_PROTOCOL,       /* unknown type, nonzero flags, bad or oversize length */
    LIVE_DROP_PING_FLOOD,
    LIVE_DROP_STALLED,        /* no send progress within send_timeout_us */
    LIVE_DROP_PEER_CLOSED,
    LIVE_DROP_IO,             /* the capture file could not be opened or read */
    LIVE_DROP_CANCELLED,      /* stop's graceful interval ran out */
    LIVE_DROP_CLOCK,          /* the monotonic clock failed: deadlines cannot be kept, the agent exits */
    LIVE_DROP_COUNT
};

/* One frame being sent: header plus the largest payload. */
typedef struct live_out {
    uint8_t bytes[LIVE_HDR_BYTES + LIVE_MAX_PAYLOAD];
    uint32_t len, done;
    uint32_t data_bytes;      /* capture bytes in this frame if it is DATA, else 0 */
} live_out;

typedef struct live_conn {
    int fd, file_fd, phase;
    uint8_t in[LIVE_HDR_BYTES + 8]; /* a viewer only sends CLIENT_HELLO (8 bytes) and PING (empty) */
    uint32_t in_len;
    live_out out;
    uint32_t pongs_pending;
    uint64_t sent;            /* DATA bytes queued so far (== next offset) */
    uint64_t handshake_deadline, last_progress, ping_window_start;
    uint32_t pings_in_window;
    int end_queued;
} live_conn;

typedef struct live_agent {
    live_config cfg;
    char path[1024];
    char name[LIVE_MAX_NAME + 1];
    uint32_t name_len;
    char app[LIVE_MAX_APP];
    uint32_t app_len;
    char build_id[LIVE_MAX_BUILD_ID];
    uint32_t build_id_len;
    uint64_t session_id;
    const struct ps3dk_prof_finalization *(*finalization)(void *ctx);
    void *ctx;
    int listen_fd;
    uint16_t bound_port;
    live_conn conn;
    pal_thread thread;
    atomic_int state;            /* see live_agent.c */
    /* Set as the thread's last action before it returns. It shows the loop
     * is over and the sockets are closed; the join that follows waits only
     * for that return, which no API bounds by a deadline. */
    atomic_int exited;
    atomic_ullong graceful_deadline_us;
    /* Counters, read by tests and by stop's caller. bytes_delivered counts
     * DATA payload whose frame the socket fully accepted. */
    atomic_ullong connections, busy_refusals, bytes_delivered, pongs;
    atomic_ullong drops[LIVE_DROP_COUNT];
} live_agent;

void live_config_defaults(live_config *cfg);

/* Validates, copies the source, binds the listener and starts the thread. */
int live_agent_start(live_agent *a, const live_config *cfg, const live_source *src);

/* The port actually bound (useful with port 0). Valid after a successful start. */
uint16_t live_agent_port(const live_agent *a);

/* Graceful stop, then cancellation, then a bounded wait for the thread.
 * - A viewer that is streaming keeps receiving until END has been sent (it
 *   needs the finalization record) or graceful_us runs out. Then the agent
 *   cancels: it closes the viewer without END. A partial download is never
 *   claimed as complete. A cancellation that is due when a blocking file
 *   call returns still cancels.
 * - Without a streaming viewer the thread exits at its next poll.
 * - The caller waits up to graceful_us + join_us in all, then joins.
 * Returns LIVE_OK (joined, stopped), LIVE_EINVAL (the deadline would
 * overflow; nothing changed), LIVE_ETIMEDOUT or LIVE_ECLOCK (still
 * stopping; call stop again, which only waits), LIVE_EJOIN (terminal; the
 * join is not retried), or LIVE_ESTATE.
 * A graceful_us of 0 cancels at once. */
int live_agent_stop(live_agent *a, uint64_t graceful_us, uint64_t join_us);

#ifdef PS3DK_PROF_TEST_HOOKS
/* Faults the tests inject while the agent thread runs, so every field is
 * atomic. (The PAL's join_fail is only read by the controlling thread.) */
typedef struct live_test_hooks {
    atomic_int clock_fail;         /* the agent's monotonic clock reads fail */
    atomic_int busy_nonblock_fail; /* the refused socket cannot be made non-blocking */
    atomic_int busy_send_eagain;   /* the one BUSY send attempt returns EAGAIN */
    atomic_int pread_block;        /* the next file read waits until pread_release (then clears) */
    atomic_int pread_entered;      /* set when a file read is held */
    atomic_int pread_release;
} live_test_hooks;
extern live_test_hooks ps3dk_prof_test_live_hooks; /* the archive check refuses ps3dk_prof_test_ symbols */
#endif

#endif
