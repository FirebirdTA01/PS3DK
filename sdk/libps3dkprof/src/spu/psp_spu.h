/* SPU lane producer core (design p3.2 r3): portable C behind a port, so the host model and (later) the SPU build
 * share one planner.  The port maps MFC channels and reports abstract status bits; the host binds it through the
 * port table, the SPU at compile time (psp_port_bind.h, psp_port_spu.h, raw status translated by psp_as.h). */
#ifndef PSP_SPU_H
#define PSP_SPU_H

#include <stddef.h>
#include <stdint.h>

#include "psp_layout.h"

/* Archive names: internals export only under the reserved ps3dk_prof__ prefix (as pal.h does). */
#define psp_spu_begin ps3dk_prof__psp_spu_begin
#define psp_spu_dec_commit ps3dk_prof__psp_spu_dec_commit
#define psp_spu_dec_preflight ps3dk_prof__psp_spu_dec_preflight
#define psp_spu_dec_write ps3dk_prof__psp_spu_dec_write
#define psp_spu_end ps3dk_prof__psp_spu_end
#define psp_spu_fini ps3dk_prof__psp_spu_fini
#define psp_spu_fini_close ps3dk_prof__psp_spu_fini_close
#define psp_spu_flush ps3dk_prof__psp_spu_flush
#define psp_spu_flush_plan ps3dk_prof__psp_spu_flush_plan
#define psp_spu_flush_publish ps3dk_prof__psp_spu_flush_publish
#define psp_spu_flush_put ps3dk_prof__psp_spu_flush_put
#define psp_spu_flush_wait ps3dk_prof__psp_spu_flush_wait
#define psp_spu_init ps3dk_prof__psp_spu_init
#define psp_spu_mark ps3dk_prof__psp_spu_mark

/* Abstract atomic status bits a port returns from rd_atomic_stat (the SPU port maps MFC_RdAtomicStat onto them). */
#define PSP_AS_GETLLAR 1u
#define PSP_AS_PUTLLC_OK 2u
#define PSP_AS_PUTLLC_LOST 4u
#define PSP_AS_PUTLLUC 8u

typedef struct psp_port {
    void *ctx;
    uint32_t (*rd_dec)(void *ctx);
    void (*wr_dec)(void *ctx, uint32_t v);
    /* nonzero when an event the library must not raise or consume is enabled (reservation lost, tag status) */
    int (*blocking_events)(void *ctx);
    uint32_t (*rd_tag_mask)(void *ctx);
    void (*wr_tag_mask)(void *ctx, uint32_t mask);
    uint32_t (*tag_wait_all)(void *ctx); /* MFC_WrTagUpdate ALL, then MFC_RdTagStat */
    void (*getllar)(void *ctx, uint64_t ea, void *ls128);
    void (*putllc)(void *ctx, uint64_t ea, const void *ls128);
    void (*putlluc)(void *ctx, uint64_t ea, const void *ls128);
    uint32_t (*rd_atomic_stat)(void *ctx);
    /* one put command: size is 8 (8-aligned) or a multiple of 16 (16-aligned), at most 16 KiB */
    void (*put)(void *ctx, const void *ls, uint64_t ea, uint32_t size, uint32_t tag);
} psp_port;

#define PSP_HEAD 4u /* headroom slots before and after the staged events */

/* 7 is the core-internal ENOSPC (psp_spu.c).  EDEPTH: dec_write refused at a nonzero zone depth or with suppressed
 * BEGINs outstanding, before any flush, write or publication; flushing never clears it (not retryable). */
enum { PSP_OK = 0, PSP_EBUSY = 1, PSP_EAGAIN = 2, PSP_ECLOSED = 3, PSP_EFAULT = 4, PSP_ENOTSUP = 5, PSP_EINVAL = 6,
       PSP_EDEPTH = 8 };

/* flush phases, exposed so the host scheduler can interleave them */
enum { PSP_FL_IDLE = 0, PSP_FL_PLANNED, PSP_FL_PUT, PSP_FL_WAITED };

typedef struct psp_spu {
    uint8_t line_a[128] __attribute__((aligned(128)));
    uint8_t line_b[128] __attribute__((aligned(128)));
    const psp_port *port;
    uint64_t ea_ring, ea_ctl;
    uint32_t ring_slots; /* C */
    uint32_t tag;
    uint8_t *buf;        /* (PSP_HEAD + ls_cap + PSP_HEAD) slots, 16-aligned */
    uint32_t ls_cap, n;  /* staged events */
    uint64_t seq, older_lost, tail_lost;
    uint64_t gen, w, r_seen;
    uint64_t refused, refused_calls;
    uint32_t flags, state, depth, suppressed, epochs_written;
    uint16_t cal_answered;
    int terminal;        /* events no longer admitted (FAULT, GEN_EXHAUSTED, closed) */
    /* the publication being built */
    int phase;
    uint32_t pub_first, pub_k;          /* slots [buf + pub_first, + pub_k) */
    uint64_t pub_events, pub_lost;      /* events and loss counts this publication covers */
    uint16_t pub_cal;                   /* the id answered by it, 0 = none */
    int pub_epoch;                      /* an EPOCH publication (dec_write) */
} psp_spu;

/* init: buf has (2 * PSP_HEAD + ls_cap) * 8 bytes, 16-aligned; ls_cap even, 4..4096; ring_slots a power of two, 8..2^20 */
int psp_spu_init(psp_spu *s, const psp_port *port, uint64_t ea_ring, uint32_t ring_slots, uint64_t ea_ctl,
                 uint32_t tag, uint8_t *buf, uint32_t ls_cap, int own_dec);
/* zone records */
void psp_spu_begin(psp_spu *s, uint16_t desc);
void psp_spu_end(psp_spu *s, uint16_t desc);
void psp_spu_mark(psp_spu *s, uint16_t desc);
/* flush in one call, or in phases (plan, put, wait, publish) for the scheduler */
int psp_spu_flush(psp_spu *s);
int psp_spu_flush_plan(psp_spu *s);
void psp_spu_flush_put(psp_spu *s);
int psp_spu_flush_wait(psp_spu *s);
int psp_spu_flush_publish(psp_spu *s);
int psp_spu_fini(psp_spu *s);
int psp_spu_fini_close(psp_spu *s); /* the CLOSED publication after fini's flush (the scheduler's step) */
int psp_spu_dec_write(psp_spu *s, uint32_t v);
/* dec_write in phases: preflight (after a completed flush), then the write, then the publication.  Both dec_write and
 * preflight return EDEPTH first when a zone is open (depth != 0) or a BEGIN is suppressed: an epoch never starts
 * inside a zone, so the drain can keep the lane history known across it (clock-mapping plan B r2 5.4, 5.5). */
int psp_spu_dec_preflight(psp_spu *s);
void psp_spu_dec_commit(psp_spu *s, uint32_t v);

#endif
