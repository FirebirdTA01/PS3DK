#ifndef HETEROGENEOUS_SAMPLE_DRAIN_H
#define HETEROGENEOUS_SAMPLE_DRAIN_H
#include <ps3dk/prof.h>
/* THREAD mode has one independent drainer. Application drain calls would
 * return EBUSY; the frame loop and lifecycle share this policy. */
static inline int sample_prof_drain(void)
{
#ifdef SAMPLE_FIFO_EVENTS
    return 0;
#else
    return PS3DK_PROF_DRAIN();
#endif
}
#endif
