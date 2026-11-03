/* E1 SPU program logic (design p3.3 E1 r2-r4 + codex 05:30), portable: the SPU build binds it to the MFC channels
 * (spu/main.c), the host tests to an atomics-based model (host-tests).  The profiler writer API is the real one
 * (prof_spu_lane.h): SPU-built on the target, PS3DK_PROF_SPU_HOST_TEST over the host port table in the tests. */
#ifndef E1_SPU_CORE_H
#define E1_SPU_CORE_H

#include <stdint.h>

#include "../source/e1_report.h"
#include "prof_spu_lane.h"

typedef struct e1_spu_port {
    void *ctx;
    /* blocks until the PPU's inbound command arrives */
    uint32_t (*read_in_mbox)(void *ctx);
    /* sends one report: PAYLOAD put, wait-all on E1_REPORT_TAG, COMMIT put (word 0 = seq), wait-all; the mask the
     * caller had is restored; neither line is rewritten before its wait returned */
    void (*report)(void *ctx, const e1_line *payload, uint32_t seq);
} e1_spu_port;

#define E1_LS_CAP 256u

/* The whole program: attach, epoch 1, report 1, wait GO, first post-GO flush (read_idx observed), dec_write once,
 * epoch 2, fini, report 2.  The first error is reported (kind 1 before GO, kind 2 after) and the program returns;
 * nothing is retried except dec_write once after an EAGAIN preflight refusal (a refusal precedes any write). */
void e1_spu_run(const e1_spu_port *port, ps3dk_prof_spu_writer *w, void *staging, uint64_t ea_ring,
                uint32_t ring_slots, uint64_t ea_ctl);

#endif
