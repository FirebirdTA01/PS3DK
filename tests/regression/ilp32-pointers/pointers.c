/* Address forms the ILP32 compiler must get right with 32-bit pointers held
   zero-extended in 64-bit registers (GCC patch 0051): symbols reached
   through a TOC entry and directly TOC-relative, symbol+offset, function
   descriptors, label addresses, thread-local data, negative offsets in an
   IV-optimised loop, and pointer arithmetic that wraps at 2^32.  Kept in
   its own unit so the caller's constants do not fold the addresses.  */
#include <stdint.h>

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

/* Pointer arithmetic is modulo 2^32: the result must not keep bit 32.  */
uintptr_t
wrap_add (uintptr_t base, int off)
{
  return (uintptr_t) ((char *) base + off);
}

uintptr_t
wrap_sub (uintptr_t a, uintptr_t b)
{
  return (uintptr_t) ((char *) a - (char *) b + (char *) 0x100);
}
