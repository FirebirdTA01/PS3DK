/* overhead-bench: the case table and each case's planned ledger (Plan A r2 section 4 with probe-design-r1 11.2 A-2,
 * A-3 and probe-design-r2 D7).  Portable C; the host ledger model (host-tests) proves each ON case reaches its
 * intended path against the real runtime core / SPU writer core before any target build, and the verifier
 * (tools/ob_verify.py) checks the same planned values in the bench CSV.
 *
 * Ledger fields per block (deltas over the block): seq = sequence numbers assigned, dropped = of those, dropped
 * (published = seq - dropped on the PPU), regs = descriptor registrations, depth = the maximum zone depth, and for
 * the SPU lane: staged = the staging count after the block minus before, tail_lost, published (events made visible).
 * OFF and OFF-b blocks run the identical loop with the macros compiled out: their ledger is the iteration count only. */
#ifndef OB_CASES_H
#define OB_CASES_H

#include <stdint.h>

#define OB_RING_RECORDS 4096u /* the bench's PPU ring (ps3dk_prof_config.ring_records) */
#define OB_LS_CAP 256u        /* the SPU lane's staging capacity (events) */
#define OB_PAIR_SEQ 3u        /* ring slots per PPU zone pair: BEGIN 2, END 1 */

enum {
    OB_P_HOT = 0,   /* warm descriptor, ring empty: n pairs */
    OB_P_NEST8,     /* warm, ring empty: n nests of depth 8 */
    OB_P_COLD,      /* 64 fresh sites, each one pair, executed once (D7) */
    OB_P_PREFILL,   /* UNTIMED: fill the ring to its capacity (A-3) */
    OB_P_DROP,      /* timed on the full ring: n pairs, all dropped (A-3) */
    OB_P_FRAME,     /* warm: n frame marks */
    OB_P_EMPTY,     /* n = 0: the raw block baseline, never divided */
    OB_S_PAIR,      /* lane warm, staging 0, decrementer written: n pairs, no flush */
    OB_S_FLUSH0,    /* staging k (0, 1, 16, 128): one timed flush */
    OB_S_FLUSH1,
    OB_S_FLUSH16,
    OB_S_FLUSH128,
    OB_S_STAGEFULL, /* ring full, staging at ls_cap: n pairs, each event a tail loss (A-3) */
    OB_S_BACKPRESS, /* one flush on a full ring: published 0 (A-3) */
    OB_CASES
};

typedef struct ob_case {
    const char *name;
    uint32_t n;      /* pairs / nests / marks per block (P-cold: pairs per site = 1) */
    int timed;       /* 0: an untimed precondition step (P-prefill) */
    int spu;
} ob_case;

typedef struct ob_ledger {
    uint64_t seq, dropped, regs, depth;    /* PPU */
    int64_t staged;                        /* SPU (signed: a flush lowers it) */
    uint64_t sseq, tail_lost, published;   /* SPU */
    uint64_t lost;                         /* SPU: staged events the writer turned into losses (older_lost) */
} ob_ledger;

extern const ob_case ob_case_table[OB_CASES];

/* the planned ledger of one ON block of case c (P-cold: one site) */
ob_ledger ob_planned(int c);
/* 0 when got equals the plan; else -1 and a reason in why (at most n bytes) */
int ob_ledger_check(int c, const ob_ledger *got, char *why, unsigned long n);

#endif
