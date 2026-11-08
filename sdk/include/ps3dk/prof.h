/* <ps3dk/prof.h> -- the ps3dk-prof PPU capture runtime (format v1, docs/design/ps3dk-prof-format.md).
 *
 * Build a program with -DPS3DK_PROF=1 and link -lps3dkprof to record.  With PS3DK_PROF undefined or 0 every
 * PS3DK_PROF_* macro compiles to nothing that runs: its arguments are not evaluated (they appear only inside
 * sizeof, on the fixed-size operands this API accepts: string-literal names, ps3dk_prof_token values and
 * ps3dk_prof_zone lvalues -- never pass a variable-length-array operand), no function is referenced, and
 * PS3DK_PROF_ZONE_BEGIN yields a typed INERT handle.  The types below are declared in both builds.
 *
 * Fast path: a zone whose descriptor is already registered, on a registered thread, does not lock or allocate.
 * First use of a descriptor, thread registration, init and shutdown are cold paths and may lock.
 *
 * PPU only: an SPU translation unit with PS3DK_PROF set fails to build.
 */
#ifndef PS3DK_PROF_H
#define PS3DK_PROF_H

#include <stdint.h>

#if defined(__SPU__) && defined(PS3DK_PROF) && PS3DK_PROF
#error "ps3dk prof: SPU capture is deferred"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- results ---------------------------------------------------------------------------------------------------- */

#define PS3DK_PROF_OK              0
#define PS3DK_PROF_EPERM          (-1)   /* APP mode: not the designated drain thread */
#define PS3DK_PROF_EBUSY          (-2)   /* inside a drain, a drain already running, init or a setter running */
#define PS3DK_PROF_EALREADY       (-3)   /* init already succeeded, shutdown already called, thread already begun */
#define PS3DK_PROF_EEXIST         (-4)   /* the capture file exists (exclusive create) */
#define PS3DK_PROF_EINVAL         (-5)   /* a configuration or argument is out of range */
#define PS3DK_PROF_ENOMEM         (-6)
#define PS3DK_PROF_ENOTQUIESCED   (-7)   /* end_flags bit0 */
#define PS3DK_PROF_ESEQLIMIT      (-8)   /* end_flags bit1: a sequence or counter ceiling was reached */
#define PS3DK_PROF_ESINKRECOVERED (-9)   /* end_flags bit2: sink errors were recovered */
#define PS3DK_PROF_ESINKFAIL      (-10)  /* the sink failed; no trailer was written */
#define PS3DK_PROF_ECLOSE         (-11)  /* close failed after the trailer was written: the run FAILS */
#define PS3DK_PROF_ETHREAD        (-12)  /* thread identity, creation or join failed */
#define PS3DK_PROF_ECLOCK         (-13)  /* the timebase frequency or the monotonic clock is unavailable */
#define PS3DK_PROF_ENOTINIT       (-14)  /* the profiler is not initialized */
#define PS3DK_PROF_ESTOPPED       (-15)  /* the stop latch is set: refused */
#define PS3DK_PROF_EFULL          (-16)  /* no free thread slot, or incarnation ids exhausted */
#define PS3DK_PROF_ENOTSUP        (-17)  /* not supported here: a callback sink, a device not yet measured, an off build */
#define PS3DK_PROF_ENET           (-18)  /* live: the listener could not be created */
#define PS3DK_PROF_ETIMEDOUT      (-19)  /* live stop: the agent did not finish in time; call stop again */

#define PS3DK_PROF_MODE_APP     1u
#define PS3DK_PROF_MODE_THREAD  2u

/* ---- types (declared in every build) ----------------------------------------------------------------------------- */

/* A frame token: {owner incarnation, frame id}; owner 0 is NONE. */
typedef struct ps3dk_prof_token {
    uint32_t owner;
    uint32_t reserved;
    uint64_t frame;
} ps3dk_prof_token;

/* A call-site descriptor.  The macros declare one per site as a function-local static; the strings must outlive the
 * process (string literals and __FILE__ do).  `state` is the registration state, touched only by the runtime. */
