/* Automatic PPU function paths (docs/design/ps3dk-prof-func-paths.md).  Ownership: every func_actor field is written
 * by its owner thread only, except the two handoff words (pub: owner RELEASE, drainer ACQUIRE; ack: drainer RELEASE,
 * owner ACQUIRE), the drainer-private d_* fields, and the whole actor once the core's ack/retire word has been
 * acquired (the owner's last write) or while the slot is FREE (reset under thread_mutex).  The publish buffer belongs
 * to the owner while pub == ack and to the drainer while pub != ack. */
#ifndef PS3DK_PROF_FUNC_INTERNAL_H
#define PS3DK_PROF_FUNC_INTERNAL_H
#include "../core/prof_internal.h"
#include <ps3dk/prof_func.h>

#define F_ROOT           0xFFFFFFFFu
#define F_UNKNOWN_CALLER 0xFFFFFFFEu
#define F_LOST_PARENT    0xFFFFFFFDu
#define F_LOST           0u
#define F_MAX_PROBE      32u
#define F_HDR            128u
#define F_DEF            16u
#define F_ROW            40u
#define F_CAP_MIN        4u
#define F_CAP_MAX        16384u
#define F_DEPTH_MIN      4u
#define F_DEPTH_MAX      256u
#define F_DEF_ACTORS     8u
#define F_DEF_CAP        4096u
#define F_DEF_DEPTH      256u
#define F_DEF_BYTES      8388608u
enum { C_FUNC_CONFIG = 28, C_FUNC_TREE = 29, C_FUNC_STATUS = 30 };
enum { FC_DEPTH, FC_LOST, FC_UNBAL, FC_ORPHAN, FC_DEFER, FC_REENTRY, FC_CLOCK, FC_ABANDON, FC_N };
enum { FF_DATA = 1, FF_LAGGING = 2, FF_FINAL = 4, FF_INCOMPLETE = 8, FF_RESET = 16, FF_UNKNOWN = 32, FF_DEPTH = 64,
       FF_LOST = 128, FF_SAT = 256, FF_OPEN = 512, FF_CLOCK = 1024 };
enum { FR_OPEN = 1, FR_SELF_UNTIMED = 2, FR_LOST = 4, FR_SAT = 8, FR_TRUNC = 16, FR_DIRTY = 0x8000u };
_Static_assert(F_HDR + (uint64_t)F_CAP_MAX * (F_DEF + F_ROW) <= P_CHUNK_CAP, "a full cut must fit one chunk");

typedef struct func_frame {
    uint64_t fn, from, child, frame; /* from: the cursor; child: timed child ticks since the cursor */
    uint32_t node, nested;           /* nested: a LOST activation inside a LOST activation (calls only) */
} func_frame;
typedef struct func_node { uint64_t fn; uint32_t parent, pad; } func_node;
typedef struct func_row { uint64_t calls, incl, child; uint32_t open, flags; } func_row;

typedef struct func_actor {
    uint64_t pub, ack;                 /* the handoff words (atomics) */
    uint32_t live, incarnation;        /* set at enrolment, under thread_mutex */
    uint64_t enroll_epoch, last_pub, deferred_epoch, start, interval, final_tick, pub_len;
    uint32_t depth, suppressed, nodes, defined, ndirty, iflags, unknown, final_set;
    uint64_t sup_frame, sup_fn;        /* the first refused level's frame and function, while suppressed > 0 */
    uint64_t cnt[FC_N];
    uint64_t d_acked;                  /* drainer-private */
    uint32_t d_done, d_pad;
} func_actor;

typedef struct func_capture {
    ps3dk_prof_func_config cfg;        /* resolved */
    uint32_t hash_slots, configured, final_status, pad;
    uint64_t allocated, stride;
    uint64_t off_frames, off_nodes, off_rows, off_hash, off_dirty, off_buf;
    uint64_t epoch;                    /* atomic: drainer RELEASE, owners load */
    uint64_t unregistered, no_actor;   /* atomic saturating counters, relaxed */
    uint32_t status_sat, pad2;         /* atomic, sticky: a capture counter saturated or is a lower bound */
    uint64_t rep_unreg, rep_noactor;   /* drainer-private: already reported */
} func_capture;

