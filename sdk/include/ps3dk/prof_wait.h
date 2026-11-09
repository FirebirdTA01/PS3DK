#ifndef PS3DK_PROF_WAIT_H
#define PS3DK_PROF_WAIT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional pre-init override for the separately linked observed runtime.
 * max_actors counts lifetime registrations, including actors with no waits. */
typedef struct ps3dk_prof_wait_config {
    uint32_t size, max_bytes, max_actors, ring_records;
} ps3dk_prof_wait_config;
int ps3dk_prof_wait_configure(const ps3dk_prof_wait_config *cfg);
#ifdef __cplusplus
}
#endif
#endif
