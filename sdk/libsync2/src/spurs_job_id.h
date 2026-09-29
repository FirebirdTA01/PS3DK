/* spurs_job_id.h - the thread id of a SPURS job (or job queue job).
 *
 * A job has no id of its own, so the first call stamps one: the decrementer
 * at that moment << 32 | (SPURS instance EA | SPU number).  The job image is
 * reloaded for every job, which resets the stamp.
 */
#ifndef SYNC2_SPURS_JOB_ID_H
#define SYNC2_SPURS_JOB_ID_H

#include <stdint.h>
#include <spu_mfcio.h>

extern uint64_t cellSpursGetSpursAddress(void);
extern uint32_t cellSpursGetCurrentSpuId(void);

static inline uint64_t s2_job_id(uint64_t *stamp)
{
	if (*stamp == 0) {
		uint32_t dec = spu_readch(SPU_RdDec);
		*stamp = ((uint64_t)dec << 32) | ((uint32_t)cellSpursGetSpursAddress() | cellSpursGetCurrentSpuId());
	}
	return *stamp;
}

#endif
