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
