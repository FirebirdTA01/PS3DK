/* Address forms the ILP32 compiler must get right with 32-bit pointers held
   zero-extended in 64-bit registers (GCC patch 0059): symbols reached
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
   zero-extended as a pointer would be (GCC patch 0059 builtin expanders).
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

/* A pointer member of a small aggregate passed by value: the caller packs
   len into the high word and p into the low word of one GPR, so the callee
   must not dereference the whole register.  */
struct sv { unsigned len; const char *p; };
char sv_first (struct sv s) { return s.p[0]; }
char sv_last (struct sv s) { return s.p[s.len - 1]; }

/* A pointer converted to a wider unsigned integer zero-extends: an address
   with bit 31 set (the stack, 0xd00xxxxx) must not become 0xffffffff....  */
unsigned long long ptr_to_u64 (const void *p) { return (unsigned long long) p; }

/* A dense switch compiled to a relative jump table.  Case bodies placed
   before the table give negative displacements, which must not carry into
   bit 32 of the branch target.  */
int
dense_switch (int k)
{
  switch (k)
    {
    case 0: return 31;
    case 1: return ext_table[1] + 1;
    case 2: return 37;
    case 3: return ext_table[3] * 2;
    case 4: return 41;
    case 5: return local_table[5] - 5;
    case 6: return 47;
    case 7: return 53;
    default: return -1;
    }
}

/* A PRX import called through a function pointer.  The import stub saves
   and restores the TOC itself with a 4-byte store at 40(r1), in the slot the
   caller saved the TOC to before its bctrl; the caller's reload must still
   give a valid TOC for the TOC-relative accesses that follow.  */
extern void *_sys_memset (void *, int, unsigned int);
void *(*volatile import_fn) (void *, int, unsigned int) = _sys_memset;

__attribute__ ((noinline)) int
toc_reader (void)
{
  return local_table[2] + ext_table[2];
}

int
import_via_pointer (char *buf)
{
  import_fn (buf, 0x5a, 8);
  return toc_reader () + buf[7];
}

/* A negative int read from a table and added into an array index: the
   table entry is loaded zero-extended (lwz), so before the address-model
   change the 64-bit sum carried into bit 32.  Same shape as a game's
   neighbour-cell flood fill.  */
static const int sides[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };

int
flood_mark (int i, int j, int first)
{
  unsigned char seen[15][15] = { { 0 } };
  seen[j + sides[first][1]][i + sides[first][0]] = 1;
  int n = 0, at = -1;
  for (int y = 0; y < 15; y++)
    for (int x = 0; x < 15; x++)
      if (seen[y][x])
        n++, at = y * 15 + x;
  return n == 1 ? at : -1;
}