typedef struct ps3dk_prof_desc {
    const char *name;
    const char *logical_tag;
    const char *file;
    uint32_t line;
    uint32_t scope;   /* 1 cpu, 2 gpu-complete */
    uint32_t state;   /* 0 unregistered, index + 1, or a failure code */
    uint32_t reserved;
} ps3dk_prof_desc;

/* A zone handle.  Zero-initialised is INERT. */
typedef struct ps3dk_prof_zone {
    uint32_t kind;               /* 0 INERT, 1 PUSHED, 2 REFUSED, 3 SPENT */
    uint32_t incarnation;
    uint32_t generation;
    uint32_t reserved;
    uint64_t id;                 /* span id (PUSHED) or refused-begin id (REFUSED) */
    uint64_t prev_refused_top;
    uint64_t span_top_at_begin;
} ps3dk_prof_zone;

typedef struct ps3dk_prof_sink_callbacks {
    long (*write)(void *ctx, const void *buf, unsigned long n);  /* accepted bytes in [0, n], or negative */
    int (*flush)(void *ctx);                                     /* optional: once, after the trailer */
    int (*close)(void *ctx);                                     /* once; negative fails the run */
    void *ctx;
} ps3dk_prof_sink_callbacks;

typedef struct ps3dk_prof_config {
    uint32_t mode;                /* PS3DK_PROF_MODE_APP or _THREAD */
    uint32_t max_threads;         /* 1..1024 */
    uint32_t max_descriptors;     /* 1..65536 */
    uint32_t ring_records;        /* a power of two, 512..1048576 */
    uint32_t drain_interval_us;   /* THREAD mode: 1000..1000000 */
    uint32_t drain_stack_bytes;   /* THREAD mode: 16 KiB..1 MiB */
    uint64_t quiesce_timeout_us;  /* 0..60000000 */
    uint32_t auto_register;       /* 0 (default): zones from unregistered threads are ignored */
    uint32_t reserved;
    const char *app;              /* 1..32 of [A-Za-z0-9_-]; names the default capture file */
    const char *build_id;
    const char *flags;
    const char *workload_tag;
} ps3dk_prof_config;

/* The capture a successful init created, published once and immutable for the runtime's lifetime (shutdown does
 * not change it).  Copied out by ps3dk_prof_get_capture_record; the caller owns its copy. */
typedef struct ps3dk_prof_capture_record {
    char path[256];               /* the file the FILE sink created exclusively, NUL-terminated, at most 255 bytes */
    uint64_t session_id;          /* the timebase value at init (also in the file name) */
    char app[33];                 /* NUL-terminated */
    uint32_t app_len;
    uint8_t build_id[512];        /* length-delimited: build_id_len bytes, not necessarily NUL-terminated */
    uint32_t build_id_len;
} ps3dk_prof_capture_record;

/* Live link configuration (docs/design/ps3dk-prof-viewer.md, Phase 2, "Target integration").  A zero field takes
 * its default; a NULL config takes every default.  Out-of-range values and nonzero reserved fields are EINVAL. */
typedef struct ps3dk_prof_live_config {
    uint16_t port;                 /* TCP port on every interface; default 31750 */
    uint16_t reserved;             /* 0 */
    uint32_t poll_us;              /* 1000..1000000; default 10000: the agent's wait between checks */
    uint32_t send_timeout_us;      /* 1000..60000000; default 2000000: no send progress for this long drops a viewer */
    uint32_t handshake_timeout_us; /* 1000..60000000; default 2000000: the viewer's hello must arrive within this */
    uint32_t stack_bytes;          /* 16 KiB..1 MiB; default 64 KiB: the agent thread's stack */
    uint32_t reserved2;            /* 0 */
    uint64_t cap_bytes;            /* 1..2^40: the most capture bytes served to one viewer; default the agent's cap */
} ps3dk_prof_live_config;

/* The live agent's final counters (EXPERIMENTAL, like the rest of the live API), copied out by
 * ps3dk_prof_live_get_stats after a successful stop.  Every count covers the whole run, all viewers together.
 * Each ADMITTED connection (counted in connections) that has finished is counted in exactly one drop_* field: the
 * reason it ended.  Busy refusals and connections that failed during setup are not admitted and have no drop. */