#define p_func_init ps3dk_prof__func_init
#define p_func_unwind ps3dk_prof__func_unwind
#define p_func_enroll ps3dk_prof__func_enroll
#define p_func_stamp_final ps3dk_prof__func_stamp_final
#define p_func_drain ps3dk_prof__func_drain
#define p_func_seal ps3dk_prof__func_seal
#define p_func_emit_chunk ps3dk_prof__func_emit_chunk
#define p_func_auto_register ps3dk_prof__func_auto_register
#define p_func_actor ps3dk_prof__func_actor
#define p_func_cut ps3dk_prof__func_cut
#define p_func_options ps3dk_prof__func_options
#define p_func_layout ps3dk_prof__func_layout
#define p_func_count ps3dk_prof__func_count
extern ps3dk_prof_func_config p_func_options;
int p_func_layout(ps3dk_prof_func_config *c, uint32_t nslots, func_capture *out);
int p_func_init(prof *p);
void p_func_unwind(prof *p);
void p_func_enroll(prof *p, uint32_t slot, uint32_t incarnation);
void p_func_stamp_final(prof *p);
void p_func_drain(prof *p, const snap *s, uint32_t n);
void p_func_seal(prof *p);
int p_func_emit_chunk(prof *p, uint32_t type, const uint8_t *payload, size_t len);
int p_func_auto_register(prof *p);
func_actor *p_func_actor(func_capture *f, uint32_t slot);
void p_func_cut(func_capture *f, func_actor *a, uint64_t tick, uint64_t epoch, uint16_t flags);
void p_func_count(func_capture *f, uint64_t *counter); /* a saturating capture-counter increment */

static inline func_frame *f_frames(func_capture *f, func_actor *a) { return (func_frame *)(void *)((uint8_t *)a + f->off_frames); }
static inline func_node *f_nodes(func_capture *f, func_actor *a) { return (func_node *)(void *)((uint8_t *)a + f->off_nodes); }
static inline func_row *f_rows(func_capture *f, func_actor *a) { return (func_row *)(void *)((uint8_t *)a + f->off_rows); }
static inline uint32_t *f_hash(func_capture *f, func_actor *a) { return (uint32_t *)(void *)((uint8_t *)a + f->off_hash); }
static inline uint32_t *f_dirty(func_capture *f, func_actor *a) { return (uint32_t *)(void *)((uint8_t *)a + f->off_dirty); }
static inline uint8_t *f_buf(func_capture *f, func_actor *a) { return (uint8_t *)a + f->off_buf; }

/* Saturating accumulation: never wraps; *sat is set when the ceiling is reached. */
static inline __attribute__((no_instrument_function)) void f_add(uint64_t *x, uint64_t v, uint32_t *sat)
{
    if (*x > UINT64_MAX - v) {
        *x = UINT64_MAX;
        *sat = 1;
    } else {
        *x += v;
    }
}
static inline __attribute__((no_instrument_function)) void f_count(func_actor *a, unsigned k)
{
    uint32_t sat = 0;
    f_add(&a->cnt[k], 1, &sat);
    if (sat)
        a->iflags |= FF_SAT;
}
static inline __attribute__((no_instrument_function)) void f_touch(func_capture *f, func_actor *a, uint32_t node)
{
    func_row *r = &f_rows(f, a)[node];
    if (!(r->flags & FR_DIRTY)) {
        r->flags |= FR_DIRTY;
        f_dirty(f, a)[a->ndirty++] = node; /* at most node_capacity distinct nodes */
    }
}

#ifdef PS3DK_PROF_TEST_HOOKS
extern void (*ps3dk_prof__func_test_hook)(unsigned point); /* 1: a cut is complete, before its publication */
void ps3dk_prof__func_test_seed(uint64_t unregistered, uint64_t no_actor); /* counters and their reported values */
void ps3dk_prof__func_test_seed_counter(unsigned k, uint64_t v);         /* the calling thread's interval counter */
#endif
#endif
