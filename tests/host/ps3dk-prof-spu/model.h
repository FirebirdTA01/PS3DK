/* Host model of the SPU lane transport (design p3.2 r3, slice 1): simulated main memory with strong (indivisible
 * line) or weak (independently visible words) line-B visibility, an SPU port shim that checks every MFC rule it can
 * model, a PPU memory shim, a chunk collector, the v1 skeleton writer, ground truth and an independent stream check.
 * Model evidence only: nothing here is target evidence. */
#ifndef SPU_MODEL_H
#define SPU_MODEL_H

#include <stdint.h>
#include <stdio.h>

#include "psp_ppu.h"
#include "psp_spu.h"

#define MAX_LANES_H 1100

/* ---- simulated main memory ---- */
extern uint8_t *g_mem;
extern uint64_t g_tb;
extern int g_weak;       /* line-B putlluc becomes queued word writes */
extern int g_failures;
extern int g_need_bar, g_order_faults;

#define FAIL(...)                                                                                                    \
    do {                                                                                                               \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                         \
        fprintf(stderr, __VA_ARGS__);                                                                                  \
        fprintf(stderr, "\n");                                                                                         \
        g_failures++;                                                                                                  \
    } while (0)

void mem_reset(void);
/* weak visibility: queued word writes of line-B publications, applied by the scheduler */
int mem_pending(void);
void mem_apply(int which); /* apply the which-th pending group (0 = oldest) */
void mem_apply_all(void);
/* every line-B version a producer published (strong and weak), for the strong-model snapshot invariant */
void versions_reset(void);
int versions_has(const uint8_t line[128]); /* the all-zero line (never started) counts as published */

/* ---- ground truth, recorded at the SPU when an event takes a sequence number ---- */
typedef struct tev {
    uint32_t dec;
    uint16_t desc;
    uint8_t kind, depth;
} tev;

/* ---- one SPU producer (lane handle) ---- */
typedef struct port_ctx {
    uint32_t dec, last_dec;
    int blocking;
    uint32_t tag_mask;
    int res_held;
    uint32_t last_stat;
    int ops, fault_at_op; /* inject: the fault_at_op-th atomic/tag status is missing (0 = never) */
    int fail_closed_store; /* inject: the putlluc that would publish CLOSED never completes */
    uint32_t wr_log[8];
    int nwr;
    psp_spu *s;
    int dma_count;
} port_ctx;

typedef struct lane_h {
    psp_spu s;
    port_ctx pc;
    psp_port port;
    uint8_t *buf;
    uint32_t idx, lane_id, domain_id;
    tev *truth;
    uint64_t ntruth, cap;
    uint64_t tbase; /* seqs below it were assigned and lost before the trace started (an injected state) */
    int dead; /* the SPU thread was terminated */
    uint64_t ea_ring, ring_bytes;
    /* order records: producer ground truth of each published CAL slot, (raw value, the lane's next seq at that
     * publication), recorded by cal_note after a flush that answered a request */
    uint64_t (*cal)[2];
    size_t ncal, capcal;
} lane_h;

/* ---- one run: the PPU drainer, its output and its lanes ---- */
typedef struct chunk_rec {
    uint32_t type;
    uint32_t len;
    uint8_t *data;
} chunk_rec;

typedef struct run {
    psp_ppu p;
    psp_mem m;
    psp_out o;
    chunk_rec *ch;
    size_t nch, capch;
    long fail_sink_at; /* the sink fails at this chunk number (-1 never) */
    uint32_t fail_type, fail_nth, seen_type; /* or: at the fail_nth chunk of fail_type */
    int sink_dead;
    lane_h *lanes[MAX_LANES_H];
    uint32_t nl;
    uint32_t image;
    uint8_t *st_scratch, *st_copy, *st_arena; /* the storage the runtime would carve from its init block */
    int order; /* the run emits SPU_ORDER records: the capture carries file_flags 7 and a PROFILE chunk */
} run;

/* set before run_init / run_init_cap: the next run calls psp_ppu_order after init (cleared by the init) */
extern int g_order_next;

