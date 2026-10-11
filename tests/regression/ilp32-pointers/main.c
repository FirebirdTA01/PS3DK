/* Runtime half of the ILP32 pointer-forms regression (GCC patch 0059).  Run it
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
struct grid { long long seed; int x0, z0, cols, rows; const unsigned char *cells; };
int grid_at (const struct grid *g, int x, int z);
float vec_load_back (int back);
void vec_store_back (int back, float value);
struct sv { unsigned len; const char *p; };
char sv_first (struct sv s);
char sv_last (struct sv s);
unsigned long long ptr_to_u64 (const void *p);
int dense_switch (int k);
int import_via_pointer (char *buf);
int flood_mark (int i, int j, int first);
int class_measure (void);

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

  /* len 0xffffffff fills the high word of the packed register.  */
  static const char text[] = "pointer";
  struct sv s = { 0xffffffffu, text };
  check ("aggregate pointer member", sv_first (s), 'p');
  s.len = 7;
  check ("aggregate pointer and length", sv_last (s), 'r');

  char on_stack = 0;
  volatile uintptr_t high = 0x80000000u;
  check ("pointer to u64, bit 31 set",
	 ptr_to_u64 ((const void *) high) == 0x80000000ull, 1);
  check ("stack pointer to u64",
	 ptr_to_u64 (&on_stack) == (unsigned long long) (uintptr_t) &on_stack, 1);

  static const int switch_want[9] = { 31, 102, 37, 206, 41, 1000, 47, 53, -1 };
  for (int k = 0; k < 9; k++)
    check ("dense switch", dense_switch (k), switch_want[k]);

  char buf[8] = { 0 };
  check ("PRX import through a function pointer", import_via_pointer (buf),
	 1002 + 102 + 0x5a);

  /* Negative table offsets into an index: each marks one neighbour.  */
  check ("negative table offset x-1", flood_mark (13, 5, 1), 5 * 15 + 12);
  check ("negative table offset y-1", flood_mark (13, 5, 3), 4 * 15 + 13);
  check ("positive table offset x+1", flood_mark (13, 5, 0), 5 * 15 + 14);
  /* An int difference used as a size_t index from a negative origin. */
  {
    static const unsigned char cells[6] = { 10, 11, 12, 13, 14, 15 };
    struct grid g = { 0x1234, -37, -2, 3, 2, cells };
    check ("index from a negative origin, x == x0", grid_at (&g, -37, -2), 10);
    check ("index from a negative origin, last cell", grid_at (&g, -35, -1), 15);
  }
  /* A by-value std::string_view walked character by character.  */
  check ("string_view by value", class_measure (), 18);

  printf ("POINTERS %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
