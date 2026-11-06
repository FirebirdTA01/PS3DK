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
} run;

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

/* expectation file helpers */
FILE *expect_open(const char *dir, const char *name);

#endif
