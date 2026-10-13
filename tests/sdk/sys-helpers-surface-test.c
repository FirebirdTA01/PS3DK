/* <sys/random_number.h>, <sys/spinlock.h> and <sys/mempool.h>: the eleven
 * sysPrxForUser imports this change ships.  sys-helpers-surface-test.sh
 * compiles this in C (gnu99, c11) and C++ (c++17), ILP32 and LP64, under
 * -Wall -Wextra -Werror; at -O2 each wrapper's section must carry an
 * R_PPC64_REL24 to its own import name and no sc.  A self-check plants an
 * inline (syscall) body for one of the imports and requires the import row
 * for that function to be the only failure.  The layout asserts run in
 * every compile mode. */
#include <stddef.h>
#include <sys/random_number.h>
#include <sys/spinlock.h>
#include <sys/mempool.h>

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#else
#define STATIC_ASSERT _Static_assert
#endif
#define NOINLINE __attribute__((noinline))

/* <sys/random_number.h> */
NOINLINE int v_sys_get_random_number(void *a, uint64_t n)
{ return sys_get_random_number(a, n); }

/* <sys/spinlock.h> */
NOINLINE void v_sys_spinlock_initialize(int *l) { sys_spinlock_initialize(l); }
NOINLINE void v_sys_spinlock_lock(int *l) { sys_spinlock_lock(l); }
NOINLINE int  v_sys_spinlock_trylock(int *l) { return sys_spinlock_trylock(l); }
NOINLINE void v_sys_spinlock_unlock(int *l) { sys_spinlock_unlock(l); }

/* <sys/mempool.h> */
NOINLINE int v_sys_mempool_create(sys_mempool_t *p, void *chunk, uint64_t cs,
                                  uint64_t bs, uint64_t ra)
{ return sys_mempool_create(p, chunk, cs, bs, ra); }
NOINLINE void *v_sys_mempool_allocate_block(sys_mempool_t p)
{ return sys_mempool_allocate_block(p); }
NOINLINE void *v_sys_mempool_try_allocate_block(sys_mempool_t p)
{ return sys_mempool_try_allocate_block(p); }
NOINLINE int v_sys_mempool_free_block(sys_mempool_t p, void *b)
{ return sys_mempool_free_block(p, b); }
NOINLINE void v_sys_mempool_destroy(sys_mempool_t p) { sys_mempool_destroy(p); }
NOINLINE uint64_t v_sys_mempool_get_count(sys_mempool_t p)
{ return sys_mempool_get_count(p); }

/* The pool header is written by the 32-bit system module.  The three size
 * fields are size_t in that module's ABI; they are declared uint32_t here
 * so LP64 userland gets the same layout as ILP32 (the same rule as
 * pmem_total in <sys/vm.h>).  Total: 40 bytes, 8-byte aligned. */
STATIC_ASSERT(sizeof(mempool_header) == 40, "mempool_header is 40 bytes");
STATIC_ASSERT(offsetof(mempool_header, chunk_size) == 16, "chunk_size at 16");
STATIC_ASSERT(offsetof(mempool_header, cond_id) == 36, "cond_id at 36");
/* sys_mempool_create writes a four-byte handle: the handle type is four
 * bytes in both data models, or an LP64 program would keep the module's
 * value in the high half of an eight-byte pointer. */
STATIC_ASSERT(sizeof(sys_mempool_t) == 4, "sys_mempool_t is the module's 4-byte handle");