typedef struct ps3dk_prof_live_stats {
    uint32_t version;                /* 1 */
    uint32_t reserved;               /* 0 */
    uint64_t connections;            /* connections admitted (each finished one ends in one drop_* reason) */
    uint64_t busy_refusals;          /* connections refused because a viewer was being served or the agent stopping */
    uint64_t bytes_delivered;        /* capture bytes in DATA frames the socket fully accepted */
    uint64_t pongs;                  /* PING answers queued */
    uint64_t drop_end;               /* the socket accepted END (not proof the viewer kept the file) */
    uint64_t drop_handshake_timeout; /* no CLIENT_HELLO in time */
    uint64_t drop_bad_hello;         /* a CLIENT_HELLO that was not acceptable */
    uint64_t drop_protocol;          /* an unknown type, nonzero flags, or a bad or oversize length */
    uint64_t drop_ping_flood;        /* too many PINGs in a window */
    uint64_t drop_stalled;           /* no send progress within send_timeout_us */
    uint64_t drop_peer_closed;       /* the viewer closed, a receive failed, or a send failed or accepted nothing */
    uint64_t drop_io;                /* the capture file could not be opened or read */
    uint64_t drop_cancelled;         /* stop's graceful interval ran out */
    uint64_t drop_clock;             /* the monotonic clock failed */
} ps3dk_prof_live_stats;

#ifdef __cplusplus
#define PS3DK_PROF_TOKEN_NONE ps3dk_prof_token{}
#define PS3DK_PROF_INERT_ZONE ps3dk_prof_zone{}
#else
#define PS3DK_PROF_TOKEN_NONE ((ps3dk_prof_token){0u, 0u, 0u})
#define PS3DK_PROF_INERT_ZONE ((ps3dk_prof_zone){0u, 0u, 0u, 0u, 0u, 0u, 0u})
#endif

#define PS3DK_PROF__CAT2(a, b) a##b
#define PS3DK_PROF__CAT(a, b) PS3DK_PROF__CAT2(a, b)

#if defined(PS3DK_PROF) && PS3DK_PROF

/* ---- API (enabled build) ----------------------------------------------------------------------------------------- */

void ps3dk_prof_config_defaults(ps3dk_prof_config *cfg);
/* Before init only (EBUSY while a setter or init runs, EALREADY after).  The last successful setter wins.
 * set_sink_path names the capture DIRECTORY (1..255 bytes): the file is <dir>/<app>-<16 hex digits of the init
 * timebase>.prof, created exclusively (an existing file is EEXIST; a name over 255 bytes is EINVAL at init).
 * Callbacks run inside a drain; a callback that blocks is NOT bounded by quiesce_timeout_us: the drain, and
 * shutdown, wait for it. */
int ps3dk_prof_set_sink_path(const char *path);
int ps3dk_prof_set_sink_callbacks(const ps3dk_prof_sink_callbacks *cb);
int ps3dk_prof_init(const ps3dk_prof_config *cfg);
int ps3dk_prof_shutdown(void);
int ps3dk_prof_drain(void);
int ps3dk_prof_thread_begin(const char *name);
void ps3dk_prof_thread_end(void);
void ps3dk_prof_frame(void);
ps3dk_prof_token ps3dk_prof_frame_token(void);
ps3dk_prof_zone ps3dk_prof_zone_begin(ps3dk_prof_desc *desc, ps3dk_prof_token token);
void ps3dk_prof_zone_end(ps3dk_prof_zone *zone);
/* Copies the capture record.  OK; EINVAL for a NULL out; ENOTINIT before a successful init (a failed init, retryable
 * or terminal, publishes nothing); ENOTSUP with a callback sink (no file).  On any error out is left untouched. */
