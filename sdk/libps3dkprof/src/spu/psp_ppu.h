/* PPU side of the SPU lane transport (design p3.2 r3): lane table, per-file capacity ledger, snapshot protocol,
 * encoder of the P3.1 extension chunks, calibration and lane lifetime.  Portable C behind psp_mem (main-memory
 * access and barriers) and psp_out (chunk output), so the host model can make visibility adversarial.  Host model
 * only in this slice: no runtime integration. */
#ifndef PSP_PPU_H
#define PSP_PPU_H

#include <stddef.h>
#include <stdint.h>

#include "psp_layout.h"

/* Archive names: internals export only under the reserved ps3dk_prof__ prefix (as pal.h does). */
#define psp_drain ps3dk_prof__psp_drain
#define psp_drain_declare ps3dk_prof__psp_drain_declare
#define psp_drain_ends ps3dk_prof__psp_drain_ends
#define psp_image_add ps3dk_prof__psp_image_add
#define psp_lane_create ps3dk_prof__psp_lane_create
#define psp_lane_end ps3dk_prof__psp_lane_end
#define psp_lane_end_poll ps3dk_prof__psp_lane_end_poll
#define psp_lane_end_request ps3dk_prof__psp_lane_end_request
#define psp_lane_stats_get ps3dk_prof__psp_lane_stats_get
#define psp_lane_step ps3dk_prof__psp_lane_step
#define psp_ppu_fini ps3dk_prof__psp_ppu_fini
#define psp_ppu_init ps3dk_prof__psp_ppu_init
#define psp_reader_limits ps3dk_prof__psp_reader_limits
#define psp_seal ps3dk_prof__psp_seal

typedef struct psp_mem {
    void *ctx;
    uint64_t (*ld64)(void *ctx, uint64_t ea);
    void (*ld_line)(void *ctx, uint64_t ea, uint8_t out[128]); /* line B fields, one load sequence */
    void (*ld_slots)(void *ctx, uint64_t ea, uint8_t *out, size_t n);
    void (*st64)(void *ctx, uint64_t ea, uint64_t v);
    void (*st16)(void *ctx, uint64_t ea, uint16_t v);
    void (*lwsync)(void *ctx);
    void (*sync)(void *ctx);
    void (*isync)(void *ctx);
    uint64_t (*mftb)(void *ctx);
    int (*alloc)(void *ctx, size_t size, uint64_t *ea); /* 128-aligned, zeroed; 0 on success */
    void (*free)(void *ctx, uint64_t ea, size_t size);
} psp_mem;

typedef struct psp_out {
    void *ctx;
    int (*chunk)(void *ctx, uint32_t type, const uint8_t *payload, size_t len); /* nonzero: the sink failed */
} psp_out;

typedef struct psp_desc {
    uint32_t ls_addr, line;
    const char *name, *file;
} psp_desc;

/* the reader's per-file aggregates (spec 1); tests may lower them only through psp_limits, and every lowered value
 * is a MODEL limit, never the reader's */
typedef struct psp_limits {
    uint32_t lanes_life, lanes_live, images, epochs, loss_chunks, brackets_epoch, brackets_file;
} psp_limits;

typedef struct psp_cfg {
    uint32_t ring_slots, lanes_max;
    uint64_t mem_max;
    uint32_t cancel_after; /* drain opportunities without an answer (private: 1024) */
} psp_cfg;

enum { PSP_P_OK = 0, PSP_P_EINVAL, PSP_P_ENOSPC, PSP_P_ENOMEM, PSP_P_ESTOPPED, PSP_P_ETIMEDOUT, PSP_P_EBUSY };

/* FINAL states (spec 1) */
#define PSP_FIN_FINALIZED 1u
#define PSP_FIN_ABANDONED 2u
#define PSP_FIN_OVERFLOWED 3u

enum { PSP_L_FREE = 0, PSP_L_CREATED, PSP_L_DECLARED, PSP_L_STARTED, PSP_L_FINAL };

typedef struct psp_snap {
    uint64_t gen, w, next_seq, older_lost, refused, refused_calls;
    uint32_t state, flags, epochs_written;
    uint16_t cal_answered;
} psp_snap;

typedef struct psp_lane {
    int state;              /* PSP_L_* */
    int end_req, released, sealed_unfreed, file_final_by_seal;
    uint32_t lane_id, domain_id, image, owner;
    char name[32];
    uint64_t ea_ring, ea_ctl;
    size_t bytes;
    uint64_t r, w_snap;
    psp_snap good;          /* the last accepted snapshot */
    int have_good;
    uint64_t emitted_next, emitted_events, emitted_lost;
    uint32_t epoch;         /* 0 before EPOCH 1 */
    uint64_t brackets_epoch; /* emitted in the current epoch */
    /* reservations held in the ledger */
    int res_epoch1, res_terminal, res_bracket;
    /* calibration */
    uint32_t next_req;      /* 1..65535; 65536 = exhausted */
    uint16_t out_id;
    uint64_t out_tb0;
    uint32_t out_epoch, out_waits;
    uint64_t stale, cancelled, unstable;
    /* replay of the emitted prefix with the reader's pairing rules (spec 3) */
    int known, ever_unknown, protocol;
    uint32_t sp;
    uint16_t sdesc[64];
    uint8_t sdepth[64];
    /* retirement from encoding (state PSP_L_FINAL) is separate from a FINAL the sink ACCEPTED */
    uint32_t fin_state;
    int final_written;
    /* the drain coroutine */
    int dphase;
    uint64_t g0;
    uint8_t line[128];
    uint64_t tb1;
    uint32_t end_waits;
    uint32_t tries;         /* inconsistent snapshots in this cycle */
} psp_lane;

