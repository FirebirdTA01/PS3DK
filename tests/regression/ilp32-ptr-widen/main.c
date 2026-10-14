/* Runtime half of the ILP32 pointer-widening regression (GCC patch 0029).
   For each address, a direct conversion of the pointer to a 64-bit integer
   (signed or unsigned) must give the address zero-extended; going through
   uintptr_t gives the same; going through intptr_t (a signed 32-bit integer
   in ILP32) gives the sign-extended value, so it is negative when bit 31 is
   set.  The faulty compiler sign-extends the direct conversions.  */
#include <stdio.h>
#include <stdint.h>

unsigned ptrwiden_count (void);
uint32_t ptrwiden_raw (unsigned i);
void *ptrwiden_make (unsigned i);
unsigned long long widen_u64_c (void *p);
long long widen_s64_c (void *p);
uint64_t widen_via_uintptr_c (void *p);
int64_t widen_via_intptr_c (void *p);
unsigned long long widen_u64_cxx (void *p);
long long widen_s64_cxx (void *p);
uint64_t widen_via_uintptr_cxx (void *p);
int64_t widen_via_intptr_cxx (void *p);

static int fails;

static void
check (const char *what, uint32_t raw, unsigned long long got, unsigned long long want)
{
  if (got != want)
    {
      printf ("PTRWIDEN %s 0x%08lx: got 0x%016llx want 0x%016llx\n",
	      what, (unsigned long) raw, got, want);
      fails++;
    }
}

int
main (void)
{
  unsigned n = ptrwiden_count ();
  printf ("PTRWIDEN start ptr=%u cases=%u\n", (unsigned) sizeof (void *), n);
  for (unsigned i = 0; i < n; i++)
    {
      uint32_t raw = ptrwiden_raw (i);
      void *p = ptrwiden_make (i);
      unsigned long long zext = raw;
      unsigned long long sext = (unsigned long long) (long long) (int32_t) raw;
      if (sizeof (void *) == 8)
	sext = zext;	/* LP64: intptr_t is 64-bit, nothing to extend */
      check ("u64 c", raw, widen_u64_c (p), zext);
      check ("s64 c", raw, (unsigned long long) widen_s64_c (p), zext);
      check ("via-uintptr c", raw, widen_via_uintptr_c (p), zext);
      check ("via-intptr c", raw, (unsigned long long) widen_via_intptr_c (p), sext);
      check ("u64 c++", raw, widen_u64_cxx (p), zext);
      check ("s64 c++", raw, (unsigned long long) widen_s64_cxx (p), zext);
      check ("via-uintptr c++", raw, widen_via_uintptr_cxx (p), zext);
      check ("via-intptr c++", raw, (unsigned long long) widen_via_intptr_cxx (p), sext);
    }
  printf ("PTRWIDEN %s (%d mismatches)\n", fails ? "FAIL" : "PASS", fails);
  return fails ? 1 : 0;
}
