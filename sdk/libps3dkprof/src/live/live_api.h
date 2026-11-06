/* Internal companions of the public live API (live_api.c), for host tests
 * and the sample: the bound port and the agent's counters. Archive names
 * stay under ps3dk_prof__. */
#ifndef PS3DK_PROF_LIVE_API_H
#define PS3DK_PROF_LIVE_API_H
#include <ps3dk/prof.h>
#include "live_agent.h"
#include "prof_finalization.h"

/* The port the agent bound (0 if live_start never succeeded). */
uint16_t ps3dk_prof__live_port(void);

/* The static agent, for reading its counters; never NULL. */
const live_agent *ps3dk_prof__live_agent(void);

#endif
