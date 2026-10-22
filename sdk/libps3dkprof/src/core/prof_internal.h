/* ps3dk-prof runtime internals.  Contract: docs/design/ps3dk-prof-format.md (sections 5, 7, 8) with
 * STAGE3-PLAN-r2 + ERRATUM-1 + ERRATUM-2 (later supersedes earlier).  Section references below are to the format. */
#ifndef PS3DK_PROF_INTERNAL_H
#define PS3DK_PROF_INTERNAL_H

#ifndef PS3DK_PROF
#define PS3DK_PROF 1
#endif
#if !PS3DK_PROF
#error "the ps3dk-prof library is always built with PS3DK_PROF=1"
#endif
#include <ps3dk/prof.h>

#include <stddef.h>
#include <stdint.h>

#include "../pal/pal.h"

/* ---- atomics (GCC builtins only; no __sync, no hand-written reservations) ---------------------------------------- */
#define LD_ACQ(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define LD_RLX(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define ST_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define ST_RLX(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define CAS_ACQREL(p, expp, des) \
    __atomic_compare_exchange_n((p), (expp), (des), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

_Static_assert(__atomic_always_lock_free(4, 0), "ps3dk prof: 4-byte atomics must be lock-free");
_Static_assert(__atomic_always_lock_free(8, 0), "ps3dk prof: 8-byte atomics must be lock-free");

/* ---- format constants -------------------------------------------------------------------------------------------- */
#define P_CEIL            (1ull << 63)   /* sequences < P_CEIL; counters and ids <= P_CEIL (section 5.1) */
#define P_REC             32u
#define P_MAX_DEPTH       256u           /* depths 0..255 */
#define P_MAX_PUB         257u           /* the largest publication: GAP + a 256-END STOP batch */
#define P_CHUNK_CAP       1048576u       /* payload bytes */
#define P_CHUNK_RECORDS   ((P_CHUNK_CAP - 8u) / P_REC)   /* 32767 */
#define P_STR_MAX         512u
#define P_UNKNOWN         0xFFFFFFFFFFFFFFFFull
#define P_FNV_BASIS       0xcbf29ce484222325ull

#ifndef PS3DK_PROF_SDK_VERSION
#define PS3DK_PROF_SDK_VERSION "unknown"   /* the SDK Makefile passes the release version */
#endif

enum { K_BEGIN = 1, K_BEGIN_PARENT = 2, K_END = 3, K_FRAME = 4, K_GAP = 5, K_REGERR = 6 };
enum { C_HEADER = 1, C_CLOCKS = 2, C_DESCRIPTORS = 3, C_THREADS = 4, C_RECORDS = 5, C_TRAILER = 6, C_RETIRED = 7 };
enum { ST_RETIRED = 1, ST_ACKED = 2, ST_NOT_QUIESCED = 3 };       /* final-entry states (section 6) */
enum { ZK_INERT = 0, ZK_PUSHED = 1, ZK_REFUSED = 2, ZK_SPENT = 3 }; /* zone handle kinds */

/* Call-site descriptor state values beyond index + 1. */
#define DESC_FAILED_TABLE_FULL 0xFFFFFFF1u
#define DESC_FAILED_COLLISION  0xFFFFFFF2u
#define DESC_FAILED_INVALID    0xFFFFFFF3u   /* empty name, bad scope or invalid UTF-8: refused, never written */

/* Slot states (r2 section 3.1); every change under thread_mutex. */
enum { SLOT_FREE = 0, SLOT_LIVE = 1, SLOT_RETIRING = 2, SLOT_RETIRED_PENDING = 3 };

/* Init gate (erratum 1 E3), CAS-only. */
enum { G_UNINIT = 0, G_CONFIGURING = 1, G_INITIALIZING = 2, G_READY = 3, G_TERMINAL = 4 };

/* ---- structures -------------------------------------------------------------------------------------------------- */

/* One incarnation's producer state.  Written by its owner thread only, until the owner's LAST write, the RELEASE of
 * ack_retire; the drainer reads it only after an ACQUIRE of that word (section 7). */
typedef struct cblock {
    uint32_t ack_retire;        /* atomic: 0, ST_RETIRED or ST_ACKED */
    uint32_t limit;             /* SEQ_LIMIT: the sequence or a counter ceiling was reached (section 5.1) */
    uint64_t next_seq;          /* = produced_count */
    uint64_t dropped;
    uint64_t refused;
    uint64_t regerr;
    uint64_t open_spans;        /* the stack snapshot taken at the STOP batch or at thread_end */
    uint64_t pend_first, pend_last;
    uint32_t pend;              /* a pending loss range exists */
    uint32_t exhausted;         /* diagnostic: a counter or id ceiling set `limit` */
    uint64_t next_frame_id;
    uint64_t current_frame;     /* atomic: frame id + 1, 0 = none (section 5.6) */
    uint64_t next_span_id;      /* starts at 1 */
    uint64_t next_refused_id;   /* starts at 1 (erratum 2 F1) */
} cblock;

typedef struct ring {
    uint64_t write_idx;         /* atomic: producer RELEASE */
    uint8_t pad0[120];
    uint64_t read_idx;          /* atomic: consumer RELEASE */
    uint8_t pad1[120];
    uint32_t cap, mask;
    uint8_t *slots;             /* cap x 32 bytes */
    uint16_t *pub_len;          /* cap: n at a publication's first slot, 0 elsewhere (r2 section 7) */
} ring;

typedef struct slot {
    uint32_t state;             /* SLOT_* (under thread_mutex) */
    uint32_t generation;        /* +1 at every reuse; starts at 1 for the first incarnation */
    uint32_t incarnation;
    uint32_t pad;
    uint64_t os_id;
    char name[64];
    cblock *cb;
    ring *rg;
} slot;

typedef struct desc_entry {
    const char *name, *tag, *file;
    uint32_t name_len, tag_len, file_len;
    uint32_t line, scope;
    uint64_t id;
} desc_entry;

typedef struct sink {
    int kind;                   /* 1 file, 2 callbacks */
    int fd;
    ps3dk_prof_sink_callbacks cb;
    int failed;                 /* a SINK FAILURE: nothing more is written */
    int first_error;
    uint64_t error_count;       /* errors recovered before the trailer (end_flags bit2) */
    int close_failed;
    int closed;
    char path[256];
} sink;

/* One frozen-frontier entry (section 7).  The identity fields are copied under thread_mutex when the latch is set
 * (Ft); the rest is filled once, at the frontier. */
typedef struct fentry {
    uint32_t slot, generation, incarnation;
    uint32_t cls;               /* ST_ACKED, ST_RETIRED or ST_NOT_QUIESCED: frozen PERMANENTLY */
    uint64_t widx;
    uint64_t produced, dropped, refused, regerr, open_spans;
    uint32_t limit;
    uint32_t done;              /* its RETIRED entry was already written by an ordinary drain before the frontier */
    uint64_t os_id;
    char name[64];
} fentry;

/* A THREADS declaration copied under thread_mutex, encoded after it is released (no sink call under a mutex). */
typedef struct tdecl {
    uint32_t slot, generation, incarnation, pad;
    uint64_t os_id;
    char name[64];
} tdecl;

/* A table-snapshot entry (r2 section 3.1). */
typedef struct snap {
    uint32_t slot, generation, incarnation, state;
    uint64_t widx;
    uint32_t retire_now;        /* RETIRED observed at the start of this drain */
    uint32_t pad;
} snap;

typedef struct drainer {
    int started;                /* file header, HEADER and CLOCKS written */
    uint64_t chunk_seq;
    uint8_t *buf;               /* one chunk: 16 + P_CHUNK_CAP + 4 bytes, allocated at init */
    size_t pos;                 /* bytes of the current chunk payload */
    uint32_t chunk_type;
    uint32_t desc_emitted;
    uint32_t *decl_gen;         /* per slot: the generation declared in THREADS (0 = none) */
    uint64_t *written;          /* per slot: sequenced records written for that generation */
    uint32_t *retired_gen;      /* per slot: the generation whose RETIRED entry was written (0 = none) */
    uint32_t any_retired_limit; /* an emitted RETIRED entry had SEQ_LIMIT */
    int corrupt;                /* a publication boundary failed validation: treated as a sink failure */
    snap *snaps;
    uint32_t nsnaps;
    tdecl *tscratch;
    fentry *front;
    uint32_t nfront;
    uint32_t front_fd;          /* descriptors below the frontier */
    int frozen;                 /* the frontier was taken: ordinary processing never runs again */
} drainer;

typedef struct prof {
    ps3dk_prof_config cfg;
    pal_owner owner;            /* APP mode: the drain thread (checked identity) */
    pal_mutex thread_mutex;     /* lock order: thread_mutex before desc_mutex */
    pal_mutex desc_mutex;
    uint32_t stop_latch;        /* atomic */
    uint32_t phase;             /* atomic: 0 RUNNING, 1 STOPPING, 2 SEALING, 3 SEALED */
    uint32_t shutdown_once;     /* atomic */
    uint32_t drain_busy;        /* atomic */
    uint32_t wake;              /* atomic: THREAD mode wake flag */
    uint32_t start_gate;        /* atomic */
    uint64_t unregistered_refusals;   /* atomic, informational: capacity / id-exhaustion refusals (EFULL) only; an
                                       * ETHREAD refusal (failed OS id read) is not counted (codex 03:21) */
    uint64_t tb_freq, tb_at_init;
    uint32_t next_incarnation;  /* under thread_mutex; 0 means exhausted */
    uint32_t nslots;
    slot *slots;
    desc_entry *descs;
    uint32_t desc_count;        /* atomic: RELEASE after a complete entry */
    uint32_t pad;
    sink snk;
    drainer dr;
    pal_thread drain_thread;
    int drain_thread_started;
    int thread_result;          /* THREAD mode: the sealing result, set by the drain thread */
    int clock_failed;
    int join_failed;
    int lock_failed;            /* a PAL mutex call failed: the capture fails closed (ESINKFAIL class, see drain) */
    uint8_t *block;
    /* HEADER strings, copied at init (the caller's config strings need not outlive init). */
    char app[33];
    char build_id[P_STR_MAX + 1];
    char flags[P_STR_MAX + 1];
    char workload_tag[P_STR_MAX + 1];
    uint32_t app_len, build_id_len, flags_len, workload_tag_len;
    uint64_t fnv_build;         /* FNV-1a-64 state after (build_id, 0x00): the shared prefix of every descriptor id */
} prof;

/* Thread-local producer state (r2 section 3; erratum 2 F1). */
typedef struct tls_state {
    uint32_t slot1;             /* slot index + 1; 0 = no incarnation */
    uint32_t generation, incarnation;
    uint8_t acked, retired, in_drain, pad;
    uint32_t depth;
    uint64_t refused_top;
    struct {
        uint64_t span_id;
        uint32_t desc;
        uint32_t pad;
    } stack[P_MAX_DEPTH];
} tls_state;

/* Cross-file internals keep short names in the source; the archive exports them only under ps3dk_prof__. */
#define g_prof ps3dk_prof__g_prof
#define g_gate ps3dk_prof__g_gate
#define g_tls ps3dk_prof__g_tls
#define p_ready ps3dk_prof__ready
#define p_observe_stop ps3dk_prof__observe_stop
#define p_drain_once ps3dk_prof__drain_once
#define p_drain_main ps3dk_prof__drain_main
#define p_quiesce_freeze_seal ps3dk_prof__quiesce_freeze_seal
#define p_fnv ps3dk_prof__fnv
#define p_crc32_init ps3dk_prof__crc32_init
#define p_crc32 ps3dk_prof__crc32
#define p_lock ps3dk_prof__lock
#define p_unlock ps3dk_prof__unlock
#define p_le16 ps3dk_prof__le16
#define p_le32 ps3dk_prof__le32
#define p_le64 ps3dk_prof__le64
#define p_tb ps3dk_prof__tb
#define p_tb_freq ps3dk_prof__tb_freq
#define p_mono_now ps3dk_prof__mono_now

extern prof *g_prof;            /* published with RELEASE when the gate becomes READY */
extern uint32_t g_gate;
extern _Thread_local tls_state g_tls;

/* ---- cross-file internals ---------------------------------------------------------------------------------------- */
int p_ready(void);
void p_observe_stop(prof *p);
int p_drain_once(prof *p);
void *p_drain_main(void *arg);
int p_quiesce_freeze_seal(prof *p);
uint64_t p_fnv(uint64_t h, const void *data, size_t n);
void p_crc32_init(void);
uint32_t p_crc32(const void *data, size_t n);
int p_lock(prof *p, pal_mutex *m);
void p_unlock(prof *p, pal_mutex *m);
void p_le16(uint8_t *d, uint16_t v);
void p_le32(uint8_t *d, uint32_t v);
void p_le64(uint8_t *d, uint64_t v);
uint64_t p_tb(void);
uint64_t p_tb_freq(void);
int p_mono_now(prof *p, uint64_t *us);

/* ---- test hooks (PS3DK_PROF_TEST_HOOKS only; never in an SDK archive) ------------------------------------------- */
#ifdef PS3DK_PROF_TEST_HOOKS
#include "prof_test_hooks.h"
#define HOOK(name) do { if (ps3dk_prof_test_hooks.name) ps3dk_prof_test_hooks.name(); } while (0)
#define HOOK_FLAG(name) (ps3dk_prof_test_hooks.name)
#define STEP(kind, v) do { if (ps3dk_prof_test_hooks.step_observer) ps3dk_prof_test_hooks.step_observer((kind), (v)); } while (0)
extern uint64_t ps3dk_prof_test_stats[128];
#define STAT_ADD(i, v) __atomic_fetch_add(&ps3dk_prof_test_stats[(i)], (uint64_t)(v), __ATOMIC_RELAXED)
#define STAT_SET(i, v) __atomic_store_n(&ps3dk_prof_test_stats[(i)], (uint64_t)(v), __ATOMIC_RELAXED)
#define STAT_GET(i) __atomic_load_n(&ps3dk_prof_test_stats[(i)], __ATOMIC_RELAXED)
#else
#define HOOK(name) do { } while (0)
#define HOOK_FLAG(name) 0
#define STEP(kind, v) do { } while (0)
#define STAT_ADD(i, v) do { } while (0)
#define STAT_SET(i, v) do { } while (0)
#define STAT_GET(i) 0
#endif

#endif /* PS3DK_PROF_INTERNAL_H */
