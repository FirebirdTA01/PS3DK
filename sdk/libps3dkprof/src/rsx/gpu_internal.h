/* P3.3 R2: the runtime's GPU domain table (host only, internal).  Design: p3.3-r2 935936fb + deltas 2400e50a,
 * 0e23d13e and the codex 12:53 rulings.
 *
 * A TABLE SLOT outlives the domains attached in it:
 *  - users, id (atomics) and the snap mutex are process-lifetime words, initialized once at runtime init and never
 *    reset; every API call PINS a slot (bounded SC CAS increment of users, then an SC load of id) before it reads
 *    any other field, and unpins on every exit;
 *  - the owner entries belong to the slot, not to a domain: identity and tag are written and read under
 *    thread_mutex, except the drain step's reads of an entry named by a published, undrained ticket (the lease);
 *    `evidence` is an atomic word; `cur` and `last_ticket` are producer-private.
 * Everything else in the slot is a domain field: written by attach (under the extension mutex) before the id is
 * published, read by pinned calls after it. */
#ifndef PS3DK_PROF_GPU_INTERNAL_H
#define PS3DK_PROF_GPU_INTERNAL_H

#include <stdint.h>

#include "gpu_port_gcm.h"
#include "pal.h"
#include "prg.h"

#define GPU_OWNER_REFS 8u
#define GPU_ID_BASE 1026u     /* SPU lanes use 2..1025 */
#define GPU_LIFETIME_MAX 64u  /* 1026..1089: the reader's 64 RSX domains */
#define GPU_USERS_MAX 0x80000000u
#define GPU_TAG_MAX 0x3FFFFFFFu /* tags 1..GPU_TAG_MAX - 1; an entry at GPU_TAG_MAX is retired from installation */
#define GPU_EV_STATE(v) ((v) >> 30)
#define GPU_EV_TAG(v) ((v) & GPU_TAG_MAX)
enum { GPU_EV_NONE = 0u, GPU_EV_EVIDENCED = 1u, GPU_EV_ORPHAN = 2u }; /* 3: reserved, never written */
#define GPU_EV(state, tag) (((uint32_t)(state) << 30) | (tag))

/* the GPU share of the reader's file-wide aggregates (D1, the static partition) */
#define GPU_LOSS_CHUNKS 4096u
#define GPU_BRACKETS_FILE 16384u
#define SPU_LOSS_CHUNKS_WITH_GPU 61440u
#define SPU_BRACKETS_FILE_WITH_GPU 245760u
#define SPU_EPOCHS_WITH_GPU 4032u

enum { GPU_S_FREE = 0, GPU_S_LIVE = 1, GPU_S_QUARANTINED = 2 };

typedef struct gpu_owner {
    /* under thread_mutex (and the lease exception) */
    uint32_t installed, dom_id, incarnation, slot, generation, tag;
    uint32_t evidence; /* atomic: GPU_EV(state, tag) */
    /* producer-private (the owning domain's ctx owner thread) */
    uint32_t cur;
    uint64_t last_ticket; /* the ticket count after the last event that named it */
    /* drainer-private */
    uint32_t orphan_counted_tag;
} gpu_owner;

typedef struct gpu_slot {
    /* process lifetime */
    uint32_t users, id;   /* atomics (SC) */
    uint32_t state;       /* GPU_S_*, under the extension mutex (QUARANTINED also stored atomically after a fault) */
    pal_mutex snap;
    int snap_ok;
    gpu_owner own[GPU_OWNER_REFS];
    /* domain fields (attach) */
    struct prof *p;
    const prg_port *dev;  /* the GPU device: &devport (the GCM port), or the legacy test-hook emulator */
    /* R4a: the prepared GCM port and its bound device ops (ctx = &gpp).  Prepared by attach under the extension
     * mutex before prg_attach; NEVER cleared by a logical release: only the next attach that selects this slot
     * (FREE, users 0, under the mutex) clears and re-prepares it.  Reachable only through dev, i.e. by a pinned call
     * whose id matched, by the drainer for a LIVE slot under the mutex, or by attach before publication. */
    gpp_port gpp;
    prg_port devport;
    pal_owner owner;      /* the ctx owner thread */
    void *ctx;
    uint32_t label;
    uint32_t report_first, report_count;
    prg_dom dom;
    prg_meta *meta;
    prg_stage *stage;
    uint8_t *scratch;
    /* producer-private */
    uint32_t cur_ref;     /* current owner entry index + 1 (0 = NONE) */
    uint64_t owner_busy, refused_scope, refused_ctx;
    /* drainer-private */
    uint64_t orphaned;
} gpu_slot;

#endif
