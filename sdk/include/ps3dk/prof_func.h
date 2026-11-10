#ifndef PS3DK_PROF_FUNC_H
#define PS3DK_PROF_FUNC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional pre-init override for the separately linked function-path runtime (libps3dkprof_func.a).
 * Zero fields take the defaults; docs/design/ps3dk-prof-func-paths.md has the limits. */
typedef struct ps3dk_prof_func_config {
    uint32_t size;          /* sizeof(ps3dk_prof_func_config) */
    uint32_t max_bytes;     /* bound on all function-path storage */
    uint32_t max_actors;    /* thread slots with storage */
    uint32_t node_capacity; /* call-tree nodes per actor, including the LOST node */
    uint32_t stack_depth;   /* retained activations per actor */
    uint32_t reserved;      /* 0 */
} ps3dk_prof_func_config;
int ps3dk_prof_func_configure(const ps3dk_prof_func_config *cfg);
#ifdef __cplusplus
}
#endif
#endif
