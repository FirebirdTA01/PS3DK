/* <ps3dk/prof_counter.h> -- named per-frame counters in a ps3dk-prof capture (format section 11: file flag bit 8,
 * chunk type 39 COUNTERS).
 *
 * The call exists only in a runtime built with PS3DK_PROF_WITH_COUNTERS: link libps3dkprof_counters.a in place of
 * libps3dkprof.a, or libps3dkprof_ext_counters.a in place of libps3dkprof_ext.a (sdk/libps3dkprof `make counters`
 * / `install-counters`).  The default archives are unchanged and never set bit 8.  Build with -DPS3DK_PROF=1; with PS3DK_PROF undefined or 0, PS3DK_PROF_COUNTER compiles to
 * nothing that runs and references no function.
 *
 * PS3DK_PROF_COUNTER(name, value) records one sample: the calling thread's incarnation, the counter, the thread's
 * frame marker (0 before its first ps3dk_prof_frame(), else 1 + the id of its last FRAME), the timebase and the
 * signed 64-bit value.  `name` is 1..64 bytes of printable ASCII without spaces (0x21..0x7e); at most 64 distinct
 * names per capture, numbered in first-use order.  One sample per call: a counter emitted once per frame reads as
 * a per-frame value (ps3dk-prof counters prints it per frame).
 *
 * Results: OK; ENOTINIT before init; ESTOPPED once shutdown has begun; EBUSY from inside a drain or sink callback
 * (counted as refused); EPERM on a thread that has no live incarnation (refused); EINVAL for a bad name (refused);
 * EFULL when the name table is full (refused) or the sample buffer is full until the next drain (dropped).  Refused
 * and dropped calls are counted in the capture.  The call takes one mutex (it is not lock-free); it never blocks on
 * the sink. */
#ifndef PS3DK_PROF_COUNTER_H
#define PS3DK_PROF_COUNTER_H

#include <stdint.h>

#include <ps3dk/prof.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3DK_PROF_COUNTER_NAMES_MAX 64u
#define PS3DK_PROF_COUNTER_NAME_MAX 64u
#define PS3DK_PROF_COUNTER_BUFFER 4096u /* samples held between two drains */

#if defined(PS3DK_PROF) && PS3DK_PROF
int ps3dk_prof_counter(const char *name, int64_t value);
#define PS3DK_PROF_COUNTER(name, value) ps3dk_prof_counter((name), (int64_t)(value))
#else
#define PS3DK_PROF_COUNTER(name, value) ((void)sizeof(name), (void)sizeof(value), PS3DK_PROF_OK)
#endif

#ifdef __cplusplus
}
#endif

#endif
