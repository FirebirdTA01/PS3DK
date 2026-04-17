/*
 * sony_process_param.h — Sony reference SDK's .sys_proc_param layout.
 *
 * Clean-room header: describes the shape of the data Sony's PS3 loader
 * reads from the .sys_proc_param ELF section.  Authoritative source is
 * the reference SDK 475.001's <sys/process.h>; this header restates the
 * layout in our own words for a coverage/ABI-check sample so we aren't
 * stuck on PSL1GHT's 32-byte variant.
 *
 * Divergences from PSL1GHT's <sys/process.h> (as of 2026-04-17 install):
 *   - struct has 9 fields / 36 bytes (PSL1GHT = 8 / 32).  The trailing
 *     field is the "crash dump param address" — an address within this
 *     ELF of a weak symbol the loader calls on process crash.  If zero,
 *     the loader skips the callback.
 *   - version constant is 0x00330000 (firmware 3.30+ layout), not
 *     0x00009000.  The loader rejects mismatched version with a
 *     protocol error at SELF load time.
 *
 * Until a toolchain-level <sys/process.h> rewrite lands (PSL1GHT v3 RFC,
 * Phase 3 milestone), samples that need Sony-ABI correctness include
 * this header directly and use SONY_PROCESS_PARAM instead of
 * SYS_PROCESS_PARAM.  The identifiers here are intentionally namespaced
 * (SONY_*) so there is no collision with PSL1GHT headers.
 */

#ifndef SONY_PROCESS_PARAM_H
#define SONY_PROCESS_PARAM_H

#include <stdint.h>

#define SONY_PROCESS_PARAM_MAGIC                    0x13bcc5f6u
#define SONY_PROCESS_PARAM_VERSION_330_0            0x00330000u
#define SONY_PROCESS_PARAM_MALLOC_PAGE_SIZE_1M      0x00100000u
#define SONY_PROCESS_PARAM_PPC_SEG_DEFAULT          0x00000000u
#define SONY_PROCESS_PARAM_SDK_VERSION_UNKNOWN      0xffffffffu

typedef struct sony_process_param {
	uint32_t size;                   /* sizeof(this struct), = 36 */
	uint32_t magic;                  /* SONY_PROCESS_PARAM_MAGIC */
	uint32_t version;                /* _VERSION_330_0 */
	uint32_t sdk_version;            /* CELL_SDK_VERSION or _UNKNOWN */
	int32_t  primary_prio;           /* 1000 = normal */
	uint32_t primary_stacksize;      /* 4 KiB .. 1 MiB */
	uint32_t malloc_pagesize;        /* _MALLOC_PAGE_SIZE_1M typical */
	uint32_t ppc_seg;                /* _PPC_SEG_DEFAULT typical */
	uint32_t crash_dump_param_addr;  /* &__sys_process_crash_dump_param, or 0 */
} sony_process_param_t;

/*
 * crash_dump_param_addr: on Sony's SDK this is the address of a weak
 * crash-dump callback.  Compile-time initialisation of `(uint32_t)&sym`
 * is not a constant expression under ELFv1 PPC64 + modern GCC, because
 * function descriptors resolve at link time.  The sample therefore
 * sets this to 0 ("no callback"), which matches the runtime semantics
 * when the symbol is weak-unresolved.  A toolchain-level fix goes in
 * the eventual <sys/process.h> rewrite (emit the pointer via an .init
 * array stub or a ld script DEFINED directive).
 */

#define SONY_PROCESS_PARAM_SECTION \
	__attribute__((aligned(8), section(".sys_proc_param"), unused))

#define SONY_PROCESS_PARAM(prio, stack_size)                                  \
	sony_process_param_t __sys_process_param SONY_PROCESS_PARAM_SECTION = {   \
		sizeof(sony_process_param_t),                                         \
		SONY_PROCESS_PARAM_MAGIC,                                             \
		SONY_PROCESS_PARAM_VERSION_330_0,                                     \
		SONY_PROCESS_PARAM_SDK_VERSION_UNKNOWN,                               \
		(prio),                                                               \
		(stack_size),                                                         \
		SONY_PROCESS_PARAM_MALLOC_PAGE_SIZE_1M,                               \
		SONY_PROCESS_PARAM_PPC_SEG_DEFAULT,                                   \
		0u,                                                                   \
	}

#endif /* SONY_PROCESS_PARAM_H */
