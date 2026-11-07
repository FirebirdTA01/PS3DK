/* overhead-bench target bindings of the portable driver (common/ob_drv.c), SPU lifecycle (common/ob_spu_life.c) and
 * GPU section (common/ob_gpu_run.c).  The resource audit list (Plan A r2 3.1e):
 *
 * Resource       | profiler                                    | bench
 * GPU reports    | OB_GPU_REPORT_FIRST .. +OB_GPU_REPORT_COUNT | OB_BENCH_REPORT, OB_BENCH_REPORT + 1
 * GPU label      | OB_GPU_LABEL (never written by the bench)    | OB_BENCH_LABEL
 * SPU DMA tag    | OB_WRITER_TAG (the lane writer)              | OB_BENCH_TAG (the replies only)
 * SPU ring       | the lane's ring (ea_ring, ring_slots)        | none
 * (constants in common/ob_gpu_run.h and common/ob_spu_res.h; ob_owner_check runs on them before the attach) */
#ifndef OB_BIND_H
#define OB_BIND_H

#include <stdint.h>

#include <ps3dk/prof.h>

#include "../common/ob_gpu_run.h"
#include "../common/ob_spu_life.h"

#define OB_WAIT_US 2000000ull
#define OB_SPU_RING_SLOTS 1024u
#define OB_SPU_IMAGE_BYTES (256u * 1024u)
#define OB_EXT_MEM_MAX (2u * 1024u * 1024u)
#define OB_SPU_POLLS 500000u   /* x 10 us: 5 s per reply */
#define OB_LABEL_POLLS 200000u /* x 10 us: 2 s per label */

int ob_prof_init(void); /* sink path, then init_ex: APP mode, ring 4096, one SPU lane, one GPU domain */
const ob_spu_port *ob_spu_port_lv2(void);
uint64_t ob_spu_reply_ea(void);
const ob_gpu_port *ob_gpu_port_gcm(void);

#endif
