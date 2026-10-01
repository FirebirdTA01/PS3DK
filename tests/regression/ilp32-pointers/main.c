/* Runtime half of the ILP32 pointer-forms regression (GCC patch 0051).  Run it
   under RPCS3's PPU INTERPRETER: the LLVM recompiler truncates a bit-32
   address and would hide the fault this row exists to catch.  */
#include <stdio.h>
#include <stdint.h>

int ext_table[64];

int *ext_at (int i);
int *ext_plus12 (void);
int *local_at (int i);
void fill_local (int base);
int sum_shifted (const int *p, int n);
int (*pick (int which)) (int);
int label_dispatch (int which);
int bump_tls (int n);
int *tls_slot (int i);
int label_runtime (int which);
extern float vec_table[16];
float vec_load_back (int back);
void vec_store_back (int back, float value);

static int failures;

static void
check (const char *what, long got, long want)
{
  if (got != want)
    {
      printf ("POINTERS FAIL %s: got %ld want %ld\n", what, got, want);
      failures++;
    }
}

int
main (void)
{
  for (int i = 0; i < 64; i++)
    ext_table[i] = 100 + i;
  fill_local (1000);

  check ("extern symbol", *ext_at (5), 105);
  check ("extern symbol+offset", *ext_plus12 (), 112);
  check ("extern address", (long) (uintptr_t) ext_at (7),
	 (long) (uintptr_t) &ext_table[7]);
  check ("local TOC-relative", *local_at (63), 1063);
  check ("shifted loop over extern", sum_shifted (ext_table, 64),
	 64 * 100 + 63 * 64 / 2);
  check ("shifted loop over local", sum_shifted (local_at (0), 10),
	 10 * 1000 + 9 * 10 / 2);
  check ("function pointer", pick (1) (21), 42);
  check ("label 0", label_dispatch (0), 10);
  check ("label 2", label_dispatch (2), 12);
  check ("runtime label 0", label_runtime (0), 20);
  check ("runtime label 2", label_runtime (2), 22);
  for (int i = 0; i < 16; i++)
    vec_table[i] = (float) (i * 10);
  check ("vec_ldl negative offset", (long) vec_load_back (16), 40);
  vec_store_back (32, 7.0f);
  check ("vec_stl negative offset", (long) vec_table[0], 7);
  check ("vec_stl left the rest", (long) vec_table[4], 40);
  check ("tls counter first", bump_tls (3), 3);
  check ("tls counter second", bump_tls (4), 7);
  *tls_slot (5) = 77;
  check ("tls array", *tls_slot (5), 77);

  printf ("POINTERS %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
