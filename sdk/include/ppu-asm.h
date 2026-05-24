#ifndef __PPU_ASM_H__
#define __PPU_ASM_H__

#include <ppu_intrinsics.h>

#define PPU_ALIGNMENT			8

/* With compact 8-byte .opd descriptors, a C function pointer already
 * IS the 32-bit descriptor EA: bytes 0..3 = entry_ea, 4..7 = toc_ea.
 * Historically (24-byte ELFv1 + sprxlinker packing) the compact 8-byte
 * form was packed into the last 8 bytes of the 24-byte descriptor, so
 * this macro added 16 to step over {func,rtoc} into {packed-entry,toc}.
 * Now there is nothing to step over; return the pointer unchanged.
 * Preserve NULL. Keep the u64 return type so callers that stash the
 * value in registers before narrowing still compile. */
#define __get_opd32(opd64)		((unsigned long long)((intptr_t)(opd64) ? (intptr_t)(opd64) : 0L))

#define __get_addr32(addr)		(unsigned int)((unsigned long long)(addr))

#define __build_opd32(opd64,opd32) __extension__ \
	({register unsigned long long func,rtoc; \
	__asm__ __volatile__("ld %0,0(%2); stw %0,0(%3); ld %1,8(%2); stw %1,4(%3)" : "=&r"(func),"=&r"(rtoc) : "b"((opd64)), "b"((opd32)) : "memory"); \
	(u32)((u64)(opd32)); \
	})

#define __read8(addr) __extension__ \
	({register unsigned char result; \
	__asm__ __volatile__ ("lbz %0,0(%1); sync" : "=r"(result) : "b"((addr))); \
	result;})

#define __read16(addr) __extension__ \
	({register unsigned short result; \
	__asm__ __volatile__ ("lhz %0,0(%1); sync" : "=r"(result) : "b"((addr))); \
	result;})

#define __read32(addr) __extension__ \
	({register unsigned int result; \
	__asm__ __volatile__ ("lwz %0,0(%1); sync" : "=r"(result) : "b"((addr))); \
	result;})

#define __read64(addr) __extension__ \
	({register unsigned long long result; \
	__asm__ __volatile__ ("ld %0,0(%1); sync" : "=r"(result) : "b"((addr))); \
	result;})

#define __write8(addr,val) \
	__asm__ __volatile__("stb %0,0(%1); eieio" : : "r"((val)), "b"((addr)) : "memory")

#define __write16(addr,val) \
	__asm__ __volatile__("sth %0,0(%1); eieio" : : "r"((val)), "b"((addr)) : "memory")

#define __write32(addr,val) \
	__asm__ __volatile__("stw %0,0(%1); eieio" : : "r"((val)), "b"((addr)) : "memory")

#define __write64(addr,val) \
	__asm__ __volatile__("std %0,0(%1); eieio" : : "r"((val)), "b"((addr)) : "memory")

#define __gettime() __extension__ \
	({register unsigned long long tb; \
	__asm__ __volatile__("1: mftb %[current_tb]; cmpwi 7,%[current_tb],0; beq- 7,1b" : [current_tb] "=r"(tb) : : "cr7"); \
	tb;})

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

static inline unsigned short bswap16(unsigned short val)
{
	unsigned short tmp = val;
	return __lhbrx(&tmp);
}

static inline unsigned int bswap32(unsigned int val)
{
	unsigned int tmp = val;
	return __lwbrx(&tmp);
}

static inline unsigned long long bswap64(unsigned long long val)
{
	unsigned long long tmp = val;
	return __ldbrx(&tmp);
}

#ifdef __cplusplus
	}
#endif /* __cplusplus */

#endif
