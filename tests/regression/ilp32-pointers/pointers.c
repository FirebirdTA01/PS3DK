/* Address forms the ILP32 compiler must get right with 32-bit pointers held
   zero-extended in 64-bit registers (GCC patch 0051): symbols reached
   through a TOC entry and directly TOC-relative, symbol+offset, function
   descriptors, label addresses (static table and run-time), thread-local
   data, and negative offsets in an IV-optimised loop.  Kept in
   its own unit so the caller's constants do not fold the addresses.  */
#include <stdint.h>
#include <altivec.h>

extern int ext_table[64];              /* defined in main.c: TOC entry */
static int local_table[64];            /* local: TOC-relative address */
static __thread int tls_counter;
__thread int tls_array[8];

int *ext_at (int i) { return &ext_table[i]; }
int *ext_plus12 (void) { return &ext_table[12]; }
int *local_at (int i) { return &local_table[i]; }

void
fill_local (int base)
{
  for (int i = 0; i < 64; i++)
    local_table[i] = base + i;
}

/* An IV-optimised loop reading one element below its index, the shape that
   used to spill a 32-bit bias and leak bit 32 into the address.  */
int
sum_shifted (const int *p, int n)
{
  int s = 0;
  for (int i = 1; i <= n; i++)
    s += p[i - 1];
  return s;
}

int twice (int x) { return 2 * x; }
int (*pick (int which)) (int) { return which ? twice : 0; }

int
label_dispatch (int which)
{
  static void *labels[] = { &&l0, &&l1, &&l2 };
  goto *labels[which];
l0:
  return 10;
l1:
  return 11;
l2:
  return 12;
}

int bump_tls (int n) { tls_counter += n; return tls_counter; }
int *tls_slot (int i) { return &tls_array[i]; }

/* A label address materialized at run time (not from a static table): the
   compiler must load it from a pointer-sized TOC entry.  */
int
label_runtime (int which)
{
  void *volatile target = &&r0;
  if (which == 1)
    target = &&r1;
  else if (which == 2)
    target = &&r2;
  goto *target;
r0:
  return 20;
r1:
  return 21;
r2:
  return 22;
}

/* AltiVec loads and stores with a NEGATIVE variable byte offset: the offset
   is a signed int and must be sign-extended into the 64-bit address, not
   zero-extended as a pointer would be (GCC patch 0051 builtin expanders).
   vec_ldl/vec_stl, not vec_ld/vec_st: the lvx/stvx address mask narrows the
   sum to 32 bits and hides a zero-extended offset.  */
float vec_table[16] __attribute__ ((aligned (16)));

float
vec_load_back (int back)
{
  vector float v = vec_ldl (-back, &vec_table[8]);
  return vec_extract (v, 0);
}

void
vec_store_back (int back, float value)
{
  vec_stl (vec_splats (value), -back, &vec_table[8]);
}
