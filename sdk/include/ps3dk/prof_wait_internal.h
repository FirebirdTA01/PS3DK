#ifndef PS3DK_PROF_WAIT_INTERNAL_H
#define PS3DK_PROF_WAIT_INTERNAL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private observed-library ABI. Raw targets carry no object-generation claim. */
typedef struct ps3dk_prof_wait_cookie {
    uint64_t session, call_id;
    uint32_t actor, generation, admitted, reserved;
} ps3dk_prof_wait_cookie;
ps3dk_prof_wait_cookie ps3dk_prof__wait_begin(uint32_t operation, uint64_t target,
    uint64_t secondary, uint64_t timeout, uint64_t site);
void ps3dk_prof__wait_end(ps3dk_prof_wait_cookie cookie, int32_t raw_result, int32_t api_result);
void __ps3dk_prof_wait_thread_start(void);
void __ps3dk_prof_wait_thread_exit(void);
#ifdef __cplusplus
}
#endif
#endif
