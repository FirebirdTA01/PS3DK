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

/* For the live auto-start runtime (live_autostart.c).  The lock serializes every start, stop and stats call with the
 * auto-start thread and the shutdown hook; the *_locked functions require it held. */
void ps3dk_prof__live_lock(void);
void ps3dk_prof__live_unlock(void);
/* ps3dk_prof_live_start's checks before anything is used: OK, or its refusal (ENOTINIT, ESTOPPED, ENOTSUP). */
int ps3dk_prof__live_servable(void);
/* ps3dk_prof_live_start under the lock.  is_auto marks the agent as the auto-start runtime's; an explicit start
 * (is_auto 0) while that agent runs is OK when every nonzero field of cfg equals what the auto-start asked for. */
int ps3dk_prof__live_start_locked(const ps3dk_prof_live_config *cfg, int is_auto);
/* ps3dk_prof_live_stop under the lock. */
int ps3dk_prof__live_stop_locked(uint64_t graceful_us, uint64_t join_us);
/* The agent's state under the lock: never started, running (or still stopping), stopped, or a terminal join
 * failure.  *owner_auto: 1 when the auto-start runtime started it. */
enum { LIVE_API_NEVER, LIVE_API_RUNNING, LIVE_API_STOPPED, LIVE_API_JOIN_FAILED };
int ps3dk_prof__live_state_locked(int *owner_auto);

#endif
