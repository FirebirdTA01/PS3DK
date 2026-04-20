/*! \file sys/time_util.h
 \brief Sony-SDK-source-compat time-base read macro.

  Reads the PowerPC TimeBase register pair into a 64-bit value with the
  rollover-tolerant loop documented in the PowerPC ISA: read TBU, TBL, TBU
  again; if TBU changed, retry.  Cost is ~3-4 cycles in the common case.
*/

#ifndef __PSL1GHT_SYS_TIME_UTIL_H__
#define __PSL1GHT_SYS_TIME_UTIL_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPR numbers for mfspr-based TimeBase read. */
#define _SYS_TBL_SPR  268
#define _SYS_TBU_SPR  269

#define SYS_TIMEBASE_GET(_v)                                            \
	do {                                                                \
		uint32_t _tbu1, _tbl, _tbu2;                                    \
		do {                                                            \
			__asm__ volatile("mfspr %0,269" : "=r"(_tbu1));             \
			__asm__ volatile("mfspr %0,268" : "=r"(_tbl));              \
			__asm__ volatile("mfspr %0,269" : "=r"(_tbu2));             \
		} while (_tbu1 != _tbu2);                                       \
		(_v) = ((uint64_t)_tbu1 << 32) | (uint64_t)_tbl;                \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_SYS_TIME_UTIL_H__ */
