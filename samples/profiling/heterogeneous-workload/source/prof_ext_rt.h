#ifndef HETEROGENEOUS_PROF_EXT_RT_H
#define HETEROGENEOUS_PROF_EXT_RT_H
/* The target side of SAMPLE_PROF_EXT (prof_ext_bind.c): the profiler init/teardown through prof_ext.c's port, the
 * per-worker lanes for the pool, the SPU epoch rule for job words, and the GPU domain for the scene. */
#include <stdint.h>
#include "prof_ext.h"

/* Profiler init (init_ex with the ext config), the owner thread, the image and one lane per worker.  0: the
 * profiler is up (lanes may be off, logged); nonzero: init failed. */
int pe_rt_start(unsigned workers);
/* The lane for worker i (NULL when SPU profiling is off). */
const pe_lane *pe_rt_lane(unsigned i);
/* SPU lanes are on (every worker has a lane).  The epoch rule lives with the pool (spu_pool.c, pool_proto.c). */
int pe_rt_spu_on(void);
/* GPU: after scene_init; per frame; calibration points; the domain id (0: off). */
void pe_rt_gpu_start(void);
void pe_rt_gpu_frame(void);
void pe_rt_gpu_calibrate(void);
uint32_t pe_rt_gpu(void);
/* Teardown after the frame loop; join_ok: the SPU group joined.  Nonzero when anything failed or was retained. */
int pe_rt_stop(int join_ok);

#endif
