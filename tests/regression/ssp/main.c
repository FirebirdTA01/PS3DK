/* Runtime row for the PPU stack protector (GCC patch 0067).  Built with
   -fstack-protector, -fstack-protector-strong and -fstack-protector-all in
   both data models.  Before 0067 every protected function in ILP32 failed
   to compile ("unrecognizable insn").

   The probe defines __stack_chk_guard and __stack_chk_fail itself, so the
   C library's pair is not linked; the handler prints SSP_CHK_FAIL_CALLED,
   counts the call and longjmps back to main.
     1. normal return: protected functions (arrays, alloca, a VLA, a
        struct with an array, recursion) return the right values and the
        handler is never called;
     2. the guard changes while a protected frame is live: the epilogue
        check must fail and call the handler exactly once;
     3. an 8-byte overflow past a char[16] covers the canary (the frame
        grows downward under the protector, so character arrays sit just
        below it: directly in LP64, below 4 bytes of padding in ILP32,
        where the 4-byte slot is kept 8-byte aligned): the handler must be
        called exactly once.
   The guard value differs in every byte (and, in LP64, between its two
   words), so a check of the wrong width or the wrong word misses (2).
   Prints SSP_OK or SSP_FAIL <reason>.  */
#include <alloca.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if UINTPTR_MAX == 0xffffffffu
#define GUARD_VALUE ((uintptr_t) 0x5a3c96e1u)
#define ABI "ilp32"
#else
#define GUARD_VALUE ((uintptr_t) 0x5a3c96e1c3a50f78ull)
#define ABI "lp64"
#endif

uintptr_t __stack_chk_guard = GUARD_VALUE;
static jmp_buf back;
static volatile int fail_calls;

__attribute__ ((noreturn, no_stack_protector)) void
__stack_chk_fail (void)
{
  __stack_chk_guard = GUARD_VALUE;
  fail_calls++;
  printf ("SSP_CHK_FAIL_CALLED %d\n", fail_calls);
  longjmp (back, 1);
}

__attribute__ ((noinline)) void
use (void *p, unsigned n)
{
  __asm__ volatile ("" : : "r" (p), "r" (n) : "memory");
}

__attribute__ ((noinline, no_stack_protector)) void
bump_guard (void)
{
  __stack_chk_guard ^= (uintptr_t) 0x01010101u;
}

__attribute__ ((noinline)) int
sum_arr (int k)
{
  char b[64];
  int t = 0;
  for (int i = 0; i < 64; i++)
    b[i] = (char) (i + k);
  use (b, sizeof b);
  for (int i = 0; i < 64; i++)
    t += b[i];
  return t;
}

__attribute__ ((noinline)) int
sum_alloca (unsigned n)
{
  unsigned char *p = alloca (n);
  int t = 0;
  for (unsigned i = 0; i < n; i++)
    p[i] = (unsigned char) i;
  use (p, n);
  for (unsigned i = 0; i < n; i++)
    t += p[i];
  return t;
}

__attribute__ ((noinline)) int
sum_vla (unsigned n)
{
  short v[n];
  int t = 0;
  for (unsigned i = 0; i < n; i++)
    v[i] = (short) (3 * i);
  use (v, sizeof v);
  for (unsigned i = 0; i < n; i++)
    t += v[i];
  return t;
}

struct named { char name[16]; int v; };

__attribute__ ((noinline)) int
struct_len (const char *s)
{
  struct named t;
  strncpy (t.name, s, sizeof t.name - 1);
  t.name[sizeof t.name - 1] = 0;
  t.v = (int) strlen (t.name);
  use (&t, sizeof t);
  return t.v;
}

__attribute__ ((noinline)) int
fact (int n)
{
  char pad[24];
  use (pad, sizeof pad);
  return n <= 1 ? 1 : n * fact (n - 1);
}

/* (2): the guard changes under a live protected frame.  */
__attribute__ ((noinline)) int
guard_changes (void)
{
  char b[16];
  use (b, sizeof b);
  bump_guard ();
  return b[0];
}

/* (3): eight bytes past the array, written through a volatile pointer.
   0xa5 differs from every byte of the ILP32 guard and from all but one of
   the LP64 guard's.  */
__attribute__ ((noinline)) int
overflow_8 (void)
{
  char b[16];
  volatile char *p = b;
  use (b, sizeof b);
  for (int i = 16; i < 24; i++)
    p[i] = (char) 0xa5;
  return b[0];
}

int
main (void)
{
  volatile int bad = 0;   /* written between setjmp and longjmp */

  if (sum_arr (1) != 2080 || sum_alloca (100) != 4950 || sum_vla (10) != 135
      || struct_len ("canary") != 6 || fact (6) != 720)
    {
      printf ("SSP_FAIL " ABI ": normal-return values wrong\n");
      bad = 1;
    }
  if (fail_calls != 0)
    {
      printf ("SSP_FAIL " ABI ": handler called on the normal path\n");
      bad = 1;
    }
  printf ("ssp " ABI ": normal path done\n");

  if (setjmp (back) == 0)
    {
      guard_changes ();
      printf ("SSP_FAIL " ABI ": a changed guard was not detected\n");
      bad = 1;
    }
  else if (fail_calls != 1)
    {
      printf ("SSP_FAIL " ABI ": %d handler calls after the guard change\n", fail_calls);
      bad = 1;
    }
  else
    printf ("ssp " ABI ": changed guard detected\n");

  if (setjmp (back) == 0)
    {
      overflow_8 ();
      printf ("SSP_FAIL " ABI ": an overflow onto the canary was not detected\n");
      bad = 1;
    }
  else if (fail_calls != 2)
    {
      printf ("SSP_FAIL " ABI ": %d handler calls after the overflow\n", fail_calls);
      bad = 1;
    }
  else
    printf ("ssp " ABI ": overflow detected\n");

  if (!bad)
    printf ("SSP_OK " ABI "\n");
  return 0;
}
