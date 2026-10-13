/*
 * PS3 Custom Toolchain - <sys/mempool.h>
 *
 * Simple fixed-block memory pool (sysPrxForUser imports, liblv2_stub): a
 * pool carved out of a caller-supplied chunk, handing out blocks of a
 * fixed size, guarded by an LV2 mutex + condition variable.  These are
 * imports, not inline syscalls — the module owns the implementation.
 *
 * The pool metadata is written by the 32-bit system module.  The three
 * size fields are size_t in that module's ABI; they are declared uint32_t
 * here so LP64 userland gets the same layout as ILP32 (the same rule as
 * pmem_total in <sys/vm.h>).  Layout: 40 bytes, 8-byte aligned.
 */

#ifndef PS3TC_SYS_MEMPOOL_H
#define PS3TC_SYS_MEMPOOL_H

#include <stdint.h>
#include <ppu-types.h>
#include <sys/synchronization.h>   /* sys_mutex_t / sys_cond_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef void *sys_mempool_t;

/* Written by the 32-bit system module; fixed layout across data models. */
typedef struct mempool_header {
	uint64_t     magic_number;
	uint64_t     free_count;
	uint32_t     chunk_size;
	sys_addr_t   first_free_block;
	uint32_t     block_size;
	uint32_t     offset;
	sys_mutex_t  mutex_id;
	sys_cond_t   cond_id;
} mempool_header;

extern int      sys_mempool_create(sys_mempool_t *mempool, void *chunk,
                                   const uint64_t chunk_size,
                                   const uint64_t block_size,
                                   const uint64_t ralignment);
extern void    *sys_mempool_allocate_block(sys_mempool_t mempool);
extern void    *sys_mempool_try_allocate_block(sys_mempool_t mempool);
extern int      sys_mempool_free_block(sys_mempool_t mempool, void *block);
extern void     sys_mempool_destroy(sys_mempool_t mempool);
extern uint64_t sys_mempool_get_count(sys_mempool_t mempool);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_MEMPOOL_H */