/* an image is registered all-or-nothing and COPIED into the image arena: descriptor structs and their name/file
 * strings are owned here after psp_image_add returns; the caller may change or free its own copies at once.
 * "emitted" is drainer bookkeeping (the SPU_IMAGES entry was handed to the sink), not proof of acceptance. */
typedef struct psp_image {
    int used, emitted;
    uint8_t build_id[64];
    uint32_t build_len, ndesc;
    psp_desc *descs;        /* in the arena; name/file point into it too */
} psp_image;

/* All storage the PPU side needs besides its own struct, supplied by the caller (the runtime carves it from its one
 * init block); nothing here allocates. */
typedef struct psp_storage {
    uint8_t *scratch;       /* one chunk payload: at least PSP_SCRATCH_BYTES */
    size_t scratch_len;
    uint8_t *copy;          /* one lane's ring copy: at least ring_slots * 8 */
    size_t copy_len;
    uint8_t *arena;         /* image descriptors and strings, bump-allocated, never freed */
    size_t arena_len;
} psp_storage;
#define PSP_SCRATCH_BYTES 1048576u

typedef struct psp_ppu {
    psp_cfg cfg;
    psp_limits lim;
    const psp_mem *mem;
    const psp_out *out;
    psp_lane lanes[64];
    psp_image images[32];
    uint32_t nimages, lanes_created, lanes_live;
    uint64_t epochs_emitted, epochs_reserved, loss_emitted, loss_reserved, brackets_file;
    uint64_t mem_used;
    int sealed, sink_failed, corrupt;
    /* the abstract lane-table lock: every drainer access to lane metadata or memory (a step, an end, the seal, the
     * declarations) happens inside it; a lane is freed or its entry reused only outside it (psp_lane_end_poll).  The
     * real mutex/atomics are deferred; this host model is single-threaded, so the boundary is the API call. */
    int cs;
    int test_no_terminal;   /* TEST ONLY: the no-reservation control of trace C2 (never set by a library caller) */
    uint8_t *scratch;       /* one chunk payload */
    size_t spos;
    uint8_t *copy;
    size_t copy_len;
    uint8_t *arena;
    size_t arena_len, arena_used;
} psp_ppu;

psp_limits psp_reader_limits(void);
int psp_ppu_init(psp_ppu *p, const psp_cfg *cfg, const psp_limits *lim, const psp_mem *mem, const psp_out *out,
                 const psp_storage *st);
void psp_ppu_fini(psp_ppu *p);
int psp_image_add(psp_ppu *p, const uint8_t *build_id, uint32_t build_len, const psp_desc *d, uint32_t n,
                  uint32_t *index);
int psp_lane_create(psp_ppu *p, const char *name, uint32_t image, uint32_t owner, uint32_t *lane);
/* the drain: declarations, then each lane's coroutine (one phase per step), then end requests.  Counts and ledger
 * entries advance only for chunks the sink ACCEPTED; after a sink failure nothing more is encoded, while lifetime
 * (FINAL bookkeeping, release, free) still progresses. */
void psp_drain_declare(psp_ppu *p);
int psp_lane_step(psp_ppu *p, uint32_t lane); /* returns 1 when the lane's drain cycle completed */
void psp_drain_ends(psp_ppu *p);
void psp_drain(psp_ppu *p);
/* lifetime: request (after a successful join), then poll; free happens in poll after the drainer released */
void psp_lane_end_request(psp_ppu *p, uint32_t lane);
int psp_lane_end_poll(psp_ppu *p, uint32_t lane, uint32_t max_waits);
int psp_lane_end(psp_ppu *p, uint32_t lane);
void psp_seal(psp_ppu *p);

typedef struct psp_lane_stats {
    /* FROZEN_AT_FINAL: what the file states */
    int final_written;
    uint32_t fin_state;
    uint64_t next_seq, events, lost;
    /* NOT_IN_FILE: line B after a successful post-seal join, never merged with the above */
    int have_post_join;
    psp_snap post_join;
    uint64_t stale, cancelled, unstable;
} psp_lane_stats;
int psp_lane_stats_get(psp_ppu *p, uint32_t lane, int post_join, psp_lane_stats *st);

#endif
