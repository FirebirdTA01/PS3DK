#ifndef HETEROGENEOUS_SPU_POOL_H
#define HETEROGENEOUS_SPU_POOL_H
#include "workload.h"
/* Distinct cache lines: PPU never resets an SPU-owned completion. */
typedef struct __attribute__((aligned(128))) {
    uint32_t digest,count,begin_dec,end_dec;
    uint32_t reserved[28];
    uint32_t sequence;
    uint32_t padding[31];
} work_slot;
#ifndef __SPU__
int pool_init(void);
unsigned pool_count(void);
int pool_submit(uint32_t sequence);
int pool_ready(uint32_t sequence);
const volatile work_slot *pool_slot(unsigned lane);
int pool_close(void);
#if SAMPLE_PROF_EXT
/* SAMPLE_PROF_EXT: the group is created first (its worker count sizes the lanes), started with the lane EAs. */
int pool_create(void);
int pool_start_ext(void);
/* Jobs and keep-alive resets go through the epoch rule (prof_ext.h); completions report the SPU status. */
int pool_submit_ext(uint32_t sequence);
void pool_done_ext(void);
int pool_keepalive(uint32_t sequence);
/* Exit words, the final markers (bounded), then terminate + join.  *join_ok: the workers are stopped. */
int pool_close_ext(int *join_ok);
/* The caller's own SPE deadline failed: eligibility is cleared. */
void pool_failed_ext(const char *why);
int pool_w32_eligible(const char **why);
#endif
#endif
#endif