void run_init(run *r, uint32_t ring_slots, uint32_t lanes_max, const psp_limits *lim);
void run_init_cap(run *r, uint32_t ring_slots, uint32_t lanes_max, uint64_t mem_max, const psp_limits *lim);
/* inject k already-assigned, already-lost sequence numbers at the producer (older_lost) */
void inject_older(lane_h *h, uint64_t k);
void run_free(run *r);
lane_h *lane_new(run *r, uint32_t ls_cap, int own_dec, int start);
void lane_drop(run *r, lane_h *h); /* free the handle after the lane ended */
/* zone wrappers that record truth */
void zb(lane_h *h, uint16_t d);
void ze(lane_h *h, uint16_t d);
void zm(lane_h *h, uint16_t d);
/* checks after each library call: no reservation of ours outlives it, the app tag mask is restored */
void call_ok(lane_h *h, const char *what);

/* the open_begins field of the lane's accepted FINAL: 1 and *open set when one was accepted, else 0 */
int final_open(run *r, uint32_t lane_id, uint32_t *open);

/* write <dir>/<name>.prof (+ .truth); with_trailer = the run finished without sink failure */
void write_capture(run *r, const char *dir, const char *name);
/* the independent stream check: tiling, event identities vs truth, FINAL = emitted, and the FINAL's open_begins
 * against a replay of the accepted chunks under the depth-zero epoch rule; returns 0 when consistent */
int check_streams(run *r, const char *name);

/* what the sink ACCEPTED for a lane (the collector stores only accepted chunks), and the stats check against it:
 * FROZEN_AT_FINAL counts must equal the accepted chunks, and final_written must equal "a FINAL was accepted" */
void accepted_counts(run *r, uint32_t lane_id, uint64_t *next, uint64_t *events, uint64_t *lost, int *final_present);
void check_stats(run *r, lane_h *h, const char *name);

/* ---- order records (spu-ticks delta D3) ---- */
/* after a flush or fini that returned PSP_OK: note the CAL it published, if any (producer ground truth) */
void cal_note(lane_h *h, uint16_t answered_before);
/* 1 when tb was returned by an mftb whose previous memory access was a ring load inside [lo, lo + n) */
int tb_after_ring_load(uint64_t tb, uint64_t lo, uint64_t n);
/* the ledger invariant: order_emitted + order_reserved <= lim.order_records */
void order_ledger_ok(run *r, const char *where);
/* the independent check of the accepted order records; returns the violated classes (0 = consistent) */
#define OC_LAYOUT 1u   /* a chunk layout other than n=1 {1,4,5} or n=2 {DRAINED, DRAIN_TB} of one lane/through */
#define OC_LONE 2u     /* a DRAINED or DRAIN_TB alone */
#define OC_CURSOR 4u   /* DRAINED.events_through differs from the accepted cursor at its position */
#define OC_EPOCH 8u    /* DRAINED.epoch differs from the current epoch, or 0 */
#define OC_RUN 16u     /* a DRAINED's run (events since the previous DRAINED) holds an event of another epoch */
#define OC_TBORDER 32u /* a DRAIN_TB tb not read right after a ring load of its lane, or decreasing */
#define OC_BEFORE 64u  /* BRACKET_AT.events_before differs from the producer's seq at the CAL publication */
#define OC_CREATED 128u /* LANE_CREATED missing, duplicated, not first, or for an undeclared lane */
#define OC_FINAL 256u  /* ORDER_FINAL count wrong, duplicated, or a lane DOMAIN_FINAL not directly after it */
#define CK_RUN 1       /* check_order flag: apply the OC_RUN check (only for runs without dropped decisions) */
unsigned check_order(run *r, const char *name, int flags);
/* count of accepted order records of `kind` for a lane; ORDER_FINAL fields (0 when absent) */
uint64_t order_count(run *r, uint32_t lane_id, uint32_t kind);
int order_final(run *r, uint32_t lane_id, uint64_t *emitted, uint64_t *dropped);

/* expectation file helpers */
FILE *expect_open(const char *dir, const char *name);

#endif