int ps3dk_prof_get_capture_record(ps3dk_prof_capture_record *out);
/* Live link (docs/design/ps3dk-prof-viewer.md, Phase 2): one agent thread serves this run's FILE-sink capture to
 * one PC viewer over TCP while it grows, reading the file only (the writer never waits for a viewer).  Manual
 * lifecycle only (libps3dkprof); the network must be initialized first (netInitialize).  Calls are caller-serialized:
 * init, live_start, shutdown and live_stop are never concurrent.
 * live_start, after a successful init: OK; ENOTINIT before one; ESTOPPED once shutdown has begun; ENOTSUP with a
 * callback sink or a capture outside /dev_hdd0/ (the only device measured); EALREADY after a start (once per run);
 * EINVAL for a bad config; ENET if the listener cannot be created; ETHREAD if the thread cannot be.
 * live_stop: lets a streaming viewer finish (END needs the shutdown's finalization) for up to graceful_us, then
 * cancels it, and waits up to graceful_us + join_us for the agent thread to leave its loop.  OK (stopped);
 * ETIMEDOUT or ECLOCK (still stopping: call stop again; keep the network initialized meanwhile); ETHREAD (the join
 * failed: terminal, never retried, and the network must NOT be torn down, since the thread's state is unknown);
 * EALREADY after a successful stop; ENOTINIT if never started; EINVAL if the deadline would overflow (nothing
 * changes).  A graceful_us of 0 cancels at once.  What the deadlines do NOT bound: an LV2 file call already in
 * progress, and the final ordinary join once the thread has left its loop (it waits only for the thread to return,
 * but no API bounds that wait). */
int ps3dk_prof_live_start(const ps3dk_prof_live_config *cfg);
int ps3dk_prof_live_stop(uint64_t graceful_us, uint64_t join_us);
/* live_get_stats (EXPERIMENTAL), caller-serialized with live_start and live_stop: OK copies the final counters once
 * a live_stop has succeeded (the agent thread is joined), and again on every later call: reading never clears them,
 * and they stay readable after ps3dk_prof_shutdown.  EINVAL for a NULL out; ENOTINIT if no live_start succeeded;
 * EBUSY while the agent runs or is still stopping (before a successful stop, including after ETIMEDOUT or ECLOCK);
 * ETHREAD after a failed join (the counters are not final).  On any error out is left untouched. */
int ps3dk_prof_live_get_stats(ps3dk_prof_live_stats *out);

#define PS3DK_PROF__DESC(var, nm) \
    static ps3dk_prof_desc var = {(nm), "", __FILE__, (uint32_t)__LINE__, 1u, 0u, 0u}

#define PS3DK_PROF_ZONE_BEGIN(name)                                                     \
    (__extension__({                                                                    \
        PS3DK_PROF__DESC(ps3dk_prof__d, name);                                         \
        ps3dk_prof_zone_begin(&ps3dk_prof__d, PS3DK_PROF_TOKEN_NONE);                  \
    }))
#define PS3DK_PROF_ZONE_BEGIN_FOR(name, tok)                                            \
    (__extension__({                                                                    \
        PS3DK_PROF__DESC(ps3dk_prof__d, name);                                         \
        ps3dk_prof_zone_begin(&ps3dk_prof__d, (tok));                                  \
    }))
#define PS3DK_PROF_ZONE_END(h) ps3dk_prof_zone_end(&(h))

#define PS3DK_PROF_INIT(cfg) ps3dk_prof_init(cfg)
#define PS3DK_PROF_SHUTDOWN() ps3dk_prof_shutdown()
#define PS3DK_PROF_DRAIN() ps3dk_prof_drain()
#define PS3DK_PROF_THREAD_BEGIN(name) ps3dk_prof_thread_begin(name)
#define PS3DK_PROF_THREAD_END() ps3dk_prof_thread_end()
#define PS3DK_PROF_FRAME() ps3dk_prof_frame()
#define PS3DK_PROF_FRAME_TOKEN() ps3dk_prof_frame_token()
#define PS3DK_PROF_GET_CAPTURE_RECORD(out) ps3dk_prof_get_capture_record(out)
#define PS3DK_PROF_LIVE_START(cfg) ps3dk_prof_live_start(cfg)
#define PS3DK_PROF_LIVE_STOP(graceful_us, join_us) ps3dk_prof_live_stop(graceful_us, join_us)
#define PS3DK_PROF_LIVE_GET_STATS(out) ps3dk_prof_live_get_stats(out)

