/*
 * PS3 Custom Toolchain - <sys/random_number.h>
 *
 * Random-number accessor (sysPrxForUser import, liblv2_stub): fill `addr`
 * with `size` bytes of kernel entropy.  `size` is bounded by
 * RANDOM_NUMBER_MAX_SIZE.
 */

#ifndef PS3TC_SYS_RANDOM_NUMBER_H
#define PS3TC_SYS_RANDOM_NUMBER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The PSL1GHT header <lv2/system.h> already defines this; an identical
 * redefinition is legal, so both headers may be included together.  Spelled
 * token-for-token the same (4096) with the parens. */
#define RANDOM_NUMBER_MAX_SIZE (4096)

extern int sys_get_random_number(void *addr, uint64_t size);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_RANDOM_NUMBER_H */
