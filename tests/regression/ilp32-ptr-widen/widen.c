/* The conversions under test (C).  Under the ILP32 data model a pointer is
   32 bits and the ABI zero-extends it (POINTERS_EXTEND_UNSIGNED), so a
   direct conversion to a 64-bit integer, signed or unsigned, yields the
   address with the upper half clear.  GCC patch 0029 keeps convert.cc from
   sign-extending it.  Separate unit, no LTO, so the calls cannot fold.  */
#include <stdint.h>

unsigned long long widen_u64_c (void *p) { return (unsigned long long) p; }
long long widen_s64_c (void *p) { return (long long) p; }
uint64_t widen_via_uintptr_c (void *p) { return (uint64_t) (uintptr_t) p; }
int64_t widen_via_intptr_c (void *p) { return (int64_t) (intptr_t) p; }