#ifndef __cplusplus
static inline void ps3dk_prof__cleanup(ps3dk_prof_zone *z) { ps3dk_prof_zone_end(z); }
#define PS3DK_PROF_ZONE_FOR(name, tok)                                                          \
    PS3DK_PROF__DESC(PS3DK_PROF__CAT(ps3dk_prof__d, __LINE__), name);                           \
    ps3dk_prof_zone PS3DK_PROF__CAT(ps3dk_prof__z, __LINE__)                                    \
        __attribute__((cleanup(ps3dk_prof__cleanup), unused)) =                                 \
            ps3dk_prof_zone_begin(&PS3DK_PROF__CAT(ps3dk_prof__d, __LINE__), (tok))
#define PS3DK_PROF_ZONE(name) PS3DK_PROF_ZONE_FOR(name, PS3DK_PROF_TOKEN_NONE)
#endif

#else /* ---- off build: nothing runs, nothing is referenced ------------------------------------------------------- */

#define PS3DK_PROF_ZONE_BEGIN(name) ((void)sizeof(name), PS3DK_PROF_INERT_ZONE)
#define PS3DK_PROF_ZONE_BEGIN_FOR(name, tok) ((void)sizeof(name), (void)sizeof(tok), PS3DK_PROF_INERT_ZONE)
#define PS3DK_PROF_ZONE_END(h) ((void)sizeof(h))
#define PS3DK_PROF_ZONE(name) ((void)sizeof(name))
#define PS3DK_PROF_ZONE_FOR(name, tok) ((void)sizeof(name), (void)sizeof(tok))
#define PS3DK_PROF_INIT(cfg) ((void)sizeof(cfg), PS3DK_PROF_OK)
#define PS3DK_PROF_SHUTDOWN() (PS3DK_PROF_OK)
#define PS3DK_PROF_DRAIN() (PS3DK_PROF_OK)
#define PS3DK_PROF_THREAD_BEGIN(name) ((void)sizeof(name), PS3DK_PROF_OK)
#define PS3DK_PROF_THREAD_END() ((void)0)
#define PS3DK_PROF_FRAME() ((void)0)
#define PS3DK_PROF_FRAME_TOKEN() (PS3DK_PROF_TOKEN_NONE)
#define PS3DK_PROF_GET_CAPTURE_RECORD(out) ((void)sizeof(out), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_LIVE_START(cfg) ((void)sizeof(cfg), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_LIVE_STOP(graceful_us, join_us) ((void)sizeof(graceful_us), (void)sizeof(join_us), PS3DK_PROF_ENOTSUP)
#define PS3DK_PROF_LIVE_GET_STATS(out) ((void)sizeof(out), PS3DK_PROF_ENOTSUP)

#endif

#ifdef __cplusplus
}

#if defined(PS3DK_PROF) && PS3DK_PROF
namespace ps3dk_prof {
/* Ends its zone at scope exit.  Neither copyable nor movable: a copy would end the same zone twice. */
class zone_guard {
public:
    zone_guard(ps3dk_prof_desc *d, ps3dk_prof_token t) : z_(ps3dk_prof_zone_begin(d, t)) {}
    ~zone_guard() { ps3dk_prof_zone_end(&z_); }
    zone_guard(const zone_guard &) = delete;
    zone_guard &operator=(const zone_guard &) = delete;
    zone_guard(zone_guard &&) = delete;
    zone_guard &operator=(zone_guard &&) = delete;

private:
    ps3dk_prof_zone z_;
};
} /* namespace ps3dk_prof */

#define PS3DK_PROF_ZONE_FOR(name, tok)                                                          \
    PS3DK_PROF__DESC(PS3DK_PROF__CAT(ps3dk_prof__d, __LINE__), name);                           \
    ::ps3dk_prof::zone_guard PS3DK_PROF__CAT(ps3dk_prof__g, __LINE__)(                          \
        &PS3DK_PROF__CAT(ps3dk_prof__d, __LINE__), (tok))
#define PS3DK_PROF_ZONE(name) PS3DK_PROF_ZONE_FOR(name, PS3DK_PROF_TOKEN_NONE)
#endif
#endif /* __cplusplus */

#endif /* PS3DK_PROF_H */
